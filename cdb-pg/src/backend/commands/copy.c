/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command.
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/copy.c,v 1.254 2005/11/03 17:11:34 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "cdb/cdbvars.h"
#include "cdb/cdblink.h"
#include "cdb/cdbcopy.h"
#include "cdb/cdbhash.h"
#include "cdb/cdbdispatchresult.h"


#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')


/*
 * Represents the different source/dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_FILE,					/* to/from file */
	COPY_OLD_FE,				/* to/from frontend (2.0 protocol) */
	COPY_NEW_FE					/* to/from frontend (3.0 protocol) */
} CopyDest;

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;

/*
 * This struct contains all the state variables used throughout a COPY
 * operation.  For simplicity, we use the same struct for all variants
 * of COPY, even though some fields are used in only some cases.
 *
 * A word about encoding considerations: encodings that are only supported on
 * the client side are those where multibyte characters may have second or
 * later bytes with the high bit not set.  When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character,
 * we must use the full pg_encoding_mblen() machinery to skip over
 * multibyte characters, else we might find a false match to a trailing
 * byte.  In supported server encodings, there is no possibility of
 * a false match, and it's faster to make useless comparisons to trailing
 * bytes than it is to invoke pg_encoding_mblen() to skip over them.
 * client_only_encoding is TRUE when we have to do it the hard way.
 */
typedef struct CopyStateData
{
	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_FILE */
	StringInfo	fe_msgbuf;		/* used if copy_dest == COPY_NEW_FE */
	bool		fe_copy;		/* true for all FE copy dests */
	bool		fe_eof;			/* true if detected end of copy data */
	EolType		eol_type;		/* EOL type of input */
	int			client_encoding;	/* remote side's character encoding */
	bool		need_transcoding;		/* client encoding diff from server? */
	bool		client_only_encoding;	/* encoding not valid on server? */
	uint64      processed;      /* # of tuples processed */
	
	/* parameters from the COPY command */
	Relation	rel;			/* relation to copy to or from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	bool		binary;			/* binary format? */
	bool		oids;			/* include OIDs? */
	bool		csv_mode;		/* Comma Separated Value format? */
	bool		header_line;	/* CSV header line? */
	char	   *null_print;		/* NULL marker string (server encoding!) */
	int			null_print_len; /* length of same */
	char	   *delim;			/* column delimiter (must be 1 byte) */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
	List	   *force_quote_atts;		/* integer list of attnums to FQ */
	List	   *force_notnull_atts;		/* integer list of attnums to FNN */
	
	/* these are just for error messages, see copy_in_error_callback */
	const char *cur_relname;	/* table name for error messages */
	int			cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	//const char *cur_attval;		/* current att value for error messages */
	
	/*
	 * These variables are used to reduce overhead in textual COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 */
	StringInfoData attribute_buf;
	
	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, convert it
	 * to server encoding there, and then extract the individual attribute
	 * fields into attribute_buf.  line_buf is preserved unmodified so that we
	 * can display it in error messages if appropriate.
	 */
	StringInfoData line_buf;
	bool		line_buf_converted;		/* converted to server encoding? */
	
	/*
	 * Finally, raw_buf holds raw data read from the data source (file or
	 * client connection).	CopyReadLine parses this data sufficiently to
	 * locate line boundaries, then transfers the data to line_buf and
	 * converts it.  Note: we guarantee that there is a \0 at
	 * raw_buf[raw_buf_len].
	 */
		
#define RAW_BUF_SIZE 65536 /* we palloc RAW_BUF_SIZE+1 bytes */
	/* NOTE: raw_buf in MPP is used with different logic than postgres COPY */
	char	    raw_buf[RAW_BUF_SIZE + 1];/* extra byte for '\0' */
	int			raw_buf_index;	/* next byte to process */
	bool		raw_buf_done;			/* finished processing the current buffer */
	//int			raw_buf_len;	/* total # of bytes stored */
	
	/* MPP specific variables */
	bool		is_copy_in;			/* copy in or out? */
	char		eol_ch[2];			/* The byte values of the 1 or 2 eol bytes */
	bool		escape_off;			/* treat backslashes as non-special? */
	int			last_hash_field;
	bool		line_done;			/* finished processing the whole line or stopped in the middle */
	bool		end_marker;
	char		*begloc;
	char		*endloc;
	bool		error_on_executor;  /* true if errors arrived from the executors COPY (QEs)*/
	StringInfoData  executor_err_context;  /* error context text from QE */

	
	/* for CSV format */
	bool		in_quote;
	bool		last_was_esc;
	bool		ready_to_send_rows; /* mainly used when we copy out with header */
	
	/* for TEXT format */
	bool		esc_in_prevbuf;		/* escape was last character of the data input buffer */
	bool		cr_in_prevbuf;		/* CR was last character of the data input buffer */
	/* end MPP specific variables */
	
} CopyStateData;

typedef CopyStateData *CopyState;

static const char BinarySignature[] = "PGCOPY\n\377\r\n";   /* 11 bytes total */

/* non-export function prototypes */
static void DoCopyTo(CopyState cstate);
static void CopyToDispatch(CopyState cstate);
static void CopyToExec(CopyState cstate);
static void CopyFromDispatch(CopyState cstate);
static void CopyFromExec(CopyState cstate);
static bool CopyReadLineText(CopyState cstate, size_t bytesread);
static bool CopyReadLineCSV(CopyState cstate, size_t bytesread);
static void CopyReadAttributesText(CopyState cstate, char *nulls,
			  int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr);
static void CopyReadAttributesCSV(CopyState cstate, char *nulls,
			 int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr);
static Datum CopyReadBinaryAttribute(CopyState cstate, int column_no, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod, bool *isnull);
static char *CopyReadOidAttr(CopyState cstate, bool *isnull);
static void CopyAttributeOutText(CopyState cstate, char *server_string);
static void CopyAttributeOutCSV(CopyState cstate, char *server_string, bool use_quote, bool single_attr);
static List *CopyGetAttnums(Relation rel, List *attnamelist);
static void limit_printout_length(StringInfo buf);
static bool DetectLineEnd(CopyState cstate, size_t bytesread);

/* Low-level communications functions */
static void SendCopyBegin(CopyState cstate);
static void ReceiveCopyBegin(CopyState cstate);
static void SendCopyEnd(CopyState cstate);
static void CopySendData(CopyState cstate, const void *databuf, int datasize);
static void CopySendString(CopyState cstate, const char *str);
static void CopySendChar(CopyState cstate, char c);
static void CopySendEndOfRow(CopyState cstate);
static int	CopyGetData(CopyState cstate, void *databuf, int datasize);
static void CopySendInt32(CopyState cstate, int32 val);
static bool CopyGetInt32(CopyState cstate, int32 *val);
static void CopySendInt16(CopyState cstate, int16 val);
static bool CopyGetInt16(CopyState cstate, int16 *val);

/* byte scaning utils */
static char *scanTextLine(CopyState cstate, const char *s, char c, size_t len);
static char *scanCSVLine(CopyState cstate, const char *s, char c1, char c2, char c3, size_t len);

static void truncateEol(CopyState cstate);
static void concatenateEol(CopyState cstate);

/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 */
static void
SendCopyBegin(CopyState cstate)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int			natts = list_length(cstate->attnumlist);
		int16		format = (cstate->binary ? 1 : 0);
		int			i;
		
		pq_beginmessage(&buf, 'H');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		cstate->copy_dest = COPY_NEW_FE;
		cstate->fe_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('H');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		cstate->copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('B');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		cstate->copy_dest = COPY_OLD_FE;
	}
}

static void
ReceiveCopyBegin(CopyState cstate)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int			natts = list_length(cstate->attnumlist);
		int16		format = (cstate->binary ? 1 : 0);
		int			i;
		
		pq_beginmessage(&buf, 'G');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		cstate->copy_dest = COPY_NEW_FE;
		cstate->fe_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('G');
		cstate->copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('D');
		cstate->copy_dest = COPY_OLD_FE;
	}
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

static void
SendCopyEnd(CopyState cstate)
{
	if (cstate->copy_dest == COPY_NEW_FE)
	{
		if (cstate->binary)
		{
			/* Need to flush out file trailer word */
			CopySendEndOfRow(cstate);
		}
		else
		{
			/* Shouldn't have any unsent data */
			Assert(cstate->fe_msgbuf->len == 0);
		}
		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		/* The FE/BE protocol uses \n as newline for all platforms */
		CopySendData(cstate, "\\.\n", 3);
		pq_endcopyout(false);
	}
}


/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *
 * NB: no data conversion is applied by these functions
 *----------
 */
static void
CopySendData(CopyState cstate, const void *databuf, int datasize)
{
	if(!cstate->is_copy_in) /* copy out */
	{
		switch (cstate->copy_dest)
		{
			case COPY_FILE:
				fwrite(databuf, datasize, 1, cstate->copy_file);
				if (ferror(cstate->copy_file))
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not write to COPY file: %m")));
				break;
			case COPY_OLD_FE:
				if (pq_putbytes((char *) databuf, datasize))
				{
					/* no hope of recovering connection sync, so FATAL */
					ereport(FATAL,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("connection lost during COPY to stdout")));
				}
				break;
			case COPY_NEW_FE:
				appendBinaryStringInfo(cstate->fe_msgbuf, (char *) databuf, datasize);
				
				/*
				 * in the dispatcher we receive chunks of whole rows with row endings.
				 * We don't want to use CopySendEndOfRow() b/c it adds row endings and
				 * also b/c it's intended for a single row at a time. Therefore we need
				 * to flush the data buf here instead.
				 *
				 * we must make sure that ready_to_send_rows is set since in the case
				 * of 'copy to stdout with csv header' we need to send the header with	
				 * CopySendEndOfRow() and therefore skip sending each part of the header
				 * here.
				 */
				if (Mpp_role == MPP_ROLE_DISPATCH && cstate->ready_to_send_rows)
				{
					/* Dump the accumulated rows/data as one CopyData message */
					(void) pq_putmessage('d', cstate->fe_msgbuf->data, cstate->fe_msgbuf->len);
					/* Reset fe_msgbuf to empty */
					cstate->fe_msgbuf->len = 0;
					cstate->fe_msgbuf->data[0] = '\0';		
				}
				
				break;
		}
	}
	else /* copy in */
	{
		/* we call copySendData in copy-in to handle results
		 * of default functions that we wish to send from the
		 * dispatcher to the executor primary and mirror segments.
		 * we do so by concatenating the results to line buffer.
		 */
		appendBinaryStringInfo(&cstate->line_buf, (char *) databuf, datasize);
	}
}

static void
CopySendString(CopyState cstate, const char *str)
{
	CopySendData(cstate, (void *) str, strlen(str));
}

static void
CopySendChar(CopyState cstate, char c)
{
	CopySendData(cstate, &c, 1);
}

static void
CopySendEndOfRow(CopyState cstate)
{
	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			if (!cstate->binary)
			{
				/* Default line termination depends on platform */
#ifndef WIN32
				CopySendChar(cstate, '\n');
#else
				CopySendString(cstate, "\r\n");
#endif
			}
			break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', cstate->fe_msgbuf->data, cstate->fe_msgbuf->len);
			/* Reset fe_msgbuf to empty */
			cstate->fe_msgbuf->len = 0;
			cstate->fe_msgbuf->data[0] = '\0';
			break;
	}
}

/*
 * CopyGetData reads data from the source (file or frontend)
 * CopyGetChar does the same for single characters
 *
 * CopyGetEof checks if EOF was detected by previous Get operation.
 *
 * Note: when copying from the frontend, we expect a proper EOF mark per
 * protocol; if the frontend simply drops the connection, we raise error.
 * It seems unwise to allow the COPY IN to complete normally in that case.
 *
 * NB: no data conversion is applied by these functions
 *
 * Returns: the number of bytes that were successfully read
 * into the data buffer.
 */
static int
CopyGetData(CopyState cstate, void *databuf, int datasize)
{
	size_t		bytesread = 0;

	switch (cstate->copy_dest)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, datasize, cstate->copy_file);
			if (feof(cstate->copy_file))
				cstate->fe_eof = true;
			break;
		case COPY_OLD_FE:
			if (pq_getbytes((char *) databuf, datasize))
			{
				/* Only a \. terminator is legal EOF in old protocol */
				ereport(ERROR,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("unexpected EOF on client connection")));
			}
			bytesread += datasize;		/* update the count of bytes that were
										 * read so far */
			break;
		case COPY_NEW_FE:
			while (datasize > 0 && !cstate->fe_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;

			readmessage:
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
					if (pq_getmessage(cstate->fe_msgbuf, 0))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("unexpected EOF on client connection")));
					switch (mtype)
					{
						case 'd':		/* CopyData */
							break;
						case 'c':		/* CopyDone */
							/* COPY IN correctly terminated by frontend */
							cstate->fe_eof = true;
							return bytesread;
						case 'f':		/* CopyFail */
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case 'H':		/* Flush */
						case 'S':		/* Sync */

							/*
							 * Ignore Flush/Sync for the convenience of
							 * client libraries (such as libpq) that may
							 * send those without noticing that the
							 * command they just sent was COPY.
							 */
							goto readmessage;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							break;
					}
				}
				avail = cstate->fe_msgbuf->len - cstate->fe_msgbuf->cursor;
				if (avail > datasize)
					avail = datasize;
				pq_copymsgbytes(cstate->fe_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				bytesread += avail;		/* update the count of bytes that were
										 * read so far */
				datasize -= avail;
			}
			break;
	}

	return bytesread;
}


/*
 * These functions do apply some data conversion
 */

/*
 * CopySendInt32 sends an int32 in network byte order
 */
static void
CopySendInt32(CopyState cstate, int32 val)
{
	uint32		buf;

	buf = htonl((uint32) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 *
 * Returns true if OK, false if EOF
 */
static bool
CopyGetInt32(CopyState cstate, int32 *val)
{
	uint32		buf;
	
	if (CopyGetData(cstate, &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int32) ntohl(buf);
	return true;
}

/*
 * CopySendInt16 sends an int16 in network byte order
 */
static void
CopySendInt16(CopyState cstate, int16 val)
{
	uint16		buf;

	buf = htons((uint16) val);
	CopySendData(cstate, &buf, sizeof(buf));
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 */
static bool
CopyGetInt16(CopyState cstate, int16 *val)
{
	uint16		buf;
	
	if (CopyGetData(cstate, &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* suppress compiler warning */
		return false;
	}
	*val = (int16) ntohs(buf);
	return true;
}


/*
 *	 DoCopy executes the SQL COPY statement.
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = TRUE means we are inserting into the table.)
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.	Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Iff <binary>, unload or reload in the binary format, as opposed to the
 * more wasteful but more robust and portable text format.
 *
 * Iff <oids>, unload or reload the format that includes OID information.
 * On input, we accept OIDs whether or not the table has an OID column,
 * but silently drop them if it does not.  On output, we report an error
 * if the user asks for OIDs in a table that has none (not providing an
 * OID column might seem friendlier, but could seriously confuse programs).
 *
 * If in the text format, delimit columns with delimiter <delim> and print
 * NULL values as <null_print>.
 *
 * When loading in the text format from an input stream (as opposed to
 * a file), recognize a "." on a line by itself as EOF. Also recognize
 * a stream EOF.  When unloading in the text format to an output stream,
 * write a "." on a line by itself at the end of the data.
 *
 * Do not allow a Postgres user without superuser privilege to read from
 * or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table.
 */
uint64
DoCopy(const CopyStmt *stmt)
{		
	CopyState	cstate;
	RangeVar   *relation = stmt->relation;
	char	   *filename = stmt->filename;
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	List	   *attnamelist = stmt->attlist;
	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;
	ListCell   *option;
	uint64      processed;

	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "binary") == 0)
		{
			if (cstate->binary)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (cstate->oids)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (cstate->delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (cstate->null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->null_print = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "csv") == 0)
		{
			if (cstate->csv_mode)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->csv_mode = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (cstate->header_line)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->header_line = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (cstate->quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->quote = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (cstate->escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cstate->escape = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (force_quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			force_quote = (List *) defel->arg;
		}
		else if (strcmp(defel->defname, "force_notnull") == 0)
		{
			if (force_notnull)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			force_notnull = (List *) defel->arg;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (cstate->binary && cstate->delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DELIMITER in BINARY mode")));

	if (cstate->binary && cstate->csv_mode)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify CSV in BINARY mode")));

	if (cstate->binary && cstate->null_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify NULL in BINARY mode")));

	/* Set defaults */
	
	cstate->escape_off = false;
	
	if (!cstate->delim)
		cstate->delim = cstate->csv_mode ? "," : "\t";

	if (!cstate->null_print)
		cstate->null_print = cstate->csv_mode ? "" : "\\N";

	if (cstate->csv_mode)
	{
		if (!cstate->quote)
			cstate->quote = "\"";
		if (!cstate->escape)
			cstate->escape = cstate->quote;
	}

	if (!cstate->binary && !cstate->csv_mode && !cstate->escape)
		cstate->escape = "\\";			/* default escape for text mode */

	/*
	 * Only single-character delimiter strings are supported.
	 */
	if (strlen(cstate->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single character")));

	/* Check header */
	if (!cstate->csv_mode && cstate->header_line)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY HEADER available only in CSV mode")));
	
	/*
	 * Check quote
	 */
	if (!cstate->csv_mode && cstate->quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote available only in CSV mode")));

	if (cstate->csv_mode && strlen(cstate->quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote must be a single character")));

	/*
	 * Check escape
	 */
	if (cstate->binary && cstate->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape is not available in binary mode")));

	if (cstate->csv_mode && strlen(cstate->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("COPY escape in CSV format must be a single character")));

	if (!cstate->csv_mode && !cstate->binary && strlen(cstate->escape) != 1)
	{
		if (!strcmp(cstate->escape, "OFF") || !strcmp(cstate->escape, "off"))
			cstate->escape_off = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY escape must be a single character, or [OFF/off] to disable escapes")));
	}

	/*
	 * Check force_quote
	 */
	if (!cstate->csv_mode && force_quote != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote available only in CSV mode")));
	if (force_quote != NIL && is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote only available using COPY TO")));

	/*
	 * Check force_notnull
	 */
	if (!cstate->csv_mode && force_notnull != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force not null available only in CSV mode")));
	if (force_notnull != NIL && !is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("COPY force not null only available using COPY FROM")));

	/*
	 * Don't allow the delimiter to appear in the null string.
	 */
	if (strchr(cstate->null_print, cstate->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("COPY delimiter must not appear in the NULL specification")));

	/*
	 * Don't allow the csv quote char to appear in the null string.
	 */
	if (cstate->csv_mode && strchr(cstate->null_print, cstate->quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSV quote character must not appear in the NULL specification")));

	/*
	 * Open and lock the relation, using the appropriate lock type.
     *
     * CDB: The proper lock mode depends on whether the relation is local or
     * distributed.  That info is obtained by heap_openrv().  Therefore we
     * use CdbLockRelation() instead of getting the lock from heap_openrv().
	 */
	cstate->rel = heap_openrv(relation, NoLock);

	/* check read-only transaction */
	if (XactReadOnly && is_from && !isTempNamespace(RelationGetNamespace(cstate->rel)))
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("transaction is read-only")));

	/* Check permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(cstate->rel), GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(cstate->rel));
	if (!pipe && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to COPY to or from a file"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));

    CdbLockRelation(cstate->rel, (is_from ? RowExclusiveLock : AccessShareLock), false);

    /*
	 * Don't allow COPY w/ OIDs to or from a table without them
	 */
	if (cstate->oids && !cstate->rel->rd_rel->relhasoids)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("table \"%s\" does not have OIDs",
						RelationGetRelationName(cstate->rel))));

	/*
	 * Generate or convert list of attributes to process
	 */
	cstate->attnumlist = CopyGetAttnums(cstate->rel, attnamelist);

	/*
	 * Check that FORCE QUOTE references valid COPY columns
	 */
	if (force_quote)
	{
		TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		ListCell   *cur;

		cstate->force_quote_atts = CopyGetAttnums(cstate->rel, force_quote);

		foreach(cur, cstate->force_quote_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				   errmsg("FORCE QUOTE column \"%s\" not referenced by COPY",
						  NameStr(attr[attnum - 1]->attname))));
		}
	}

	/*
	 * Check that FORCE NOT NULL references valid COPY columns
	 */
	if (force_notnull)
	{
		ListCell   *cur;
		TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
		Form_pg_attribute *attr = tupDesc->attrs;

		cstate->force_notnull_atts = CopyGetAttnums(cstate->rel, force_notnull);

		foreach(cur, cstate->force_notnull_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				errmsg("FORCE NOT NULL column \"%s\" not referenced by COPY",
					   NameStr(attr[attnum - 1]->attname))));
		}
	}

	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->line_buf_converted = false;
	cstate->processed = 0;

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to
	 * validate data in multibyte encodings.
	 */
	cstate->client_encoding = pg_get_client_encoding();
	cstate->need_transcoding =
		(cstate->client_encoding != GetDatabaseEncoding() ||
		 pg_database_encoding_max_length() > 1);
	cstate->client_only_encoding = PG_ENCODING_IS_CLIENT_ONLY(cstate->client_encoding);

	cstate->copy_dest = COPY_FILE;		/* default */
	cstate->copy_file = NULL;
	cstate->fe_msgbuf = NULL;
	cstate->fe_eof = false;

	if (is_from)
	{							/* copy from file to database */
		cstate->is_copy_in = true;
		cstate->error_on_executor = false;
		initStringInfo(&(cstate->executor_err_context));
		
		if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy to view \"%s\"",
								RelationGetRelationName(cstate->rel))));
			else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy to sequence \"%s\"",
								RelationGetRelationName(cstate->rel))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy to non-table relation \"%s\"",
								RelationGetRelationName(cstate->rel))));
		}
		if (pipe)
		{
			if (whereToSendOutput == DestRemote)
				ReceiveCopyBegin(cstate);
			else
				cstate->copy_file = stdin;
		}
		else
		{
			struct stat st;

			cstate->copy_file = AllocateFile(filename, PG_BINARY_R);

			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								filename)));

			fstat(fileno(cstate->copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(cstate->copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		if (Mpp_role == MPP_ROLE_DISPATCH)
			CopyFromDispatch(cstate);
		
		else
			/* execute or utility mode */
			CopyFromExec(cstate);
	}
	else
	{							/* copy from database to file */
		cstate->is_copy_in = false;

		if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from view \"%s\"",
								RelationGetRelationName(cstate->rel))));
			else if (cstate->rel->rd_rel->relkind == RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from sequence \"%s\"",
								RelationGetRelationName(cstate->rel))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from non-table relation \"%s\"",
								RelationGetRelationName(cstate->rel))));
		}
		if (pipe)
		{
			if (whereToSendOutput == DestRemote)
				cstate->fe_copy = true;
			else
				cstate->copy_file = stdout;
		}
		else
		{
			mode_t		oumask; /* Pre-existing umask value */
			struct stat st;

			/*
			 * Prevent write to relative path ... too easy to shoot
			 * oneself in the foot by overwriting a database file ...
			 */
			if (!is_absolute_path(filename))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
					  errmsg("relative path not allowed for COPY to file")));

			oumask = umask((mode_t) 022);
			cstate->copy_file = AllocateFile(filename, PG_BINARY_W);
			umask(oumask);

			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for writing: %m",
								filename)));

			fstat(fileno(cstate->copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(cstate->copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		DoCopyTo(cstate);
	}

	if (!pipe)
	{
		/* we assume only the write case could fail here */
		if (FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							filename)));
	}

	/*
	 * Close the relation.	If reading, we can release the AccessShareLock
	 * we got; if writing, we should hold the lock until end of
	 * transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	heap_close(cstate->rel, (is_from ? NoLock : AccessShareLock));

	/* Clean up storage (probably not really necessary) */
	processed = cstate->processed;

	pfree(cstate->attribute_buf.data);
	pfree(cstate->line_buf.data);
	pfree(cstate);

	return processed;
}


/*
 * This intermediate routine just exists to localize the effects of setjmp
 * so we don't need to plaster a lot of variables with "volatile".
 */
static void
DoCopyTo(CopyState cstate)
{
	cstate->ready_to_send_rows = true;
	
	PG_TRY();
	{
		if (cstate->fe_copy)
			SendCopyBegin(cstate);

		if (Mpp_role == MPP_ROLE_DISPATCH)
			CopyToDispatch(cstate);		
		else
			CopyToExec(cstate);
		
		if (cstate->fe_copy)
			SendCopyEnd(cstate);
	}
	PG_CATCH();
	{
		/*
		 * Make sure we turn off old-style COPY OUT mode upon error. It is
		 * okay to do this in all cases, since it does nothing if the mode
		 * is not on.
		 */
		pq_endcopyout(true);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Copy from relation TO file. This method will be executed on each
 * executor or in utility mode.
 */
static void
CopyToExec(CopyState cstate)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	HeapScanDesc scandesc;
	int			num_phys_attrs;
	int			attr_count;
	Form_pg_attribute *attr;
	FmgrInfo   *out_functions;
	bool	   *force_quote;
	Oid		   *typioparams;
	bool	   *isvarlena;
	char	   *string;
	ListCell   *cur;
	MemoryContext oldcontext;
	MemoryContext mycontext;

	tupDesc = cstate->rel->rd_att;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);

	/*
	 * Get info about the columns we need to process.
	 */
	out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	isvarlena = (bool *) palloc(num_phys_attrs * sizeof(bool));
	force_quote = (bool *) palloc(num_phys_attrs * sizeof(bool));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;

		if (cstate->binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
									&out_func_oid,
									&isvarlena[attnum - 1]);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid,
							  &isvarlena[attnum - 1]);
		fmgr_info(out_func_oid, &out_functions[attnum - 1]);

		if (list_member_int(cstate->force_quote_atts, attnum))
			force_quote[attnum - 1] = true;
		else
			force_quote[attnum - 1] = false;
	}

	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks
	 * inside datatype output routines, and should be faster than retail
	 * pfree's anyway.  (We don't need a whole econtext as CopyFrom does.)
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "COPY TO",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * MPP: the code that prints a binary header or table header
	 * was removed from here. This is done on the dispatcher, once.
	 */
	
	
	if (cstate->binary)
	{
		/*
		 * We don't want binary header in exec mode. it's
		 * taken care of in the dispatcher code.
		 */
		if (Mpp_role == MPP_ROLE_UTILITY)
		{
			/* Generate header for a binary copy */
			int32		tmp;
			
			/* Signature */
			CopySendData(cstate, BinarySignature, sizeof(BinarySignature));
			/* Flags field */
			tmp = 0;
			if (cstate->oids)
				tmp |= (1 << 16);
			CopySendInt32(cstate, tmp);
			/* No header extension */
			tmp = 0;
			CopySendInt32(cstate, tmp);
		}
	}
	else
	{
		/*
		 * For non-binary copy, we need to convert null_print to client
		 * encoding, because it will be sent directly with CopySendString.
		 */
		if (cstate->need_transcoding)
			cstate->null_print = (char *)
				pg_server_to_client(cstate->null_print,
									strlen(cstate->null_print));
		
		/* if a header has been requested send the line */
		if (cstate->header_line)
		{
			/*
			 * header is printed by dispatcher copy. we only
			 * want it printed here if we are in utility mode.
			 */
			if (Mpp_role == MPP_ROLE_UTILITY)
			{
			bool		hdr_delim = false;

				foreach(cur, cstate->attnumlist)
				{
					int			attnum = lfirst_int(cur);
					char	   *colname;

					if (hdr_delim)
						CopySendChar(cstate, cstate->delim[0]);
					hdr_delim = true;

					colname = NameStr(attr[attnum - 1]->attname);

					CopyAttributeOutCSV(cstate, colname, false,
										list_length(cstate->attnumlist) == 1);
				}

				CopySendEndOfRow(cstate);
			}
		}
	}
	
	scandesc = heap_beginscan(cstate->rel, ActiveSnapshot, 0, NULL);

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		bool		need_delim = false;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(mycontext);
		oldcontext = MemoryContextSwitchTo(mycontext);

		if (cstate->binary)
		{
			/* Binary per-tuple header */
			CopySendInt16(cstate, attr_count);
			/* Send OID if wanted --- note attr_count doesn't include it */
			if (cstate->oids)
			{
				Oid			oid = HeapTupleGetOid(tuple);

				/* Hack --- assume Oid is same size as int32 */
				CopySendInt32(cstate, sizeof(int32));
				CopySendInt32(cstate, oid);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			if (cstate->oids)
			{
				string = DatumGetCString(DirectFunctionCall1(oidout,
								  ObjectIdGetDatum(HeapTupleGetOid(tuple))));
				CopySendString(cstate, string);
				need_delim = true;
			}
		}

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			Datum		value;
			bool		isnull;

			value = heap_getattr(tuple, attnum, tupDesc, &isnull);

			if (!cstate->binary)
			{
				if (need_delim)
					CopySendChar(cstate, cstate->delim[0]);
				need_delim = true;
			}

			if (isnull)
			{
				if (!cstate->binary)
					CopySendString(cstate, cstate->null_print); /* null indicator */
				else
					CopySendInt32(cstate, -1);	/* null marker */
			}
			else
			{
				if (!cstate->binary)
				{
					string = DatumGetCString(FunctionCall3(&out_functions[attnum - 1],
														   value,
								   ObjectIdGetDatum(typioparams[attnum - 1]),
								Int32GetDatum(attr[attnum - 1]->atttypmod)));
					if (cstate->csv_mode)
					{
						CopyAttributeOutCSV(cstate, string,
											force_quote[attnum - 1],
											list_length(cstate->attnumlist) == 1);
					}
					else
						CopyAttributeOutText(cstate, string);
				}
				else
				{
					bytea	   *outputbytes;

					outputbytes = DatumGetByteaP(FunctionCall2(&out_functions[attnum - 1],
															   value,
								 ObjectIdGetDatum(typioparams[attnum - 1])));
					/* We assume the result will not have been toasted */
					CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
					CopySendData(cstate, VARDATA(outputbytes),
								 VARSIZE(outputbytes) - VARHDRSZ);
				}
			}
		}

		CopySendEndOfRow(cstate);

		MemoryContextSwitchTo(oldcontext);
		
		/* note: this counter in here is only needed for utility mode */
		cstate->processed++;
	}

	heap_endscan(scandesc);

	
	if (cstate->binary)
	{
		if (Mpp_role == MPP_ROLE_UTILITY)
			/* Generate trailer for a binary copy */
			CopySendInt16(cstate, -1);
	}
	

	MemoryContextDelete(mycontext);

	pfree(out_functions);
	pfree(typioparams);
	pfree(isvarlena);
	pfree(force_quote);
}

/*
 * Copy from relation TO file. Starts a COPY TO command on each of
 * the executors and gathers all the results and writes it out.
 */
static void
CopyToDispatch(CopyState cstate)
{
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	int			attr_count;
	Form_pg_attribute *attr;
	ListCell   *cur;
	CdbCopy    *cdbCopy;
	StringInfoData cdbcopy_err;
	StringInfoData cdbcopy_cmd;
	bool		is_first_col;
	
	tupDesc = cstate->rel->rd_att;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);

	/*
	 * prepare to get COPY data from segDBs:
	 * 1 - re-construct the orignial COPY command sent from the client.
	 * 2 - execute a BEGIN DTM transaction.
	 * 3 - send the COPY command to all segment databases.
	 */
	
	cdbCopy = makeCdbCopy(false);
	
	/* allocate memory for error and copy strings */
	initStringInfo(&cdbcopy_err);
	initStringInfo(&cdbcopy_cmd);
	
	/* append schema and tablename */
	appendStringInfo(&cdbcopy_cmd, "COPY %s.%s",
					 quote_identifier(get_namespace_name(RelationGetNamespace(cstate->rel))),
					 quote_identifier(RelationGetRelationName(cstate->rel)));
	/*
	 * append column list. NOTE: if not specified originally, attnumlist will
	 * include all non-dropped columns of the table by default
	 */
	if(num_phys_attrs > 0) /* don't append anything for zero column table */
	{

		appendStringInfo(&cdbcopy_cmd, "(");
		is_first_col = true;
		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;
			
			/* We don't add dropped attributes */
			if (attr[m]->attisdropped)
				continue;
			
			if (is_first_col)
				appendStringInfo(&cdbcopy_cmd, "%s", quote_identifier(NameStr(attr[m]->attname)));
			else
				appendStringInfo(&cdbcopy_cmd, ",%s", quote_identifier(NameStr(attr[m]->attname)));
			
			is_first_col = false;
		}

		appendStringInfo(&cdbcopy_cmd, ")");
	}
	
	appendStringInfo(&cdbcopy_cmd, " TO STDOUT WITH");
	
	if (cstate->binary)
	{
		appendStringInfo(&cdbcopy_cmd, " BINARY");
	}
	else
	{
		appendStringInfo(&cdbcopy_cmd, " DELIMITER AS '%s'", cstate->delim);
		
		if (cstate->null_print[0] == '\\')		/* null value is de-escaped.
			* re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " NULL AS '\\%s'", cstate->null_print);
		else
			appendStringInfo(&cdbcopy_cmd, " NULL AS '%s'", cstate->null_print);
		
		if (!cstate->csv_mode)
		{
			if (strcmp(cstate->escape, "\\") != 0 && !cstate->csv_mode) /* if default ("\")
					   * leave expression out */
				appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '%s'", cstate->escape);
		}
		
	}
	
	if (cstate->csv_mode)
	{
		appendStringInfo(&cdbcopy_cmd, " CSV");
		
		/* if it's a single quote de-escape it. else keep the way it is. */
		if (cstate->quote[0] == '\'')  /* quote value is de-escaped. re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " QUOTE AS ''''");
		else
			appendStringInfo(&cdbcopy_cmd, " QUOTE AS '%s'", cstate->quote);
			
		/* if it's a single quote or backslash de-escape it. else keep the way it is */
		if (cstate->escape[0] == '\'')
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS ''''");
		else if (cstate->escape[0] == '\\')	/* escape value is de-escaped. re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '\\%s'", cstate->escape);
		else
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '%s'", cstate->escape);
					
		/* do NOT include HEADER. Header row is generated by dispatcher COPY only  */
	}

	/*
	 * start a transaction
	 */
	//cdbCopyStartTransaction();
	
	/*
	 * Start a COPY command in every db of every segment in MPP.
	 *
	 * From this point in the code we need to be extra careful
	 * about error handling. ereport() must not be called until
	 * the COPY command sessions are closed on the executors.
	 * Calling ereport() will leave the executors hanging in
	 * COPY state.
	 */
	elog(DEBUG5, "COPY command sent to segdbs: %s", cdbcopy_cmd.data);
	if (cdbCopyStart(cdbCopy, cdbcopy_cmd.data) == -1)
	{
		/* get error message from CopyStart */
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
		
		/* TODO: end COPY in all the segdbs in progress */
		//cdbCopyEnd(cdbCopy);
		
		/* get error message from CopyEnd */
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
		
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("%s. Closing all other existing COPY sessions.",
						cdbcopy_err.data)));
	}
	
	if (cstate->binary)
	{
		/* Generate header for a binary copy */
		int32		tmp;
		
		/* Signature */
		CopySendData(cstate, BinarySignature, sizeof(BinarySignature));
		/* Flags field */
		tmp = 0;
		if (cstate->oids)
			tmp |= (1 << 16);
		CopySendInt32(cstate, tmp);
		/* No header extension */
		tmp = 0;
		CopySendInt32(cstate, tmp);
	}
	else
	{
		/* if a header has been requested send the line */
		if (cstate->header_line)
		{
			bool		hdr_delim = false;

			/*
			 * For non-binary copy, we need to convert null_print to client
			 * encoding, because it will be sent directly with CopySendString.
			 *
			 * MPP: in here we only care about this if we need to print the
			 * header. We rely on the segdb server copy out to do the conversion
			 * before sending the data rows out. We don't need to repeat it here
			 */
			if (cstate->need_transcoding)
				cstate->null_print = (char *)
					pg_server_to_client(cstate->null_print,
										strlen(cstate->null_print));
			
			/* 
			 * tell CopySendData that we are not ready with complete rows yet b/c
			 * we send the header on an attribute (not row) granularity.
			 */
			cstate->ready_to_send_rows = false;
			
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colname;

				if (hdr_delim)
					CopySendChar(cstate, cstate->delim[0]);
				hdr_delim = true;

				colname = NameStr(attr[attnum - 1]->attname);

				CopyAttributeOutCSV(cstate, colname, false,
									list_length(cstate->attnumlist) == 1);
			}
			
			CopySendEndOfRow(cstate);

			/* header is done. we are now ready to send data rows */
			cstate->ready_to_send_rows = true; 
		}
	}
	
	/*
	 * This is the main work-loop. In here we keep collecting data from the
	 * COPY commands on the segdbs, until no more data is available. We
	 * keep writing data out a chunk at a time.
	 */
	while(true)
	{
		
		bool done;
		bool copy_cancel = (QueryCancelPending ? true : false);
		
		/* get a chunk of data rows from the QE's */
		done = cdbCopyGetData(cdbCopy, copy_cancel, &cstate->processed);
		
		/* send the chunk of data rows to destination (file or stdout) */
		if(cdbCopy->copy_out_buf.len > 0) /* conditional is important! */
			CopySendData(cstate, (void *) cdbCopy->copy_out_buf.data, cdbCopy->copy_out_buf.len);
		
		if(done)
		{
			if(cdbCopy->remote_data_err || cdbCopy->local_data_err || cdbCopy->io_errors)
				appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);

			break;
		}
		
	}
	
	/* we can throw the error now if QueryCancelPending was set previously */
	CHECK_FOR_INTERRUPTS();
	
	/*
	 * report all accumulated errors back to the client.
	 */
	if (cdbCopy->remote_data_err || cdbCopy->local_data_err)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("%s", cdbcopy_err.data)));
	if (cdbCopy->io_errors)
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("%s", cdbcopy_err.data)));
						
	if (cstate->binary)
	{
		/* Generate trailer for a binary copy */
		CopySendInt16(cstate, -1);
	}
	
	pfree(cdbcopy_cmd.data);
	pfree(cdbcopy_err.data);
	pfree(cdbCopy);
}


/*
 * error context callback for COPY FROM
 */
static void
copy_in_error_callback(void *arg)
{

	CopyState	cstate = (CopyState) arg;
	
	/*
	 * If we saved the error context from a QE in cdbcopy.c append it here.
	 */
	if(Mpp_role == MPP_ROLE_DISPATCH && cstate->executor_err_context.len > 0)
	{
		errcontext("%s", cstate->executor_err_context.data);
		return;
	}
	
	if (cstate->binary)
	{
		/* can't usefully display the data */
		if (cstate->cur_attname)
			errcontext("COPY %s, line %d, column %s",
					   cstate->cur_relname, cstate->cur_lineno, cstate->cur_attname);
		else
			errcontext("COPY %s, line %d", cstate->cur_relname, cstate->cur_lineno);
	}
	else
	{
		if (cstate->cur_attname)
		{
			/* error is relevant to a particular column */
			limit_printout_length(&cstate->attribute_buf);

			if (Mpp_role == MPP_ROLE_EXECUTE)
				errcontext("COPY %s, line %d of this segment db input data, column %s",
						   cstate->cur_relname, cstate->cur_lineno, cstate->cur_attname);
			else if(!cstate->error_on_executor)
				errcontext("COPY %s, line %d, column %s",
						   cstate->cur_relname, cstate->cur_lineno, cstate->cur_attname);
		}
		else
		{
			int			eol_len = 1;
			int			eol_loc = 0;

			if (cstate->eol_type == EOL_CRNL)
				eol_len = 2;

			eol_loc = cstate->line_buf.len - eol_len;

			if (eol_loc < 0)	/* just to be sure */
				eol_loc = 0;

			/* remove LF from line before displaying it */
			cstate->line_buf.data[cstate->line_buf.len - eol_len] = '\0';

			/* error is relevant to a particular line */
			if (cstate->line_buf_converted || !cstate->need_transcoding)
			{
				limit_printout_length(&cstate->line_buf);

				if (Mpp_role == MPP_ROLE_EXECUTE)
					errcontext("COPY %s, line %d of this segment db input data: \"%s\"", cstate->cur_relname, cstate->cur_lineno, cstate->line_buf.data);
				else if(!cstate->error_on_executor) /* don't print out context if error wasn't local */
					errcontext("COPY %s, line %d: \"%s\"", cstate->cur_relname, cstate->cur_lineno, cstate->line_buf.data);
			}
			else
			{
				/*
				 * Here, the line buffer is still in a foreign encoding,
				 * and indeed it's quite likely that the error is precisely
				 * a failure to do encoding conversion (ie, bad data).	We
				 * dare not try to convert it, and at present there's no way
				 * to regurgitate it without conversion.  So we have to punt
				 * and just report the line number.
				 */
				if (Mpp_role == MPP_ROLE_EXECUTE)
					errcontext("COPY %s, line %d of this segment db input data", cstate->cur_relname, cstate->cur_lineno);
				else if(!cstate->error_on_executor)
					errcontext("COPY %s, line %d", cstate->cur_relname, cstate->cur_lineno);
			}
		}
	}
}

/*
 * Make sure we don't print an unreasonable amount of COPY data in a message.
 *
 * It would seem a lot easier to just use the sprintf "precision" limit to
 * truncate the string.  However, some versions of glibc have a bug/misfeature
 * that vsnprintf will always fail (return -1) if it is asked to truncate
 * a string that contains invalid byte sequences for the current encoding.
 * So, do our own truncation.  We assume we can alter the StringInfo buffer
 * holding the input data.
 */
static void
limit_printout_length(StringInfo buf)
{
#define MAX_COPY_DATA_DISPLAY 100

	int			len;

	/* Fast path if definitely okay */
	if (buf->len <= MAX_COPY_DATA_DISPLAY)
		return;

	/* Apply encoding-dependent truncation */
	len = pg_mbcliplen(buf->data, buf->len, MAX_COPY_DATA_DISPLAY);
	if (buf->len <= len)
		return;					/* no need to truncate */
	buf->len = len;
	buf->data[len] = '\0';

	/* Add "..." to show we truncated the input */
	appendStringInfoString(buf, "...");
}



/*
 * Copy FROM file to relation.
 */
static void
CopyFromDispatch(CopyState cstate)
{
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	num_phys_attrs,
				attr_count,
				extra_attr_count, /* count extra attributes we add in the dispatcher COPY
							         usually non constant defaults we pre-evaluate in here */
				num_defaults;
	FmgrInfo   *in_functions;
	FmgrInfo   *out_functions; /* for handling defaults in mpp */
	FmgrInfo	oid_in_function;
	Oid		   *typioparams;
	Oid			oid_typioparam;
	ExprState **constraintexprs;
	bool	   *force_notnull;
	bool		hasConstraints = false;
	int			attnum;
	int			i;
	int			p_index;
	Oid			in_func_oid;
	Oid			out_func_oid;
	Datum	   *values;
	char	   *nulls;
	int		   *attr_offsets;
	bool		isnull;
	bool	   *isvarlena;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	bool		file_has_oids = false;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	ExprContext *econtext;		/* used for ExecEvalExpr for default atts */
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcontext;
	bool		no_more_data = false;
	bool		is_first_col;
	ListCell   *cur;

	CdbCopy    *cdbCopy;

	/*
	 * This stringInfo will contain 2 types of error messages:
	 *
	 * 1) Data errors refer to errors that are a result of inappropriate
	 *	  input data or constraint violations. All data error messages
	 *	  from the segment databases will be added to this variable and
	 *	  reported back to the client at the end of the copy command
	 *	  execution on the dispatcher.
	 * 2) Any command execution error that occurs during this COPY session.
	 *	  Such errors will usually be failure to send data over the network,
	 *	  a COPY command that was rejected by the segment databases or any I/O
	 *	  error.
	 */
	StringInfoData cdbcopy_err;

	/*
	 * contains a copy of the original COPY command string that was issued by the
	 * client. This copy of the COPY command string is sent to all the segment
	 * databases to execute there in mpp_role execute
	 */
	StringInfoData cdbcopy_cmd;

	/*
	 * Variables for cdbpolicy
	 */
	MppPolicy  *policy;			/* the partitioning policy for this table			 */
	int			p_nattrs;		/* num of partitioning keys in the policy			 */

	/*
	 * Variables for cdbhash
	 */
	CdbHash    *cdbHash;		/* a CdbHash API object		 */
	AttrNumber	h_attnum;		/* hash key attribute number */
	Datum		h_key;			/* hash key value			 */
	Oid			h_oid;			/* hash key data type		 */
	unsigned int target_seg;	/* result segment of cdbhash */
	
	tupDesc = RelationGetDescr(cstate->rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);
	num_defaults = 0;

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
	 * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = cstate->rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(cstate->rel->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
            palloc0(resultRelInfo->ri_TrigDesc->numtriggers * sizeof(FmgrInfo));
    resultRelInfo->ri_TrigInstrument = NULL;

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a dummy tuple table too */
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	ExecSetSlotDescriptor(slot, tupDesc, false);

	econtext = GetPerTupleExprContext(estate);

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass
	 * to the input function), and info about defaults and constraints.
	 * (Which input function we use depends on text/binary format choice.)
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));
	constraintexprs = (ExprState **) palloc0(num_phys_attrs * sizeof(ExprState *));
	force_notnull = (bool *) palloc(num_phys_attrs * sizeof(bool));
	isvarlena = (bool *) palloc(num_phys_attrs * sizeof(bool));


	for (attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		/* We don't need info for dropped attributes */
		if (attr[attnum - 1]->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		if (cstate->binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);

		if (list_member_int(cstate->force_notnull_atts, attnum))
			force_notnull[attnum - 1] = true;
		else
			force_notnull[attnum - 1] = false;

		/*
		 * Fetch the output function and typioparam info. We need it
		 * for handling default functions on the dispatcher COPY, if
		 * there are any.
		 */
		if (cstate->binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
									&out_func_oid,
									&isvarlena[attnum - 1]);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid,
							  &isvarlena[attnum - 1]);
		fmgr_info(out_func_oid, &out_functions[attnum - 1]);
		
		/* TODO: is force quote array necessary for default conversion */
		
		/* Get default info if needed */
		if (!list_member_int(cstate->attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(cstate->rel, attnum);

			if (defexpr != NULL)
			{
				defexprs[num_defaults] = ExecPrepareExpr((Expr *) defexpr,
														 estate);
				defmap[num_defaults] = attnum - 1;
				num_defaults++;
			}
		}

		/* If it's a domain type, set up to check domain constraints */
		if (get_typtype(attr[attnum - 1]->atttypid) == 'd')
		{
			Param	   *prm;
			Node	   *node;

			/*
			 * Easiest way to do this is to use parse_coerce.c to set up
			 * an expression that checks the constraints.  (At present,
			 * the expression might contain a length-coercion-function
			 * call and/or CoerceToDomain nodes.)  The bottom of the
			 * expression is a Param node so that we can fill in the
			 * actual datum during the data input loop.
			 */
			prm = makeNode(Param);
			prm->paramkind = PARAM_EXEC;
			prm->paramid = 0;
			prm->paramtype = getBaseType(attr[attnum - 1]->atttypid);

			node = coerce_to_domain((Node *) prm,
									prm->paramtype,
									attr[attnum - 1]->atttypid,
									COERCE_IMPLICIT_CAST, false, false);

			constraintexprs[attnum - 1] = ExecPrepareExpr((Expr *) node,
														  estate);
			hasConstraints = true;
		}
	}

	/*
	 * prepare to COPY data into segDBs:
	 * 1 - get the partitioning policy for this table.
	 * 2 - re-construct the orignial COPY command sent from the client.
	 * 3 - execute a BEGIN DTM transaction.
	 * 4 - send the COPY command to all segment databases.
	 * 5 - prepare cdbhash for hashing on row values.
	 */
	
	cdbCopy = makeCdbCopy(true);
	
	/*
	 * get the CDB policy for this table and prepare for hashing.
	 */
	policy = cstate->rel->rd_cdbpolicy;
	
	/* policy should be PARTITIONED (normal tables) or ENTRY (no column tables) */
	Insist(policy &&
           policy->ptype != POLICYTYPE_UNDEFINED);
	
	p_nattrs = policy->nattrs;	/* number of partitioning keys */
	cdbHash = makeCdbHash(cdbCopy->total_segs, HASH_FNV_1);		/* Create hash API
		* reference */
	
	/* allocate memory for error and copy strings */
	initStringInfo(&cdbcopy_err);
	initStringInfo(&cdbcopy_cmd);

	/* append schema and tablename */
	appendStringInfo(&cdbcopy_cmd, "COPY %s.%s",
					 quote_identifier(get_namespace_name(RelationGetNamespace(cstate->rel))),
					 quote_identifier(RelationGetRelationName(cstate->rel)));
	/*
	 * append column list. NOTE: if not specified originally, attnumlist will
	 * include all non-dropped columns of the table by default
	 */
	if(num_phys_attrs > 0) /* don't append anything for zero column table */
	{
		appendStringInfo(&cdbcopy_cmd, "(");
		
		is_first_col = true;
		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;

			/* We don't add dropped attributes */
			if (attr[m]->attisdropped)
				continue;
			
			/* append column string. quote it if needed */
			if (is_first_col)
				appendStringInfo(&cdbcopy_cmd, "%s", quote_identifier(NameStr(attr[m]->attname)));
			else
				appendStringInfo(&cdbcopy_cmd, ",%s", quote_identifier(NameStr(attr[m]->attname)));

			is_first_col = false;
		}
		
		/*
		 * In order to maintain consistency between the primary and mirror segment data, we
		 * want to evaluate all table columns that are not participating in this COPY command
		 * and have a non-constant default values on the dispatcher. If we let them evaluate
		 * on the primary and mirror executors separately - they will get different values.
		 * Also, if the distribution column is not participating and it has any default value,
		 * we have to evaluate it on the dispatcher only too, so that it wouldn't hash as a null
		 * and inserted as a default value on the segment databases.
		 *
		 * Therefore, we include these columns in the column list for the executor COPY.
		 * The default values will be evaluated on the dispatcher COPY and the results for
		 * the added columns will be appended to each data row that is shipped to the segments.	
		 */
		extra_attr_count = 0;
		
		for (i = 0; i < num_defaults; i++)
		{
			bool add_to_list = false;
			
			/* check 1: is this default for a distribution column? */
			for (p_index = 0; p_index < p_nattrs; p_index++)
			{
				h_attnum = policy->attrs[p_index];	
				
				if(h_attnum - 1 == defmap[i])
					add_to_list = true;
			}
			
			/* check 2: is this a non constant default? */
			if(defexprs[i]->expr->type != T_Const)
				add_to_list = true;
			
			if(add_to_list)
			{
				/* We don't add dropped attributes */
				if (attr[defmap[i]]->attisdropped)
					continue;
				
				/* append column string. quote it if needed */
				if (is_first_col)
					appendStringInfo(&cdbcopy_cmd, "%s", quote_identifier(NameStr(attr[defmap[i]]->attname)));
				else
					appendStringInfo(&cdbcopy_cmd, ",%s", quote_identifier(NameStr(attr[defmap[i]]->attname)));
				
				extra_attr_count++;
				is_first_col = false;			
			}
		}

		appendStringInfo(&cdbcopy_cmd, ")");
	}
	
	appendStringInfo(&cdbcopy_cmd, " FROM STDIN WITH");

	if (cstate->binary)
	{
		appendStringInfo(&cdbcopy_cmd, " BINARY");
	}
	else
	{
		appendStringInfo(&cdbcopy_cmd, " DELIMITER AS '%s'", cstate->delim);

		if (cstate->null_print[0] == '\\')		/* null value is de-escaped.
										 * re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " NULL AS '\\%s'", cstate->null_print);
		else
			appendStringInfo(&cdbcopy_cmd, " NULL AS '%s'", cstate->null_print);

		if (!cstate->csv_mode)
		{
			if (strcmp(cstate->escape, "\\") != 0 && !cstate->csv_mode) /* if default ("\")
														 * leave expression out */
				appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '%s'", cstate->escape);
		}

	}

	if (cstate->csv_mode)
	{
		appendStringInfo(&cdbcopy_cmd, " CSV");
		
		/* if it's a single quote de-escape it. else keep the way it is. */
		if (cstate->quote[0] == '\'')  /* quote value is de-escaped. re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " QUOTE AS ''''");
		else
			appendStringInfo(&cdbcopy_cmd, " QUOTE AS '%s'", cstate->quote);

		/* if it's a single quote or backslash de-escape it. else keep the way it is */
		if (cstate->escape[0] == '\'')
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS ''''");
		else if (cstate->escape[0] == '\\')	/* escape value is de-escaped. re-escape for segdbs */
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '\\%s'", cstate->escape);
		else
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '%s'", cstate->escape);

		/* do NOT include HEADER. Header row is swallowed by dispatcher COPY */
	}


	/*
	 * for optimized parsing - get the last field number in the
	 * file that we need to parse to have all values for the hash keys.
	 * (If the table has an empty distribution policy, then we don't need
	 * to parse any attributes really... just send the row away using
	 * a special cdbhash function designed for this purpose).
	 */
	cstate->last_hash_field = 0;

	for (p_index = 0; p_index < p_nattrs; p_index++)
	{
		i = 1;

		/*
		 * for this partitioning key, search for its location in the attr list.
		 * (note that fields may be out of order).
		 */
		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);

			if (attnum == policy->attrs[p_index])
			{
				if (i > cstate->last_hash_field)
					cstate->last_hash_field = i;
			}

			i++;
		}
	}


	/*
	 * start a transaction
	 */
	cdbCopyStartTransaction();

	/*
	 * Start a COPY command in every db of every segment in MPP.
	 *
	 * From this point in the code we need to be extra careful
	 * about error handling. ereport() must not be called until
	 * the COPY command sessions are closed on the executors.
	 * Calling ereport() will leave the executors hanging in
	 * COPY state.
	 *
	 * For errors detected by the dispatcher, we save the
	 * error message in cdbcopy_err StringInfo, move on
	 * to closing all COPY sessions on the executors and
	 * only then raise an error. We need to make sure to
	 * TRY/CATCH all other errors that may be raised from
	 * elsewhere in the backend. All error during COPY on the
	 * executors will be detected only when we end the COPY
	 * session there, so we are fine there.
	 */
	elog(DEBUG5, "COPY command sent to segdbs: %s", cdbcopy_cmd.data);
	if (cdbCopyStart(cdbCopy, cdbcopy_cmd.data) == -1)
	{
		/* get error message from CopyStart */
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);

		/* end COPY in all the segdbs in progress */
		cdbCopyEnd(cdbCopy);

		/* get error message from CopyEnd */
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);

		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("%sClosing all other existing COPY sessions.",
						cdbcopy_err.data)));
	}



	/*
	 * Prepare to catch AFTER triggers.
	 */
	//AfterTriggerBeginQuery();

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable whether
	 * we should do this for COPY, since it's not really an "INSERT"
	 * statement as such. However, executing these triggers maintains
	 * consistency with the EACH ROW triggers that we already fire on
	 * COPY.
	 */
	//ExecBSInsertTriggers(estate, resultRelInfo);

	if (!cstate->binary)
		file_has_oids = cstate->oids;	/* must rely on user to tell us this... */
	else
	{
		/* Read and verify binary header */
		char		readSig[sizeof(BinarySignature)];
		int32		tmp;
		StringInfoData header_buf;

		initStringInfo(&header_buf);

		/* Signature */
		CopyGetData(cstate, readSig, sizeof(readSig));

		appendBinaryStringInfo(&header_buf, readSig, sizeof(readSig));

		if (cstate->fe_eof ||
            memcmp(readSig, BinarySignature, sizeof(BinarySignature)) != 0)
		{
			appendStringInfo(&cdbcopy_err, "COPY file signature not recognized");
			cdbCopy->local_data_err = true;
		}

		if (!cdbCopy->local_data_err)
		{
			/* Flags field */
			if(!CopyGetInt32(cstate, &tmp))
			   appendStringInfo(&cdbcopy_err, "invalid COPY file header (missing flags)");
			
			appendBinaryStringInfo(&header_buf, (char *) &tmp, sizeof(int32));

			file_has_oids = (tmp & (1 << 16)) != 0;
			tmp &= ~(1 << 16);
			if ((tmp >> 16) != 0)
			{
				appendStringInfo(&cdbcopy_err, "unrecognized critical flags in COPY file header");
				cdbCopy->local_data_err = true;
			}
		}

		tmp = 0;
        if (!cdbCopy->local_data_err)
		{
			/* Header extension length */
			if (!CopyGetInt32(cstate, &tmp) ||
				tmp < 0)
			{
				appendStringInfo(&cdbcopy_err, "invalid COPY file header (missing length)");
				cdbCopy->local_data_err = true;
			}
			
			appendBinaryStringInfo(&header_buf, (char *) &tmp, sizeof(int32));
		}

		if (!cdbCopy->local_data_err)
		{
			/* Skip extension header, if present */
			while (tmp-- > 0)
			{
				CopyGetData(cstate, readSig, 1);

				if (Mpp_role == MPP_ROLE_DISPATCH)
					appendBinaryStringInfo(&header_buf, readSig, 1);

				
				if (cstate->fe_eof)
				{
					appendStringInfo(&cdbcopy_err, "invalid COPY file header (wrong length)");
					cdbCopy->local_data_err = true;
				}
			}
		}

		/*
		 * Send binary header to all databases if the header is valid.
		 */
		if (!cdbCopy->local_data_err)
		{
			for (i = 0; i < cdbCopy->total_segs; i++)
			{
				cdbCopySendData(cdbCopy, i, header_buf.data, header_buf.len);

				if (cdbCopy->io_errors)
					appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
			}

			pfree(header_buf.data);
		}
		else
		{
			/* Binary header not valid, end COPY and throw an error */
			cdbCopyEnd(cdbCopy);

			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("%s", cdbcopy_err.data)));

		}

		/* we can reset line buffer now, after copyGetIntX() are done */
		cstate->line_buf.len = 0;
		cstate->line_buf.data[0] = '\0';
		cstate->line_buf.cursor = 0;
	}

	if (file_has_oids && cstate->binary)
	{
		getTypeBinaryInputInfo(OIDOID,
							   &in_func_oid, &oid_typioparam);
		fmgr_info(in_func_oid, &oid_in_function);
	}

	values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
	nulls = (char *) palloc(num_phys_attrs * sizeof(char));
	attr_offsets = (int *) palloc(num_phys_attrs * sizeof(int));

	/* Make room for a PARAM_EXEC value for domain constraint checks */
	if (hasConstraints)
		econtext->ecxt_param_exec_vals = (ParamExecData *)
			palloc0(sizeof(ParamExecData));

	/* Initialize static variables */
	cstate->fe_eof = false;
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 1;
	cstate->cur_attname = NULL;

	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = (void *) cstate;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/*
	 * initialize buffered scan variables.
	 */
	if (cstate->csv_mode)
	{
		cstate->in_quote = false;
		cstate->last_was_esc = false;
	}

	cstate->null_print_len = strlen(cstate->null_print);

	/* Set up data buffer to hold a chunk of data */
	MemSet(cstate->raw_buf, ' ', RAW_BUF_SIZE * sizeof(char));
	cstate->raw_buf[RAW_BUF_SIZE] = '\0';

	no_more_data = false;		/* no more input data to read from file or FE */
	cstate->line_done = true;
	cstate->raw_buf_done = false;

	do
	{
		size_t		bytesread = 0;

		/* read a chunk of data into the buffer */
		if (!cstate->binary)
		{
			PG_TRY();
			{
				bytesread = CopyGetData(cstate, cstate->raw_buf, RAW_BUF_SIZE);	
			}
			PG_CATCH();
			{
				/* 
				 * If we are here, we got some kind of communication error
				 * with the client or a bad protocol message. clean up and 
				 * re-throw error
				 */
				cdbCopyEnd(cdbCopy);
				PG_RE_THROW();
			}
			PG_END_TRY();

			cstate->raw_buf_done = false;

			/* set buffer pointers to beginning of the buffer */
			cstate->begloc = cstate->raw_buf;
			cstate->raw_buf_index = 0;
		}

		/*
		 * continue if some bytes were read or if we didn't reach EOF. if we
		 * both reached EOF _and_ no bytes were read, quit the loop we are
		 * done
		 */
		if (bytesread > 0 || !cstate->fe_eof || cstate->binary)
		{
			/* on first time around just throw the header line away */
			if (cstate->header_line)
			{
				PG_TRY();
				{
					cstate->line_done = cstate->csv_mode ?
						CopyReadLineCSV(cstate, bytesread) :
						CopyReadLineText(cstate, bytesread);
				}
				PG_CATCH();
				{
					/* clean up and re-throw error */
					cdbCopyEnd(cdbCopy);
					PG_RE_THROW();
				}
				PG_END_TRY();
				
				cstate->cur_lineno++;
				cstate->line_buf.len = 0;
				cstate->line_buf.data[0] = '\0';
				cstate->line_buf.cursor = 0;
								
				cstate->header_line = false;
			}
			
			while (!cstate->raw_buf_done)
			{
				Oid			loaded_oid = InvalidOid;

				if (QueryCancelPending)
				{
					/* quit processing loop */
					no_more_data = true;
					break;
				}

				/* Reset the per-tuple exprcontext */
				ResetPerTupleExprContext(estate);

				/* Switch into its memory context */
				MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

				/* Initialize all values for row to NULL */
				MemSet(values, 0, num_phys_attrs * sizeof(Datum));
				MemSet(nulls, 'n', num_phys_attrs * sizeof(char));
				MemSet(attr_offsets, 0, num_phys_attrs * sizeof(int));

				if (!cstate->binary)
				{

					PG_TRY();
					{
						/* Actually read the line into memory here */
						cstate->line_done = cstate->csv_mode ?
							CopyReadLineCSV(cstate, bytesread) :
							CopyReadLineText(cstate, bytesread);
					}
					PG_CATCH();
					{
						/* clean up and re-throw error */
						cdbCopyEnd(cdbCopy);
						PG_RE_THROW();
					}
					PG_END_TRY();					

					/*
					 * if finished processing data line - increment line count.
					 * Otherwise, if eof is not yet reached, we skip att parsing
					 * and read more data. But if eof _was_ reached it means
					 * that the original last data line is defective and
					 * we want to catch that error later on.
					 */
					if (cstate->line_done)
						cstate->cur_lineno++;
					else if (!cstate->fe_eof || cstate->end_marker)
						break;

					if (file_has_oids)
					{
						char	   *oid_string;

						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(cstate, &isnull);

						if (isnull)
						{
							/* clean up and throw error */
							cdbCopyEnd(cdbCopy);
							
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("null OID in COPY data.")));									
						}
						else
						{
							PG_TRY();
							{
								cstate->cur_attname = "oid";
								loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
											   CStringGetDatum(oid_string)));
							}
							PG_CATCH();
							{
								/* clean up and re-throw error */
								cdbCopyEnd(cdbCopy);
								PG_RE_THROW();
							}
							PG_END_TRY();

							if (loaded_oid == InvalidOid)
							{
								/* clean up and throw error */
								cdbCopyEnd(cdbCopy);
								
								ereport(ERROR,
										(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
										 errmsg("invalid OID in COPY data.")));
							}

							cstate->cur_attname = NULL;
						}
					}

					/*
					 * try to read a data line, if failed catch the error,
					 * flag it and break
					 */
					PG_TRY();
					{
						/* parse all the attributes in the data line */
						if (cstate->csv_mode)
							CopyReadAttributesCSV(cstate, nulls, attr_offsets, num_phys_attrs, attr);
						else
							CopyReadAttributesText(cstate, nulls, attr_offsets, num_phys_attrs, attr);
					}
					PG_CATCH();
					{
						/* clean up and re-throw error */
						cdbCopyEnd(cdbCopy);
						PG_RE_THROW();						
					}
					PG_END_TRY();

					/*
					 * Since we only need the internal format of values that
					 * we want to hash on (partitioning keys only), we want to
					 * skip converting the other values so we can run faster.
					 */
					for (p_index = 0; p_index < p_nattrs; p_index++)
					{
						/*
						 * for this partitioning key, search for its location in the attr list.
						 * (note that fields may be out of order, so this is necessary).
						 */
						foreach(cur, cstate->attnumlist)
						{
							int			attnum = lfirst_int(cur);
							int			m = attnum - 1;
							char	   *string;

							if (attnum == policy->attrs[p_index])
							{
								string = cstate->attribute_buf.data + attr_offsets[m];

								if (nulls[m] == ' ')
									isnull = false;
								else
									isnull = true;

								if (cstate->csv_mode && isnull && force_notnull[m])
								{
									string = cstate->null_print;		/* set to NULL string */
									isnull = false;
								}

								/* we read an SQL NULL, no need to do anything */
								if (!isnull)
								{
									cstate->cur_attname = NameStr(attr[m]->attname);

									PG_TRY();
									{
										values[m] = FunctionCall3(&in_functions[m],
													 CStringGetDatum(string),
											ObjectIdGetDatum(typioparams[m]),
										  Int32GetDatum(attr[m]->atttypmod));
									}
									PG_CATCH();
									{
										/* clean up and re-throw error */
										cdbCopyEnd(cdbCopy);
										PG_RE_THROW();
									}
									PG_END_TRY();

									cstate->cur_attname = NULL;
								}		/* end if (!isnull) */

								break;	/* go to next partitioning key
										 * attribute */
							}
						}		/* end foreach */
					}			/* end for partitioning indexes */

				}
				else
				{
					/* binary */
					int16		fld_count;
					uint16		buf;
					ListCell   *cur;

					if (!CopyGetInt16(cstate, &fld_count) ||
						fld_count == -1)
					{
						cstate->raw_buf_done = true;
						no_more_data = true;
						break;
					}
					
					fld_count += (int16)extra_attr_count; /* add pre-evaluated defaults to count */
					buf = htons((uint16) fld_count); /* change to network byte order */
					appendBinaryStringInfo(&cstate->line_buf, (char *) &buf, sizeof(buf));
					
					if (fld_count != attr_count + extra_attr_count)
					{
						appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY: row field count is %d, expected %d",
										 (int) fld_count, attr_count + extra_attr_count);
						cdbCopy->local_data_err = true;
						no_more_data = true;
						break;
					}

					if (file_has_oids)
					{
						cstate->cur_attname = "oid";
						loaded_oid =
							DatumGetObjectId(CopyReadBinaryAttribute(cstate, 0,
															&oid_in_function,
															  oid_typioparam,
																		  -1,
																   &isnull));
						if (isnull || loaded_oid == InvalidOid)
						{
							appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY: invalid OID in COPY data");
							cdbCopy->local_data_err = true;
							no_more_data = true;
							break;
						}

						cstate->cur_attname = NULL;
					}

					i = 0;
					foreach(cur, cstate->attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;

						cstate->cur_attname = NameStr(attr[m]->attname);
						i++;
						values[m] = CopyReadBinaryAttribute(cstate, i,
															&in_functions[m],
															typioparams[m],
															attr[m]->atttypmod,
															&isnull);
						nulls[m] = isnull ? 'n' : ' ';
						cstate->cur_attname = NULL;
					}
				}

				/*
				 * Now compute defaults for only:
				 * 1 - the distribution column,
				 * 2 - any other column with a non-constant default expression (such as a function)
				 * that is, of course, if these columns not provided by the input data.	
				 * Anything not processed here or above will remain NULL.
				 */
				for (i = 0; i < num_defaults; i++)
				{
					bool compute_default = false;

					/* check 1: is this default for a distribution column? */
					for (p_index = 0; p_index < p_nattrs; p_index++)
					{
						h_attnum = policy->attrs[p_index];	
						
						if(h_attnum - 1 == defmap[i])
							compute_default = true;
					}
					
					/* check 2: is this a default function? (non-constant default) */
					if(defexprs[i]->expr->type != T_Const)
						compute_default = true;
					
					if(compute_default)
					{
						char *string;
						
						PG_TRY();
						{
							values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
															 &isnull, NULL);
						}
						PG_CATCH();
						{
							/* clean up and re-throw error */
							cdbCopyEnd(cdbCopy);
							PG_RE_THROW();
						}
						PG_END_TRY();
												
						/*
						 * prepare to concatinate next value:
						 * remove eol characters from end of line buf
						 */
						if(!cstate->binary)
							truncateEol(cstate);
						
						if (isnull)
						{
							if (!cstate->binary)
							{
								appendStringInfo(&cstate->line_buf, "%c%s", cstate->delim[0], cstate->null_print);
							}
							else
							{
								int32		null_marker = -1; /* null marker */
								appendBinaryStringInfo(&cstate->line_buf, (char *) &null_marker, sizeof(null_marker));
							}
						}
						else
						{
							nulls[defmap[i]] = ' ';	
							
							if (!cstate->binary)
							{
								appendStringInfo(&cstate->line_buf, "%c", cstate->delim[0]); /* write the delimiter */
												
								string = DatumGetCString(FunctionCall3(&out_functions[defmap[i]],
																	   values[defmap[i]],
																	   ObjectIdGetDatum(typioparams[defmap[i]]),
																	   Int32GetDatum(attr[defmap[i]]->atttypmod)));
								if (cstate->csv_mode)
								{
									CopyAttributeOutCSV(cstate, string,
														false, /*force_quote[attnum - 1],*/
														list_length(cstate->attnumlist) == 1);
								}
								else
									CopyAttributeOutText(cstate, string);
								
							}
							else
							{
								bytea	   *outputbytes;
							
								outputbytes = DatumGetByteaP(FunctionCall2(&out_functions[defmap[i]],
																		   values[defmap[i]],
																		   ObjectIdGetDatum(typioparams[defmap[i]])));
								
								/* We assume the result will not have been toasted */
								CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
								CopySendData(cstate, VARDATA(outputbytes),
											 VARSIZE(outputbytes) - VARHDRSZ);
							}
						}
												
						/* re-add the eol characters */
						if(!cstate->binary)
							concatenateEol(cstate);
					}
					
				}

				if (cdbCopy->local_data_err)	/* if data error occured,
											* break out of all loops */
				{
					no_more_data = true;
					break;
				}

				/*
				 * Next apply any domain constraints
				 */
				if (hasConstraints)
				{
					ParamExecData *prmdata = &econtext->ecxt_param_exec_vals[0];
					ExprState  *exprstate;
					
					/* we must only care about constraints for distribution keys
					   right now, the rest of the attributes with constraints
					   will be dealt with in CopyFromExec */
					for (i = 0; i < p_nattrs; i++)
					{
						h_attnum = policy->attrs[i];		/* current att num from
						* cdb policy */

						exprstate = constraintexprs[h_attnum - 1];
							
						if (exprstate == NULL)
							continue;	/* no constraint for this attr */

						/* Insert current hash key value into the Param value */
						prmdata->value = values[h_attnum - 1];
						prmdata->isnull = (nulls[h_attnum - 1] == 'n');

						/*
						 * Execute the constraint expression.  Allow the
						 * expression to replace the value (consider e.g. a
						 * timestamp precision restriction).
						 */
						values[h_attnum - 1] = ExecEvalExpr(exprstate, econtext,
												 &isnull, NULL);
						nulls[h_attnum - 1] = isnull ? 'n' : ' ';
					}
				}

				/*
				 * At this point in the code, values[x] is final for this
				 * data row -- either the input data, a null or a default
				 * value is in there, and constraints applied.
				 *
				 * Perform a cdbhash on this data row. Perform a hash operation
				 * on each attribute that is included in CDB policy (partitioning
				 * key columns). Send COPY data line to the target segment
				 * database executors. Data row will not be inserted locally.
				 */
				cdbhashinit(cdbHash);

				for (i = 0; i < p_nattrs; i++)
				{
					h_attnum = policy->attrs[i];		/* current att num from
														 * cdb policy */
					h_key = values[h_attnum - 1];		/* value of this attr			   */
					
					/*
					 * get the data type of this attribute. If it's
					 * a domain type get the base type, if it's an
					 * array type use anyarray, or else just use as is.
					 */
					if (get_typtype(attr[h_attnum - 1]->atttypid) == 'd')
						h_oid = getBaseType(attr[h_attnum - 1]->atttypid);
					else if(attr[h_attnum - 1]->attndims > 0)
						h_oid = ANYARRAYOID;
					else
						h_oid = attr[h_attnum - 1]->atttypid;
					
					if (nulls[h_attnum - 1] != 'n')
					{
						PG_TRY();
						{
							cdbhash(cdbHash, h_key, h_oid);
						}
						PG_CATCH();
						{
							/* clean up and re-throw error */
							cdbCopyEnd(cdbCopy);
							PG_RE_THROW();
						}
						PG_END_TRY();					
					}
					else
						cdbhashnull(cdbHash);
				}

				/*
				 * If this is a relation with an empty policy, there is no
				 * hash key to use, therefore use cdbhashnokey() to pick a 
				 * hash value for us.
				 */
				if(p_nattrs == 0)
					cdbhashnokey(cdbHash);

				target_seg = cdbhashreduce(cdbHash);	/* hash result segment */

				/*
				 * Send data row to all databases for this segment.
				 */
				cdbCopySendData(cdbCopy, target_seg, cstate->line_buf.data, cstate->line_buf.len);
				
				cstate->processed++;
				
				if (cdbCopy->io_errors)
				{
					appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
					no_more_data = true;
					break;
				}

				cstate->line_buf.len = 0;		/* we can reset line buffer now. */
				cstate->line_buf.data[0] = '\0';
				cstate->line_buf.cursor = 0;
			}					/* end while(!raw_buf_done) */
		}						/* end if (bytesread > 0 || !cstate->fe_eof) */
		else
			/* no bytes read, end of data */
		{
			no_more_data = true;
		}
	} while (!no_more_data);

	/*
	 * Done reading input data and sending it off to the segment
	 * databases Now we would like to end the copy command on
	 * all segment databases across the cluster.
	 */
	cdbCopyEnd(cdbCopy);

	/*
	 * If we quit the processing loop earlier due to a
	 * cancel query signal, we now throw an error.
	 * (Safe to do only after cdbCopyEnd).
	 */
	CHECK_FOR_INTERRUPTS();


	if (cdbCopy->remote_data_err || cdbCopy->local_data_err || cdbCopy->io_errors)
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);

	if (cdbCopy->remote_data_err)
	{
		cstate->error_on_executor = true;
		if(cdbCopy->err_context.len > 0)
			appendStringInfo(&cstate->executor_err_context, cdbCopy->err_context.data);
	}
		
	/*
	 * report all accumulated errors back to the client.
	 */
	if (cdbCopy->remote_data_err || cdbCopy->local_data_err)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("%s", cdbcopy_err.data)));
	if (cdbCopy->io_errors)
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("%s", cdbcopy_err.data)));
	/*
	 * Done, clean up
	 */
	error_context_stack = errcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Execute AFTER STATEMENT insertion triggers
	 */
	//ExecASInsertTriggers(estate, resultRelInfo);

	/*
	 * Handle queued AFTER triggers
	 */
	//AfterTriggerEndQuery(estate);

	/*
	 * free all resources besides ones that are needed for error reporting
	 */
	pfree(cdbHash);
	pfree(values);
	pfree(nulls);
	pfree(attr_offsets);
	pfree(in_functions);
	pfree(out_functions);
	pfree(isvarlena);
	pfree(typioparams);
	pfree(defmap);
	pfree(defexprs);
	pfree(constraintexprs);
	pfree(force_notnull);
	pfree(cdbcopy_cmd.data);
	pfree(cdbcopy_err.data);
	pfree(cdbCopy);

	ExecDropTupleTable(tupleTable, true);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
}

/*
 * Copy FROM file to relation.
 */
static void
CopyFromExec(CopyState cstate)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	num_phys_attrs,
				attr_count,
				num_defaults;
	FmgrInfo   *in_functions;
	FmgrInfo	oid_in_function;
	Oid		   *typioparams;
	Oid			oid_typioparam;
	ExprState **constraintexprs;
	bool	   *force_notnull;
	bool		hasConstraints = false;
	int			attnum;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	bool		isnull;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	bool		file_has_oids;
	int		   *defmap;
	ExprState **defexprs;		/* array of default att expressions */
	ExprContext *econtext;		/* used for ExecEvalExpr for default atts */
	MemoryContext oldcontext = CurrentMemoryContext;
	ErrorContextCallback errcontext;
	int		   *attr_offsets;
	bool		no_more_data;
	ListCell   *cur;


	tupDesc = RelationGetDescr(cstate->rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);
	num_defaults = 0;

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
	 * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = cstate->rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(cstate->rel->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
            palloc0(resultRelInfo->ri_TrigDesc->numtriggers * sizeof(FmgrInfo));
    resultRelInfo->ri_TrigInstrument = NULL;

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a dummy tuple table too */
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	ExecSetSlotDescriptor(slot, tupDesc, false);

	econtext = GetPerTupleExprContext(estate);

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function, the element type (to pass
																 * to the input function), and info about defaults and constraints.
	 * (Which input function we use depends on text/binary format choice.)
	 */
	in_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));
	constraintexprs = (ExprState **) palloc0(num_phys_attrs * sizeof(ExprState *));
	force_notnull = (bool *) palloc(num_phys_attrs * sizeof(bool));

	for (attnum = 1; attnum <= num_phys_attrs; attnum++)
	{
		/* We don't need info for dropped attributes */
		if (attr[attnum - 1]->attisdropped)
			continue;

		/* Fetch the input function and typioparam info */
		if (cstate->binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);

		if (list_member_int(cstate->force_notnull_atts, attnum))
			force_notnull[attnum - 1] = true;
		else
			force_notnull[attnum - 1] = false;

		/* Get default info if needed */
		if (!list_member_int(cstate->attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(cstate->rel, attnum);

			if (defexpr != NULL)
			{
				defexprs[num_defaults] = ExecPrepareExpr((Expr *) defexpr,
														 estate);
				defmap[num_defaults] = attnum - 1;
				num_defaults++;
			}
		}

		/* If it's a domain type, set up to check domain constraints */
		if (get_typtype(attr[attnum - 1]->atttypid) == 'd')
		{
			Param	   *prm;
			Node	   *node;

			/*
			 * Easiest way to do this is to use parse_coerce.c to set up
			 * an expression that checks the constraints.  (At present,
															* the expression might contain a length-coercion-function
															* call and/or CoerceToDomain nodes.)  The bottom of the
			 * expression is a Param node so that we can fill in the
			 * actual datum during the data input loop.
			 */
			prm = makeNode(Param);
			prm->paramkind = PARAM_EXEC;
			prm->paramid = 0;
			prm->paramtype = getBaseType(attr[attnum - 1]->atttypid);

			node = coerce_to_domain((Node *) prm,
									prm->paramtype,
									attr[attnum - 1]->atttypid,
									COERCE_IMPLICIT_CAST, false, false);

			constraintexprs[attnum - 1] = ExecPrepareExpr((Expr *) node,
														  estate);
			hasConstraints = true;
		}
	}

	/*
	 * Prepare to catch AFTER triggers.
	 */
	//AfterTriggerBeginQuery();

	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable whether
	 * we should do this for COPY, since it's not really an "INSERT"
	 * statement as such. However, executing these triggers maintains
	 * consistency with the EACH ROW triggers that we already fire on
	 * COPY.
	 */
	//ExecBSInsertTriggers(estate, resultRelInfo);

	if (!cstate->binary)
		file_has_oids = cstate->oids;	/* must rely on user to tell us this... */
	else
	{
		/* Read and verify binary header */
		char		readSig[sizeof(BinarySignature)];
		int32		tmp;
		
		/* Signature */
		if (CopyGetData(cstate, readSig, sizeof(readSig)) != sizeof(readSig) ||
			memcmp(readSig, BinarySignature, sizeof(BinarySignature)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("COPY file signature not recognized")));
		/* Flags field */
		if (!CopyGetInt32(cstate, &tmp))
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (missing flags)")));
		file_has_oids = (tmp & (1 << 16)) != 0;
		tmp &= ~(1 << 16);
		if ((tmp >> 16) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("unrecognized critical flags in COPY file header")));
		/* Header extension length */
		if (!CopyGetInt32(cstate, &tmp) ||
			tmp < 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (missing length)")));
		/* Skip extension header, if present */
		while (tmp-- > 0)
		{
			if (CopyGetData(cstate, readSig, 1) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("invalid COPY file header (wrong length)")));
		}
	}

	if (file_has_oids && cstate->binary)
	{
		getTypeBinaryInputInfo(OIDOID,
							   &in_func_oid, &oid_typioparam);
		fmgr_info(in_func_oid, &oid_in_function);
	}

	values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
	nulls = (char *) palloc(num_phys_attrs * sizeof(char));
	attr_offsets = (int *) palloc(num_phys_attrs * sizeof(int));

	/* Make room for a PARAM_EXEC value for domain constraint checks */
	if (hasConstraints)
		econtext->ecxt_param_exec_vals = (ParamExecData *)
			palloc0(sizeof(ParamExecData));

	/* Initialize static variables */
	cstate->fe_eof = false;
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = RelationGetRelationName(cstate->rel);
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;

	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = (void *) cstate;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/*
	 * initialize buffered scan variables.
	 */
	if (cstate->csv_mode)
	{
		cstate->in_quote = false;
		cstate->last_was_esc = false;
	}

	cstate->null_print_len = strlen(cstate->null_print);

	/* Set up data buffer to hold a chunk of data */
	MemSet(cstate->raw_buf, ' ', RAW_BUF_SIZE * sizeof(char));
	cstate->raw_buf[RAW_BUF_SIZE] = '\0';

	no_more_data = false;		/* no more input data to read from file or FE */
	cstate->line_done = true;
	cstate->raw_buf_done = false;

	do
	{
		size_t		bytesread = 0;

		/* read a chunk of data into the buffer */
		if (!cstate->binary)
		{
			bytesread = CopyGetData(cstate, cstate->raw_buf, RAW_BUF_SIZE);
			cstate->raw_buf_done = false;

			/* set buffer pointers to beginning of the buffer */
			cstate->begloc = cstate->raw_buf;
			cstate->raw_buf_index = 0;
		}

		/*
		 * continue if some bytes were read or if we didn't reach EOF. if we
		 * both reached EOF _and_ no bytes were read, quit the loop we are
		 * done
		 */
		if (bytesread > 0 || !cstate->fe_eof || cstate->binary)
		{

			while (!cstate->raw_buf_done)
			{
				bool		skip_tuple;
				Oid			loaded_oid = InvalidOid;

				CHECK_FOR_INTERRUPTS();

				/* Reset the per-tuple exprcontext */
				ResetPerTupleExprContext(estate);

				/* Switch into its memory context */
				MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

				/* Initialize all values for row to NULL */
				MemSet(values, 0, num_phys_attrs * sizeof(Datum));
				MemSet(nulls, 'n', num_phys_attrs * sizeof(char));
				/* reset attribute pointers */
				MemSet(attr_offsets, 0, num_phys_attrs * sizeof(int));

				if (!cstate->binary)
				{

					/* Actually read the line into memory here */
					cstate->line_done = cstate->csv_mode ?
						CopyReadLineCSV(cstate, bytesread) :
						CopyReadLineText(cstate, bytesread);

					/*
					 * if finished processing data line - increment line count.
					 * Otherwise, if eof is not yet reached, we skip att parsing
					 * and read more data. But if eof _was_ reached it means
					 * that the original last data line is defective and
					 * we want to catch that error later on.
					 */
					if (cstate->line_done)
						cstate->cur_lineno++;
					else if (!cstate->fe_eof || cstate->end_marker)
						break;

					if (file_has_oids)
					{
						char	   *oid_string;

						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(cstate, &isnull);

						if (isnull)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("null OID in COPY data")));
						else
						{
							cstate->cur_attname = "oid";
							loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
											   CStringGetDatum(oid_string)));
							if (loaded_oid == InvalidOid)
								ereport(ERROR,
									  (errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									   errmsg("invalid OID in COPY data")));
							cstate->cur_attname = NULL;
						}
					}

					/* parse all the attribute in the data line */
					if (cstate->csv_mode)
						CopyReadAttributesCSV(cstate, nulls, attr_offsets, num_phys_attrs, attr);
					else
						CopyReadAttributesText(cstate, nulls, attr_offsets, num_phys_attrs, attr);
					
					/*
					 * Loop to read the user attributes on the line.
					 */
					foreach(cur, cstate->attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;
						char	   *string;

						string = cstate->attribute_buf.data + attr_offsets[m];

						if (nulls[m] == ' ')
							isnull = false;
						else
							isnull = true;

						if (cstate->csv_mode && isnull && force_notnull[m])
						{
							string = cstate->null_print;		/* set to NULL string */
							isnull = false;
						}

						/* we read an SQL NULL, no need to do anything */
						if (!isnull)
						{
							cstate->cur_attname = NameStr(attr[m]->attname);
							values[m] = FunctionCall3(&in_functions[m],
													  CStringGetDatum(string),
											ObjectIdGetDatum(typioparams[m]),
										  Int32GetDatum(attr[m]->atttypmod));
							nulls[m] = ' ';
							cstate->cur_attname = NULL;
						}
					}
				}
				else
				{
					/* binary */
					int16		fld_count;
					ListCell   *cur;

					if (!CopyGetInt16(cstate, &fld_count) ||
						fld_count == -1)
					{
						cstate->raw_buf_done = true;
						no_more_data = true;
						break;
					}
					
					if (fld_count != attr_count)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("row field count is %d, expected %d",
										(int) fld_count, attr_count)));

					if (file_has_oids)
					{
						cstate->cur_attname = "oid";
						loaded_oid =
							DatumGetObjectId(CopyReadBinaryAttribute(cstate, 0,
															&oid_in_function,
															  oid_typioparam,
															  -1,
																   &isnull));
						if (isnull || loaded_oid == InvalidOid)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("invalid OID in COPY data")));
						cstate->cur_attname = NULL;
					}

					i = 0;
					foreach(cur, cstate->attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;

						cstate->cur_attname = NameStr(attr[m]->attname);
						i++;
						values[m] = CopyReadBinaryAttribute(cstate, i,
															&in_functions[m],
															typioparams[m],
															attr[m]->atttypmod,
															&isnull);
						nulls[m] = isnull ? 'n' : ' ';
						cstate->cur_attname = NULL;
					}
				}

				/*
				 * Now compute and insert any defaults available for the columns
				 * not provided by the input data.	Anything not processed here or
				 * above will remain NULL.
				 */
				for (i = 0; i < num_defaults; i++)
				{
					values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
													 &isnull, NULL);
					if (!isnull)
						nulls[defmap[i]] = ' ';
				}

				/*
				 * Next apply any domain constraints
				 */
				if (hasConstraints)
				{
					ParamExecData *prmdata = &econtext->ecxt_param_exec_vals[0];

					for (i = 0; i < num_phys_attrs; i++)
					{
						ExprState  *exprstate = constraintexprs[i];

						if (exprstate == NULL)
							continue;	/* no constraint for this attr */

						/* Insert current row's value into the Param value */
						prmdata->value = values[i];
						prmdata->isnull = (nulls[i] == 'n');

						/*
						 * Execute the constraint expression.  Allow the
						 * expression to replace the value (consider e.g. a
															* timestamp precision restriction).
						 */
						values[i] = ExecEvalExpr(exprstate, econtext,
												 &isnull, NULL);
						nulls[i] = isnull ? 'n' : ' ';
					}
				}

				/*
				 * And now we can form the input tuple.
				 */
				tuple = heap_formtuple(tupDesc, values, nulls);

				if (cstate->oids && file_has_oids)
					HeapTupleSetOid(tuple, loaded_oid);

				/*
				 * Triggers and stuff need to be invoked in query context.
				 */
				MemoryContextSwitchTo(oldcontext);

				skip_tuple = false;

				/* BEFORE ROW INSERT Triggers */
				if (resultRelInfo->ri_TrigDesc &&
					resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
				{
					HeapTuple	newtuple;				
					Assert(resultRelInfo->ri_TrigFunctions != NULL);
					newtuple = ExecBRInsertTriggers(estate, resultRelInfo, tuple);

					if (newtuple == NULL)		/* "do nothing" */
						skip_tuple = true;
					else if (newtuple != tuple) /* modified by Trigger(s) */
					{
						heap_freetuple(tuple);
						tuple = newtuple;
					}
				}

				if (!skip_tuple)
				{
					/* Place tuple in tuple slot */
					ExecStoreTuple(tuple, slot, InvalidBuffer, false);

					/*
					 * Check the constraints of the tuple
					 */
					if (cstate->rel->rd_att->constr)
						ExecConstraints(resultRelInfo, slot, estate);

					/*
					 * OK, store the tuple and create index entries for it
					 */
					simple_heap_insert(cstate->rel, tuple);

					if (resultRelInfo->ri_NumIndices > 0)
						ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

					/* AFTER ROW INSERT Triggers */
					//ExecARInsertTriggers(estate, resultRelInfo, tuple);
					
					/*
					 * We count only tuples not suppressed by a BEFORE INSERT trigger;
					 * this is the same definition used by execMain.c for counting
					 * tuples inserted by an INSERT command.
					 *
					 * MPP: incrementing this counter here only matters for utility
					 * mode. in dispatch mode only the dispatcher COPY collects row
					 * count, so this counter is meaningless.
					 */
					cstate->processed++;
				}

				cstate->line_buf.len = 0;		/* we can reset line buffer now. */
				cstate->line_buf.data[0] = '\0';
				cstate->line_buf.cursor = 0;
			}					/* end while(!raw_buf_done) */
		}						/* end if (bytesread > 0 || !cstate->fe_eof) */
		else
			/* no bytes read, end of data */
		{
			no_more_data = true;
		}
	} while (!no_more_data);

	/*
	 * Done, clean up
	 */
	error_context_stack = errcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Execute AFTER STATEMENT insertion triggers
	 */
	//ExecASInsertTriggers(estate, resultRelInfo);

	/*
	 * Handle queued AFTER triggers
	 */
	//AfterTriggerEndQuery(estate);

	pfree(attr_offsets);

	pfree(values);
	pfree(nulls);

	pfree(in_functions);
	pfree(typioparams);
	pfree(defmap);
	pfree(defexprs);
	pfree(constraintexprs);
	pfree(force_notnull);

	ExecDropTupleTable(tupleTable, true);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
}

/*
 * Finds the next TEXT line that is in the input buffer and loads
 * it into line_buf. Returns an indication if the line that was read
 * is complete (if an unescaped line-end was encountered). If we
 * reached the end of buffer before the whole line was written into the
 * line buffer then returns false.
 */
static bool
CopyReadLineText(CopyState cstate, size_t bytesread)
{
	int			linesize;
	char	   *cvt;
	char		escapec = '\0';

	/* mark that encoding conversion hasn't occurred yet */
	cstate->line_buf_converted = false;

	/*
	 * set the escape char for text format ('\\' by default).
	 */
	escapec = cstate->escape[0];

	/*
	 * Detect end of line type if not already detected.
	 */
	if (cstate->eol_type == EOL_UNKNOWN)
	{
		bool		eol_detected;
		
		cstate->quote = NULL;
		eol_detected = DetectLineEnd(cstate, bytesread);

		if (!eol_detected)
		{
			/* load entire input buffer into line buf, and quit */
			appendBinaryStringInfo(&cstate->line_buf, cstate->raw_buf, RAW_BUF_SIZE);
			cstate->raw_buf_done = true;
			return false;
		}
	}

	/*
	 * Special case: eol is CRNL, last byte of previous buffer was an
	 * unescaped CR and 1st byte of current buffer is NL. We check for
	 * that here.
	 */
	if (cstate->eol_type == EOL_CRNL)
	{
		/* if we started scanning from the 1st byte of the buffer */
		if (cstate->begloc == cstate->raw_buf)
		{
			/* and had a CR in last byte of prev buf */
			if (cstate->cr_in_prevbuf)
			{
				/*
				 * if this 1st byte in buffer is 2nd byte of line end sequence
				 * (linefeed)
				 */
				if (*(cstate->begloc) == cstate->eol_ch[1])
				{
					/*
					* load that one linefeed byte and indicate we are done
					* with the data line
					*/
					appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, 1);
					cstate->raw_buf_index++;
					cstate->begloc++;
					cstate->esc_in_prevbuf = false;
					cstate->cr_in_prevbuf = false;
					return true;
				}
			}

			cstate->cr_in_prevbuf = false;
		}
	}

	/*
	 * (we need a loop so that if eol_ch is found, but prev ch is backslash,
		* we can search for the next eol_ch)
	 */
	while (true)
	{
		/* reached end of buffer */
		if ((cstate->endloc = scanTextLine(cstate, cstate->begloc, cstate->eol_ch[0], bytesread - cstate->raw_buf_index)) == NULL)
		{
			linesize = RAW_BUF_SIZE - (cstate->begloc - cstate->raw_buf);
			appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, linesize);

			if (cstate->line_buf.len > 1)
			{
				char	   *last_ch = cstate->line_buf.data + cstate->line_buf.len - 1; /* before terminating \0 */

				if (*last_ch == escapec && !cstate->escape_off)
				{
					cstate->esc_in_prevbuf = true;

					if (cstate->line_buf.len > 2)
					{
						last_ch--;
						if (*last_ch == escapec)
							cstate->esc_in_prevbuf = false;
					}
				}
				else if (*last_ch == '\r')
				{
					if (cstate->eol_type == EOL_CRNL)
						cstate->cr_in_prevbuf = true;
				}
			}

			cstate->line_done = false;
			cstate->raw_buf_done = true;
			break;
		}
		else
			/* found the 1st eol ch in raw_buf. */
		{
			bool		eol_found = true;
			bool		eol_escaped = (cstate->escape_off ? false : true);

			/*
			 * Load that piece of data (potentially a data line) into the line buffer,
			 * and update the pointers for the next scan.
			 */
			linesize = cstate->endloc - cstate->begloc + 1;
			appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, linesize);
			cstate->raw_buf_index += linesize;
			cstate->begloc = cstate->endloc + 1;

			if (cstate->eol_type == EOL_CRNL)
			{
				/* check if there is a '\n' after the '\r' */
				if (*(cstate->endloc + 1) == '\n')
				{
					/* this is a line end */
					appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, 1);		/* load that '\n' */
					cstate->raw_buf_index++;
					cstate->begloc++;
				}
				else
					/* just a CR, not a line end */
					eol_found = false;
			}

			/*
			 * in some cases, this end of line char happens to be the
			 * last character in the buffer. we need to catch that.
			 */
			if (cstate->raw_buf_index >= bytesread)
				cstate->raw_buf_done = true;

			/*
			 * Check if the 1st end of line ch is escaped.
			 */
			if (cstate->endloc != cstate->raw_buf)	/* can we look 1 char back? */
			{
				if (*(cstate->endloc - 1) != escapec)	/* prev char is not an escape */
					eol_escaped = false;
				else
					/* prev char is an escape */
				{
					if (cstate->endloc != (cstate->raw_buf + 1))		/* can we look another
														 * char back? */
					{
						/* it's a double escape char, so it's not an escape */
						if (*(cstate->endloc - 2) == escapec)
							eol_escaped = false;
						/* else it's a single escape char, so EOL is ascaped */
					}
					else
					{
						/* we need to check in the last buffer */
						if (cstate->esc_in_prevbuf)
							eol_escaped = false;
					}
				}
			}
			else
				/*
				 * this eol ch is first ch in buffer, check for escape in prev buf
				 */
			{
				if (!cstate->esc_in_prevbuf)
					eol_escaped = false;
			}

			cstate->esc_in_prevbuf = false;		/* reset variable */

			/*
			 * if eol was found, and it isn't escaped, line is done
			 */
			if ((eol_escaped == false) && eol_found)
			{
				cstate->line_done = true;
				break;
			}
			else
			{
				/* stay in the loop and process some more data. */
				cstate->line_done = false;

				if (eol_found)
					cstate->cur_lineno++;		/* increase line index for error
										 * reporting */
			}

		}						/* end of found eol_ch */
	}

	/*
	 * Done reading the line. Convert it to server encoding if it's complete.
	 */
	if (cstate->need_transcoding && cstate->line_done)
	{
		cvt = (char *) pg_client_to_server(cstate->line_buf.data,
										   cstate->line_buf.len);
		if (cvt != cstate->line_buf.data)
		{
			/* transfer converted data back to line_buf */
			cstate->line_buf.len = 0;
			cstate->line_buf.data[0] = '\0';
			cstate->line_buf.cursor = 0;
			appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
			pfree(cvt);
		}
	}

	/* indicate that conversion had occured */
	cstate->line_buf_converted = true;

	/*
	 * check if this line is an end marker -- "\."
	 */
	cstate->end_marker = false;

	switch (cstate->eol_type)
	{
		case EOL_NL:
			if (!strcmp(cstate->line_buf.data, "\\.\n"))
				cstate->end_marker = true;
			break;
		case EOL_CR:
			if (!strcmp(cstate->line_buf.data, "\\.\r"))
				cstate->end_marker = true;
			break;
		case EOL_CRNL:
			if (!strcmp(cstate->line_buf.data, "\\.\r\n"))
				cstate->end_marker = true;
			break;
		case EOL_UNKNOWN:
			break;
	}

	if (cstate->end_marker)
	{
		/*
		 * Reached end marker. In protocol version 3 we
		 * should ignore anything after \. up to protocol
		 * end of copy data.
		 */
		if (cstate->copy_dest == COPY_NEW_FE)
		{
			while (!cstate->fe_eof)
			{
				CopyGetData(cstate, cstate->raw_buf, RAW_BUF_SIZE);	/* eat data */
			}
		}

		cstate->fe_eof = true;
		/* we don't want to process a \. as data line, want to quit. */
		cstate->line_done = false;
		cstate->raw_buf_done = true;
	}

	return cstate->line_done;
}

/*
 * Finds the next CSV line that is in the input buffer and loads
 * it into line_buf. Returns an indication if the line that was read
 * is complete (if an unescaped line-end was encountered). If we
 * reached the end of buffer before the whole line was written into the
 * line buffer then returns false.
 */
static bool
CopyReadLineCSV(CopyState cstate, size_t bytesread)
{
	int			linesize;
	char	   *cvt;
	char		quotec = '\0',
				escapec = '\0';

	/* mark that encoding conversion hasn't occurred yet */
	cstate->line_buf_converted = false;

	escapec = cstate->escape[0];
	quotec = cstate->quote[0];

	/* ignore special escape processing if it's the same as quotec */
	if (quotec == escapec)
		escapec = '\0';

	/*
	 * Detect end of line type if not already detected.
	 */
	if (cstate->eol_type == EOL_UNKNOWN)
	{
		bool		eol_detected = DetectLineEnd(cstate, bytesread);

		if (!eol_detected)
		{
			/* load entire input buffer into line buf, and quit */
			appendBinaryStringInfo(&cstate->line_buf, cstate->raw_buf, RAW_BUF_SIZE);
			cstate->line_done = false;
			cstate->raw_buf_done = true;

			return cstate->line_done;
		}
	}

	/*
	 * Special case: eol is CRNL, last byte of previous buffer was an
	 * unescaped CR and 1st byte of current buffer is NL. We check for
	 * that here.
	 */
	if (cstate->eol_type == EOL_CRNL)
	{
		/* if we started scanning from the 1st byte of the buffer */
		if (cstate->begloc == cstate->raw_buf)
		{
			/* and had a CR in last byte of prev buf */
			if (cstate->cr_in_prevbuf)
			{
				/*
				 * if this 1st byte in buffer is 2nd byte of line end sequence
				 * (linefeed)
				 */
				if (*(cstate->begloc) == cstate->eol_ch[1])
				{
					/*
					 * load that one linefeed byte and indicate we are done
					 * with the data line
					 */
					appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, 1);
					cstate->raw_buf_index++;
					cstate->begloc++;

					cstate->line_done = true;
					cstate->esc_in_prevbuf = false;
					cstate->cr_in_prevbuf = false;

					return cstate->line_done;
				}
			}

			cstate->cr_in_prevbuf = false;
		}
	}

	/*
	 * (we need a loop so that if eol_ch is found, but we are in quotes,
		* we can search for the next eol_ch)
	 */
	while (true)
	{
		/* reached end of buffer */
		if ((cstate->endloc = scanCSVLine(cstate, cstate->begloc, cstate->eol_ch[0], escapec, quotec, bytesread - cstate->raw_buf_index)) == NULL)
		{
			linesize = RAW_BUF_SIZE - (cstate->begloc - cstate->raw_buf);
			appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, linesize);

			if (cstate->last_was_esc)
				cstate->esc_in_prevbuf = true;

			if (cstate->line_buf.len > 1)
			{
				char	   *last_ch = cstate->line_buf.data + cstate->line_buf.len - 1; /* before terminating \0 */

				if (*last_ch == '\r')
				{
					if (cstate->eol_type == EOL_CRNL)
						cstate->cr_in_prevbuf = true;
				}
			}

			cstate->line_done = false;
			cstate->raw_buf_done = true;
			break;
		}
		else
			/* found 1st eol char in raw_buf. */
		{
			bool		eol_found = true;

			/*
			 * Load that piece of data (potentially a data line) into the line buffer,
			 * and update the pointers for the next scan.
			 */
			linesize = cstate->endloc - cstate->begloc + 1;
			appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, linesize);
			cstate->raw_buf_index += linesize;
			cstate->begloc = cstate->endloc + 1;

			/* end of line only if not in quotes */
			if (cstate->in_quote)
			{
				cstate->line_done = false;

				/* update file line for error message */

				/*
				 * TODO: for dos line end we need to do check before
				 * incrementing!
				 */
				cstate->cur_lineno++;

				/* buf done, but still in quote */
				if (cstate->raw_buf_index >= bytesread)
				{
					cstate->raw_buf_done = true;
					break;
				}
			}
			else
			{
				/* if dos eol, check for '\n' after the '\r' */
				if (cstate->eol_type == EOL_CRNL)
				{
					if (*(cstate->endloc + 1) == '\n')
					{
						/* this is a line end */
						appendBinaryStringInfo(&cstate->line_buf, cstate->begloc, 1);	/* load that '\n' */
						cstate->raw_buf_index++;
						cstate->begloc++;
					}
					else
						/* just a CR, not a line end */
						eol_found = false;
				}

				/*
				 * in some cases, this end of line char happens to be the
				 * last character in the buffer. we need to catch that.
				 */
				if (cstate->raw_buf_index >= bytesread)
					cstate->raw_buf_done = true;

				/*
				 * if eol was found line is done
				 */
				if (eol_found)
				{
					cstate->line_done = true;
					break;
				}
			}
		}						/* end of found eol_ch */
	}

	/*
	 * Done reading the line. Convert it to server encoding.
	 */
	if (cstate->need_transcoding)
	{
		cvt = (char *) pg_client_to_server(cstate->line_buf.data,
										   cstate->line_buf.len);
		if (cvt != cstate->line_buf.data)
		{
			/* transfer converted data back to line_buf */
			cstate->line_buf.len = 0;
			cstate->line_buf.data[0] = '\0';
			cstate->line_buf.cursor = 0;
			appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
		}
	}

	/* indicate that conversion had occured */
	cstate->line_buf_converted = true;

	/*
	 * check if this line is an end marker -- "\."
	 */
	cstate->end_marker = false;

	switch (cstate->eol_type)
	{
		case EOL_NL:
			if (!strcmp(cstate->line_buf.data, "\\.\n"))
				cstate->end_marker = true;
			break;
		case EOL_CR:
			if (!strcmp(cstate->line_buf.data, "\\.\r"))
				cstate->end_marker = true;
			break;
		case EOL_CRNL:
			if (!strcmp(cstate->line_buf.data, "\\.\r\n"))
				cstate->end_marker = true;
			break;
		case EOL_UNKNOWN:
			break;
	}

	if (cstate->end_marker)
	{
		/*
		 * Reached end marker. In protocol version 3 we
		 * should ignore anything after \. up to protocol
		 * end of copy data.
		 */
		if (cstate->copy_dest == COPY_NEW_FE)
		{
			while (!cstate->fe_eof)
			{
				CopyGetData(cstate, cstate->raw_buf, RAW_BUF_SIZE);	/* eat data */
			}
		}

		cstate->fe_eof = true;
		/* we don't want to process a \. as data line, want to quit. */
		cstate->line_done = false;
		cstate->raw_buf_done = true;
	}

	return cstate->line_done;
}

/*
 * Detected the eol type by looking at the first data row.
 * Possible eol types are NL, CR, or CRNL. If eol type was
 * detected, it is set and a boolean true is returned to
 * indicated detection was successful. If the first data row
 * is longer than the input buffer, we return false and will
 * try again in the next buffer.
 */
static bool
DetectLineEnd(CopyState cstate, size_t bytesread)
{
	int			index = 0;
	char		c;
	char		quotec = '\0',
				escapec = '\0';
	bool		csv = false;

	if (cstate->quote)					/* CSV format */
	{
		csv = true;
		quotec = cstate->quote[0];
		escapec = cstate->escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}

	while (index < RAW_BUF_SIZE)
	{
		c = cstate->raw_buf[index];

		if (csv)
		{
			if (cstate->in_quote && c == escapec)
				cstate->last_was_esc = !cstate->last_was_esc;
			if (c == quotec && !cstate->last_was_esc)
				cstate->in_quote = !cstate->in_quote;
			if (c != escapec)
				cstate->last_was_esc = false;
		}

		if (c == '\n')
		{
			if (!csv || (csv && !cstate->in_quote))
			{
				cstate->eol_type = EOL_NL;
				cstate->eol_ch[0] = '\n';
				cstate->eol_ch[1] = '\0';

				cstate->in_quote = false;
				cstate->last_was_esc = false;
				return true;
			}
		}
		if (c == '\r')
		{
			if (!csv || (csv && !cstate->in_quote))
			{
				if (cstate->raw_buf[index + 1] == '\n')		/* always safe */
				{
					cstate->eol_type = EOL_CRNL;
					cstate->eol_ch[0] = '\r';
					cstate->eol_ch[1] = '\n';
				}
				else
				{
					cstate->eol_type = EOL_CR;
					cstate->eol_ch[0] = '\r';
					cstate->eol_ch[1] = '\0';
				}

				cstate->in_quote = false;
				cstate->last_was_esc = false;
				return true;
			}
		}

		index++;
	}

	return false;
}

/*
 *  Return decimal value for a hexadecimal digit
 */
static
int GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char)hex))
		return hex - '0';
	else
		return tolower((unsigned char)hex) - 'a' + 10;
}

/*
 * Read all TEXT attributes. Attributes are parsed from line_buf and
 * inserted (all at once) to attribute_buf, while saving pointers to
 * each attribute's starting position.
 *
 * When this routine finishes execution both the nulls array and
 * the attr_offsets array are updated. The attr_offsets will include
 * the offset from the beginning of the attribute array of which
 * each attribute begins. If a specific attribute is not used for this
 * COPY command (ommitted from the column list), a value of 0 will be assigned.
 * For example: for table foo(a,b,c,d,e) and COPY foo(a,b,e)
 * attr_offsets may look something like this after this routine
 * returns: [0,20,0,0,55]. That means that column "a" value starts
 * at byte offset 0, "b" in 20 and "e" in 55, in attribute_buf.
 *
 * In the attribute buffer (attribute_buf) each attribute
 * is terminated with a '\0', and therefore by using the attr_offsets
 * array we could point to a beginning of an attribute and have it
 * behave as a C string, much like previously done in COPY.
 *
 * Another aspect to improving performance is reducing the frequency
 * of data load into buffers. The original COPY read attribute code
 * loaded a character at a time. In here we try to load a chunk of data
 * at a time. Usually a chunk will include a full data row
 * (unless we have an escaped delim). That effectively reduces the number of
 * loads by a factor of number of bytes per row. This improves performance
 * greatly, unfortunately it add more complexity to the code.
 *
 * Global participants in parsing logic:
 *
 * line_buf.cursor -- an offset from beginning of the line buffer
 * that indicates where we are about to begin the next scan. Note that
 * if we have WITH OIDS this cursor is already shifted past the first
 * OID attribute.
 *
 * attribute_buf.cursor -- an offset from the beginning of the
 * attribute buffer that indicates where the current attribute begins.
 */
static void
CopyReadAttributesText(CopyState cstate, char *nulls,
					   int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr)
{
	char		delimc = cstate->delim[0];		/* delimiter character */
	char		escapec = cstate->escape[0];	/* escape character    */
	char	   *scan_start;		/* pointer to line buffer for scan start. */
	char	   *scan_end;		/* pointer to line buffer where char was found */
	char	   *stop;
	char	   *scanner;
	int			attr_pre_len;	/* attr raw len, before processing escapes */
	int			attr_post_len;	/* current attr len after escaping */
	int			m;				/* attribute index being parsed */
	int			bytes_remaining;/* num bytes remaining to be scanned in line
								 * buf */
	int			chunk_start;	/* offset to beginning of line chunk to load */
	int			chunk_len;		/* length of chunk of data to load to attr buf */
	int			oct_val;		/* byte value for octal escapes */
	int			hex_val;
	int			attnum;			/* attribute number being parsed */
	ListCell   *cur;			/* cursor to attribute list used for this COPY */
	int			attribute;

	/*
	 * init variables for attribute scan
	 */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';
	cstate->attribute_buf.cursor = 0;
	/* cursor is now > 0 if we copy WITH OIDS */
	scan_start = cstate->line_buf.data + cstate->line_buf.cursor;
	cur = list_head(cstate->attnumlist);
	
	/* check for zero column table case */
	if(num_phys_attrs > 0)
	{
		attnum = lfirst_int(cur);
		m = attnum - 1;
	}
	
	chunk_start = cstate->line_buf.cursor;
	chunk_len = 0;
	attr_pre_len = 0;
	attr_post_len = 0;
	attribute = 1;

	if (cstate->escape_off)
		escapec = delimc;		/* look only for delimiters, escapes are
								 * disabled */

	/*
	 * Scan through the line buffer to read all attributes data
	 */
	while (cstate->line_buf.cursor < cstate->line_buf.len)
	{
		bytes_remaining = cstate->line_buf.len - cstate->line_buf.cursor;
		stop = scan_start + bytes_remaining;

		for (scanner = scan_start; *scanner != delimc && *scanner != escapec && scanner < stop; scanner++)
			;

		scan_end = (*scanner != '\0' ? (char *) scanner : NULL);

		if (scan_end == NULL)
		{
			/* GOT TO END OF LINE BUFFER */

			if (cur == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("extra data after last expected column")));

			attnum = lfirst_int(cur);
			m = attnum - 1;

			/* don't count eol char(s) in attr and chunk len calculation */
			if (cstate->eol_type == EOL_CRNL)
			{
				attr_pre_len += bytes_remaining - 2;
				chunk_len = cstate->line_buf.len - chunk_start - 2;
			}
			else
			{
				attr_pre_len += bytes_remaining - 1;
				chunk_len = cstate->line_buf.len - chunk_start - 1;
			}

			/* check if this is a NULL value or data value (assumed NULL) */
			if (attr_pre_len == cstate->null_print_len
				&&
				strncmp(cstate->line_buf.data + cstate->line_buf.len - attr_pre_len - 1, cstate->null_print, attr_pre_len)
				== 0)
				nulls[m] = 'n';
			else
				nulls[m] = ' ';

			attr_offsets[m] = cstate->attribute_buf.cursor;


			/* load the last chunk, the whole buffer in most cases */
			appendBinaryStringInfo(&cstate->attribute_buf, cstate->line_buf.data + chunk_start, chunk_len);

			cstate->line_buf.cursor += attr_pre_len + 2;		/* skip eol char and
														 * '\0' to exit loop */

			if (lnext(cur) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("missing data for column \"%s\"",
					   NameStr(attr[lfirst_int(lnext(cur)) - 1]->attname))));
		}
		else
			/* FOUND A DELIMITER OR ESCAPE */
		{
			if (cur == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("extra data after last expected column")));

			if (*scan_end == delimc)	/* found a delimiter */
			{
				attnum = lfirst_int(cur);
				m = attnum - 1;

				/* (we don't include the delimiter ch in length) */
				attr_pre_len += scan_end - scan_start;
				/* (we don't include the delimiter ch in length) */
				attr_post_len += scan_end - scan_start;

				/* check if this is a null print or data (assumed NULL) */
				if (attr_pre_len == cstate->null_print_len &&
					strncmp(scan_end - attr_pre_len, cstate->null_print, attr_pre_len) == 0)
					nulls[m] = 'n';
				else
					nulls[m] = ' ';

				/* set the pointer to next attribute position */
				attr_offsets[m] = cstate->attribute_buf.cursor;

				/*
				 * update buffer cursors to our current location, +1 to skip
				 * the delimc
				 */
				cstate->line_buf.cursor = scan_end - cstate->line_buf.data + 1;
				cstate->attribute_buf.cursor += attr_post_len + 1;

				/* prepare scan for next attr */
				scan_start = cstate->line_buf.data + cstate->line_buf.cursor;
				cur = lnext(cur);
				attr_pre_len = 0;
				attr_post_len = 0;

				/*
				 * for the dispatcher - stop parsing once we have
				 * all the hash field values. We don't need the rest.
				 */
				if (Mpp_role == MPP_ROLE_DISPATCH)
				{
					if (attribute == cstate->last_hash_field)
					{
						/*
						 * load the chunk from chunk_start to end of current
						 * attribute, not including delimiter
						 */
						chunk_len = cstate->line_buf.cursor - chunk_start - 1;
						appendBinaryStringInfo(&cstate->attribute_buf, cstate->line_buf.data + chunk_start, chunk_len);
						break;
					}
				}

				attribute++;
			}
			else
				/* found an escape character */
			{
				char		nextc = *(scan_end + 1);
				char		newc;
				int			skip = 2;

				chunk_len = (scan_end - cstate->line_buf.data) - chunk_start + 1;

				/* load a chunk of data */
				appendBinaryStringInfo(&cstate->attribute_buf, cstate->line_buf.data + chunk_start, chunk_len);

				switch (nextc)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						/* handle \013 */
						oct_val = OCTVALUE(nextc);
						nextc = *(scan_end + 2);

						/*
						 * (no need for out bad access check since line if
						 * buffered)
						 */
						if (ISOCTAL(nextc))
						{
							skip++;
							oct_val = (oct_val << 3) + OCTVALUE(nextc);
							nextc = *(scan_end + 3);
							if (ISOCTAL(nextc))
							{
								skip++;
								oct_val = (oct_val << 3) + OCTVALUE(nextc);
							}
						}
						newc = oct_val & 0377;	/* the escaped byte value */
						break;
					case 'x':
						/* Handle \x3F */
						hex_val = 0; /* init */
						nextc = *(scan_end + 2); /* get char after 'x' */
						
						if (isxdigit((unsigned char)nextc))
						{
							skip++;
							hex_val = GetDecimalFromHex(nextc);
							nextc = *(scan_end + 3); /* get second char */
							
							if (isxdigit((unsigned char)nextc))
							{
								skip++;
								hex_val = (hex_val << 4) + GetDecimalFromHex(nextc);
							}
							newc = hex_val & 0xff;
						}
						else
						{
							newc = 'x';
						}
						break;
							
					case 'b':
						newc = '\b';
						break;
					case 'f':
						newc = '\f';
						break;
					case 'n':
						newc = '\n';
						break;
					case 'r':
						newc = '\r';
						break;
					case 't':
						newc = '\t';
						break;
					case 'v':
						newc = '\v';
						break;
					default:
						if (nextc == delimc)
							newc = delimc;
						else if (nextc == escapec)
							newc = escapec;
						else
							/* no escape sequence, take next char literaly */
							newc = nextc;
						break;
				}

				/* update to current length, add escape and escaped chars  */
				attr_pre_len += scan_end - scan_start + 2;
				/* update to current length, escaped char */
				attr_post_len += scan_end - scan_start + 1;

				/*
				 * Need to get rid of the escape character. This is done by
				 * loading the chunk up to including the escape character
				 * into the attribute buffer. Then overwritting the escape char
				 * with the escaped sequence or char, and continuing to scan
				 * from *after* the char than is after the escape in line buf.
				 */
				*(cstate->attribute_buf.data + cstate->attribute_buf.len - 1) = newc;
				cstate->line_buf.cursor = scan_end - cstate->line_buf.data + skip;
				scan_start = scan_end + skip;
				chunk_start = cstate->line_buf.cursor;
				chunk_len = 0;
			}

		}						/* end delimiter/backslash */

	}							/* end line buffer scan. */

	/*
	 * Replace all delimiters with NULL for string termination.
	 * NOTE: only delimiters (NOT necessarily all delimc) are replaced.
	 * Example (delimc = '|'):
	 * - Before:  f  1	|  f  \|  2  |	f  3
	 * - After :  f  1 \0  f   |  2 \0	f  3
	 */
	for (attribute = 0; attribute < num_phys_attrs; attribute++)
	{
		if (attr_offsets[attribute] != 0)
			*(cstate->attribute_buf.data + attr_offsets[attribute] - 1) = '\0';
	}

}

/*
 * Read a binary attribute
 */
static Datum
CopyReadBinaryAttribute(CopyState cstate, int column_no, FmgrInfo *flinfo, Oid typioparam,
						int32 typmod, bool *isnull)
{
	int32		fld_size;
	uint32		buf;
	Datum		result;

	if(!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	
	if (Mpp_role == MPP_ROLE_DISPATCH)
	{
		buf = htonl((uint32) fld_size); /* change to network byte order */
		appendBinaryStringInfo(&cstate->line_buf, (char *) &buf, sizeof(buf));
	}

	if (fld_size == -1)
	{
		*isnull = true;
		return (Datum) 0;
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));


	/* reset attribute_buf to empty, and load raw data in it */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';
	cstate->attribute_buf.cursor = 0;

	enlargeStringInfo(&cstate->attribute_buf, fld_size);

	CopyGetData(cstate, cstate->attribute_buf.data, fld_size);
	if (cstate->fe_eof)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	/* load raw data into line buf for dispatching later */
	if (Mpp_role == MPP_ROLE_DISPATCH)
		appendBinaryStringInfo(&cstate->line_buf, cstate->attribute_buf.data, fld_size);

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = FunctionCall3(flinfo,
						   PointerGetDatum(&cstate->attribute_buf),
						   ObjectIdGetDatum(typioparam),
						   Int32GetDatum(typmod));

	/* Trouble if it didn't eat the whole buffer */
	if (cstate->attribute_buf.cursor != cstate->attribute_buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("incorrect binary data format")));

	*isnull = false;
	return result;
}

/*
 * Read all the attributes of the data line in CSV mode,
 * performing de-escaping as needed. Escaping does not follow the normal
 * PostgreSQL text mode, but instead "standard" (i.e. common) CSV usage.
 *
 * Quoted fields can span lines, in which case the line end is embedded
 * in the returned string.
 *
 * null_print is the null marker string.  Note that this is compared to
 * the pre-de-escaped input string (thus if it is quoted it is not a NULL).
 *----------
 */
static void
CopyReadAttributesCSV(CopyState cstate, char *nulls, int *attr_offsets,
					  int num_phys_attrs, Form_pg_attribute *attr)
{
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];
	char		c;
	int			start_cursor = cstate->line_buf.cursor;
	int			end_cursor = start_cursor;
	int			input_len;
	bool		in_quote = false;
	bool		saw_quote = false;
	int			attnum;			/* attribute number being parsed */
	int			m;				/* attribute index being parsed */
	ListCell   *cur;			/* cursor to attribute list used for this COPY */
	int			attribute;

	/*
	 * init variables for attribute scan
	 */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';
	cstate->attribute_buf.cursor = 0;

	cur = list_head(cstate->attnumlist);
	
	if(num_phys_attrs > 0)
	{		
		attnum = lfirst_int(cur);
		m = attnum - 1;
	}

	input_len = 0;
	attribute = 1;


	for (;;)
	{
		end_cursor = cstate->line_buf.cursor;

		/* finished processing attributes in line */
		if (cstate->line_buf.cursor >= cstate->line_buf.len - 1)
		{
				
			/* check whether raw input matched null marker */
			input_len = end_cursor - start_cursor;
			
			/* make sure this is not a zero column table*/
			if(num_phys_attrs > 0)
			{
				if (!saw_quote && input_len == cstate->null_print_len &&
					strncmp(&cstate->line_buf.data[start_cursor], cstate->null_print, input_len) == 0)
					nulls[m] = 'n';
				else
					nulls[m] = ' ';				
			}

			/* if zero column table and data is trying to get in */
			if(num_phys_attrs == 0 && input_len > 0)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("extra data after last expected column")));
			
			if (in_quote)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unterminated CSV quoted field")));

			if (lnext(cur) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("missing data for column \"%s\"",
								NameStr(attr[m + 1]->attname))));
			break;
		}

		c = cstate->line_buf.data[cstate->line_buf.cursor++];

		/* unquoted field delimiter  */
		if (!in_quote && c == delimc)
		{
			/* check whether raw input matched null marker */
			input_len = end_cursor - start_cursor;
			
			if(num_phys_attrs > 0)
			{
				if (!saw_quote && input_len == cstate->null_print_len &&
				strncmp(&cstate->line_buf.data[start_cursor], cstate->null_print, input_len) == 0)
					nulls[m] = 'n';
				else
					nulls[m] = ' ';
			}

			/* terminate attr string with '\0' */
			appendStringInfoCharMacro(&cstate->attribute_buf, '\0');
			cstate->attribute_buf.cursor++;

			/* setup next attribute scan */
			cur = lnext(cur);

			if (cur == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("extra data after last expected column")));

			saw_quote = false;
			
			if(num_phys_attrs > 0)
			{
				attnum = lfirst_int(cur);
				m = attnum - 1;
				attr_offsets[m] = cstate->attribute_buf.cursor;
			}
			
			start_cursor = cstate->line_buf.cursor;

			/*
			 * for the dispatcher - stop parsing once we have
			 * all the hash field values. We don't need the rest.
			 */
			if (Mpp_role == MPP_ROLE_DISPATCH)
			{
				if (attribute == cstate->last_hash_field)
					break;
			}

			attribute++;
			continue;
		}

		/* start of quoted field (or part of field) */
		if (!in_quote && c == quotec)
		{
			saw_quote = true;
			in_quote = true;
			continue;
		}

		/* escape within a quoted field */
		if (in_quote && c == escapec)
		{
			/*
			 * peek at the next char if available, and escape it if it is
			 * an escape char or a quote char
			 */
			if (cstate->line_buf.cursor <= cstate->line_buf.len)
			{
				char		nextc = cstate->line_buf.data[cstate->line_buf.cursor];

				if (nextc == escapec || nextc == quotec)
				{
					appendStringInfoCharMacro(&cstate->attribute_buf, nextc);
					cstate->line_buf.cursor++;
					cstate->attribute_buf.cursor++;
					continue;
				}
			}
		}

		/*
		 * end of quoted field. Must do this test after testing for escape
		 * in case quote char and escape char are the same (which is the
		 * common case).
		 */
		if (in_quote && c == quotec)
		{
			in_quote = false;
			continue;
		}
		appendStringInfoCharMacro(&cstate->attribute_buf, c);
		cstate->attribute_buf.cursor++;
	}

}

/*
 * Read the first attribute. This is mainly used to maintain support
 * for an OID column. All the rest of the columns will be read at once with
 * CopyReadAttributesText.
 */
static char *
CopyReadOidAttr(CopyState cstate, bool *isnull)
{
	char		delimc = cstate->delim[0];
	char	   *start_loc = cstate->line_buf.data + cstate->line_buf.cursor;
	char	   *end_loc;
	int			attr_len = 0;
	int			bytes_remaining;

	/* reset attribute buf to empty */
	cstate->attribute_buf.len = 0;
	cstate->attribute_buf.data[0] = '\0';
	cstate->attribute_buf.cursor = 0;

	/* # of bytes that were not yet processed in this line */
	bytes_remaining = cstate->line_buf.len - cstate->line_buf.cursor;

	/* got to end of line */
	if ((end_loc = scanTextLine(cstate, start_loc, delimc, bytes_remaining)) == NULL)
	{
		attr_len = bytes_remaining - 1; /* don't count '\n' in len calculation */
		appendBinaryStringInfo(&cstate->attribute_buf, start_loc, attr_len);
		cstate->line_buf.cursor += attr_len + 2;		/* skip '\n' and '\0' */
	}
	else
		/* found a delimiter */
	{
		/*
		 * (we don't care if delim was preceded with a backslash, because it's
		 * an invalid OID anyway)
		 */

		attr_len = end_loc - start_loc; /* we don't include the delimiter ch */

		appendBinaryStringInfo(&cstate->attribute_buf, start_loc, attr_len);
		cstate->line_buf.cursor += attr_len + 1;
	}


	/* check whether raw input matched null marker */
	if (attr_len == cstate->null_print_len && strncmp(start_loc, cstate->null_print, attr_len) == 0)
		*isnull = true;
	else
		*isnull = false;

	return cstate->attribute_buf.data;
}


/*
 * Send text representation of one attribute, with conversion and escaping
 */
static void
CopyAttributeOutText(CopyState cstate, char *server_string)
{
	char	   *string;
	char		c;
	char		delimc = cstate->delim[0];
	int			mblen;

	if (cstate->need_transcoding)
		string = pg_server_to_client(server_string, strlen(server_string));
	else
		string = server_string;

	for (; (c = *string) != '\0'; string += mblen)
	{
		mblen = 1;

		switch (c)
		{
			case '\b':
				CopySendString(cstate, "\\b");
				break;
			case '\f':
				CopySendString(cstate, "\\f");
				break;
			case '\n':
				CopySendString(cstate, "\\n");
				break;
			case '\r':
				CopySendString(cstate, "\\r");
				break;
			case '\t':
				CopySendString(cstate, "\\t");
				break;
			case '\v':
				CopySendString(cstate, "\\v");
				break;
			case '\\':
				CopySendString(cstate, "\\\\");
				break;
			default:
				if (c == delimc)
					CopySendChar(cstate, '\\');

				/*
				 * We can skip pg_encoding_mblen() overhead when encoding is
				 * safe, because in valid backend encodings, extra bytes of a
				 * multibyte character never look like ASCII.
				 */
				if (cstate->client_only_encoding)
					mblen = pg_encoding_mblen(cstate->client_encoding, string);
				CopySendData(cstate, string, mblen);
				break;
		}
	}
}

/*
 * Send CSV representation of one attribute, with conversion and
 * CSV type escaping
 */
static void
CopyAttributeOutCSV(CopyState cstate, char *server_string,
					bool use_quote, bool single_attr)
{
	char	   *string;
	char		c;
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];
	char	   *tstring;
	int			mblen;

	/* force quoting if it matches null_print */
	if (!use_quote && strcmp(server_string, cstate->null_print) == 0)
		use_quote = true;

	if (cstate->need_transcoding)
		string = pg_server_to_client(server_string, strlen(server_string));
	else
		string = server_string;

	/*
	 * have to run through the string twice, first time to see if it needs
	 * quoting, second to actually send it
	 */
	if (!use_quote)
	{
		/*
		 *	Because '\.' can be a data value, quote it if it appears
		 *	alone on a line so it is not interpreted as the end-of-data
		 *	marker.
		 */
		if (single_attr && strcmp(string, "\\.") == 0)
 			use_quote = true;
 		else
 		{
			for (tstring = string; (c = *tstring) != '\0'; tstring += mblen)
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (cstate->client_only_encoding)
					mblen = pg_encoding_mblen(cstate->client_encoding, tstring);
				else
					mblen = 1;
			}
		}
	}

	if (use_quote)
		CopySendChar(cstate, quotec);

	for (; (c = *string) != '\0'; string += mblen)
	{
		if (use_quote && (c == quotec || c == escapec))
			CopySendChar(cstate, escapec);
		if (cstate->client_only_encoding)
			mblen = pg_encoding_mblen(cstate->client_encoding, string);
		else
			mblen = 1;
		CopySendData(cstate, string, mblen);
	}

	if (use_quote)
		CopySendChar(cstate, quotec);
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 */
static List *
CopyGetAttnums(Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		TupleDesc	tupDesc = RelationGetDescr(rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (attr[i]->attisdropped)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;

			/* Lookup column name, ereport on failure */
			/* Note we disallow system columns here */
			attnum = attnameAttNum(rel, name, false);
			/* Check for duplicates */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
}


/*
 * The following are custom versions of the string function strchr().
 * As opposed to the original strchr which searches through
 * a string until the target character is found, or a NULL is
 * found, this version will not return when a NULL is found.
 * Instead it will search through a pre-defined length of
 * bytes and will return only if the target character(s) is reached.
 *
 * If our client encoding is not a supported server encoding, we
 * know that it is not safe to look at each character as trailing
 * byte in a multibyte character may be a 7-bit ASCII equivalent.
 * Therefore we use pg_encoding_mblen to skip to the end of the
 * character.
 *
 * returns:
 *	 pointer to c - if c is located within the string.
 *	 NULL - if c was not found in specified length of search. Note:
 *			this DOESN'T mean that a '\0' was reached.
 */
char *
scanTextLine(CopyState cstate, const char *s, char eol, size_t len)
{
	const char *start;

	if (cstate->client_only_encoding && !cstate->line_buf_converted)
	{
		int			mblen = pg_encoding_mblen(cstate->client_encoding, s);

		for (start = s; *s != eol && s < start + len; s += mblen)
			mblen = pg_encoding_mblen(cstate->client_encoding, s);

		return ((*s == eol) ? (char *) s : NULL);
	}
	else
		return memchr(s, eol, len);
}


char *
scanCSVLine(CopyState cstate, const char *s, char eol, char escapec, char quotec, size_t len)
{
	const char *start;

	if (cstate->client_only_encoding && !cstate->line_buf_converted)
	{
		int			mblen = pg_encoding_mblen(cstate->client_encoding, s);

		for (start = s; *s != eol && s < start + len; s += mblen)
		{
			if (cstate->in_quote && *s == escapec)
				cstate->last_was_esc = !cstate->last_was_esc;
			if (*s == quotec && !cstate->last_was_esc)
				cstate->in_quote = !cstate->in_quote;
			if (*s != escapec)
				cstate->last_was_esc = false;

			mblen = pg_encoding_mblen(cstate->client_encoding, s);
		}
	}
	else
		/* safe to scroll byte by byte */
	{
		for (start = s; *s != eol && s < start + len; s++)
		{
			if (cstate->in_quote && *s == escapec)
				cstate->last_was_esc = !cstate->last_was_esc;
			if (*s == quotec && !cstate->last_was_esc)
				cstate->in_quote = !cstate->in_quote;
			if (*s != escapec)
				cstate->last_was_esc = false;
		}

	}

	if (*s == eol)
		cstate->last_was_esc = false;

	return ((*s == eol) ? (char *) s : NULL);
}

/* remove end of line chars from end of line buf */
static void truncateEol(CopyState cstate)
{
	if(cstate->eol_type == EOL_CRNL)
	{
		cstate->line_buf.data[cstate->line_buf.len - 2] = '\0';
		cstate->line_buf.data[cstate->line_buf.len - 1] = '\0';
		cstate->line_buf.len -= 2;
	}
	else
	{
		cstate->line_buf.data[cstate->line_buf.len - 1] = '\0';
		cstate->line_buf.len--;
	}
}

/* add end of line chars to end line buf */
static void concatenateEol(CopyState cstate)
{
	switch (cstate->eol_type)
	{
		case EOL_NL:
			appendStringInfo(&cstate->line_buf, "\n");
			break;
		case EOL_CR:
			appendStringInfo(&cstate->line_buf, "\r");
			break;
		case EOL_CRNL:
			appendStringInfo(&cstate->line_buf, "\r\n");
			break;
		case EOL_UNKNOWN:
			/* TODO: find a way to throw an error safely */
			break;
			
	}
}

