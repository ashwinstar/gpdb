/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/copy.c,v 1.236 2004/12/31 21:59:41 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_shadow.h"
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
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"
#include "cdb/cdblink.h"
#include "cdb/cdbcopy.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbhash.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')
#define COPY_BUF_SIZE 65536

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

static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";

/*
 * Static communication variables ... pretty grotty, but COPY has
 * never been reentrant...
 */
static CopyDest copy_dest;
static FILE *copy_file;			/* used if copy_dest == COPY_FILE */
static StringInfo copy_msgbuf;	/* used if copy_dest == COPY_NEW_FE */
static bool fe_eof;				/* true if detected end of copy data */
static EolType eol_type;		/* EOL type of input */
static int	client_encoding;	/* remote side's character encoding */
static int	server_encoding;	/* local encoding */
static bool embedded_line_warning;
static char	eol_ch[2];			/* The byte values of the 1 or 2 eol bytes */
static bool client_encoding_only; /* true if client encoding is a non
* supported server encoding */

/* these are just for error messages, see copy_in_error_callback */
static bool copy_binary;		/* is it a binary copy? */
static const char *copy_relname;	/* table name for error messages */
static int	copy_lineno;		/* line number for error messages */
static const char *copy_attname;	/* current att for error messages */

/*
 * Static variables for buffered input parsing 
 */
static char input_buf[COPY_BUF_SIZE + 1]; /* extra byte for '\0' */
static int	buffer_index;		/* input buffer index */
static bool end_marker;
static char *begloc;
static char *endloc;
static bool buf_done;			/* finished processing the current buffer */
static bool line_done;			/* finished processing the whole line or
* stopped in the middle */
static int last_hash_field;
/* these are for CSV format */
static bool	in_quote;
static bool last_was_esc;
/* these are for TEXT format */
static bool esc_in_prevbuf;		/* escape was last character of the data
* input buffer */
static bool cr_in_prevbuf;		/* CR was last character of the data input
* buffer */

/*
 * These static variables are used to avoid incurring overhead for each
 * attribute processed.  attr_buf is reused on each CopyReadAttribute
 * call to hold the string being read in.  Under normal use it will soon
 * grow to a suitable size, and then we will avoid palloc/pfree overhead
 * for subsequent attributes.  Note that CopyReadAttribute returns a pointer
 * to attr_buf's data buffer!
 */
static StringInfoData attr_buf;

/*
 * Similarly, line_buf holds the whole input line being processed (its
 * cursor field points to the next character to be read by CopyReadAttribute).
 * The input cycle is first to read the whole line into line_buf, convert it
 * to server encoding, and then extract individual attribute fields into
 * attr_buf.  (We used to have CopyReadAttribute read the input source
 * directly, but that caused a lot of encoding issues and unnecessary logic
 * complexity.)
 */
static StringInfoData line_buf;
static bool line_buf_converted;

/* non-export function prototypes */
static void DoCopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
		             char *delim, char *null_print, bool csv_mode, char *quote,
		             char *escape, List *force_quote_atts, bool fe_copy);
static void CopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
				   char *delim, char *null_print, bool csv_mode, char *quote, char *escape,
				   List *force_quote_atts);
static void CopyFromDispatch(Relation rel, List *attnumlist, bool binary, bool oids,
			             char *delim, char *null_print, bool csv_mode, char *quote,
						 char *escape, List *force_notnull_atts, bool escape_off);
static void CopyFromExec(Relation rel, List *attnumlist, bool binary, bool oids,
			             char *delim, char *null_print, bool csv_mode, char *quote,
						 char *escape, List *force_notnull_atts, bool escape_off);
static bool CopyReadLineText(size_t bytesread, char *escape, bool escape_off);
static bool CopyReadLineCSV(size_t bytesread, char *quote, char *escape);
static void CopyReadAttributesText(const char *delim, const char *escape, const char *null_print,
								   int null_print_len, char *nulls, List *attnumlist, 
								   int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr,
								   bool escape_off);
static void CopyReadAttributesCSV(const char *delim, const char *null_print, char *quote,
								  char *escape, int null_print_len, char *nulls, List *attnumlist, 
								  int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr);
static Datum CopyReadBinaryAttribute(int column_no, FmgrInfo *flinfo,
									 Oid typioparam, bool *isnull);
static char *CopyReadOidAttr(const char *delim, const char *null_print, int null_print_len,
				             bool *isnull);
static void CopyAttributeOut(char *string, char *delim);
static void CopyAttributeOutCSV(char *string, char *delim, char *quote,
					            char *escape, bool force_quote);
static List *CopyGetAttnums(Relation rel, List *attnamelist);
static void limit_printout_length(StringInfo buf);
static bool DetectLineEnd(size_t bytesread, char *quote, char *escape);


/* Internal communications functions */
static void SendCopyBegin(bool binary, int natts);
static void ReceiveCopyBegin(bool binary, int natts);
static void SendCopyEnd(bool binary);
static void CopySendData(void *databuf, int datasize);
static void CopySendString(const char *str);
static void CopySendChar(char c);
static void CopySendEndOfRow(bool binary);
static int  CopyGetData(void *databuf, int datasize);

#define CopyGetEof()  (fe_eof)
static void CopySendInt32(int32 val);
static int32 CopyGetInt32(void);
static void CopySendInt16(int16 val);
static int16 CopyGetInt16(void);

/* byte scaning utils */
static char *scanTextLine(const char *s, char c, size_t len);
static char *scanCSVLine(const char *s, char c1, char c2, char c3, size_t len);


/*
 * Send copy start/stop messages for frontend copies.  These have changed
 * in past protocol redesigns.
 */
static void
SendCopyBegin(bool binary, int natts)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int16		format = (binary ? 1 : 0);
		int			i;

		pq_beginmessage(&buf, 'H');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		copy_dest = COPY_NEW_FE;
		copy_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('H');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('B');
		/* grottiness needed for old COPY OUT protocol */
		pq_startcopyout();
		copy_dest = COPY_OLD_FE;
	}
}

static void
ReceiveCopyBegin(bool binary, int natts)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* new way */
		StringInfoData buf;
		int16		format = (binary ? 1 : 0);
		int			i;

		pq_beginmessage(&buf, 'G');
		pq_sendbyte(&buf, format);		/* overall format */
		pq_sendint(&buf, natts, 2);
		for (i = 0; i < natts; i++)
			pq_sendint(&buf, format, 2);		/* per-column formats */
		pq_endmessage(&buf);
		copy_dest = COPY_NEW_FE;
		copy_msgbuf = makeStringInfo();
	}
	else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
	{
		/* old way */
		if (binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('G');
		copy_dest = COPY_OLD_FE;
	}
	else
	{
		/* very old way */
		if (binary)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY BINARY is not supported to stdout or from stdin")));
		pq_putemptymessage('D');
		copy_dest = COPY_OLD_FE;
	}
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

static void
SendCopyEnd(bool binary)
{
	if (copy_dest == COPY_NEW_FE)
	{
		if (binary)
		{
			/* Need to flush out file trailer word */
			CopySendEndOfRow(true);
		}
		else
		{
			/* Shouldn't have any unsent data */
			Assert(copy_msgbuf->len == 0);
		}
		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		/* The FE/BE protocol uses \n as newline for all platforms */
		CopySendData("\\.\n", 3);
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
CopySendData(void *databuf, int datasize)
{
	switch (copy_dest)
	{
		case COPY_FILE:
			fwrite(databuf, datasize, 1, copy_file);
			if (ferror(copy_file))
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
			appendBinaryStringInfo(copy_msgbuf, (char *) databuf, datasize);
			break;
	}
}

static void
CopySendString(const char *str)
{
	CopySendData((void *) str, strlen(str));
}

static void
CopySendChar(char c)
{
	CopySendData(&c, 1);
}

static void
CopySendEndOfRow(bool binary)
{
	switch (copy_dest)
	{
		case COPY_FILE:
			if (!binary)
			{
				/* Default line termination depends on platform */
#ifndef WIN32
				CopySendChar('\n');
#else
				CopySendString("\r\n");
#endif
			}
			break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!binary)
				CopySendChar('\n');
			break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!binary)
				CopySendChar('\n');
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', copy_msgbuf->data, copy_msgbuf->len);
			/* Reset copy_msgbuf to empty */
			copy_msgbuf->len = 0;
			copy_msgbuf->data[0] = '\0';
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
CopyGetData(void *databuf, int datasize)
{
	size_t		bytesread = 0;
	
	switch (copy_dest)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, datasize, copy_file);
			if (feof(copy_file))
				fe_eof = true;
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
			while (datasize > 0 && !fe_eof)
			{
				int			avail;
				
				while (copy_msgbuf->cursor >= copy_msgbuf->len)
				{
					/* Try to receive another message */
					int			mtype;
					
readmessage:
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection")));
					if (pq_getmessage(copy_msgbuf, 0))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection")));
					switch (mtype)
					{
						case 'd':		/* CopyData */
							break;
						case 'c':		/* CopyDone */
							/* COPY IN correctly terminated by frontend */
							fe_eof = true;
							return bytesread;
						case 'f':		/* CopyFail */
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(copy_msgbuf))));
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
				avail = copy_msgbuf->len - copy_msgbuf->cursor;
				if (avail > datasize)
					avail = datasize;
				pq_copymsgbytes(copy_msgbuf, databuf, avail);
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
CopySendInt32(int32 val)
{
	uint32		buf;

	buf = htonl((uint32) val);
	CopySendData(&buf, sizeof(buf));
}

/*
 * CopyGetInt32 reads an int32 that appears in network byte order
 */
static int32
CopyGetInt32(void)
{
	uint32		buf;

	CopyGetData(&buf, sizeof(buf));
	
	/* load raw data into line buf for dispatching later */
	if(Mpp_role == MPP_ROLE_DISPATCH)
	   appendBinaryStringInfo(&line_buf, (char *)&buf, sizeof(buf));
	
	return (int32) ntohl(buf);
}

/*
 * CopySendInt16 sends an int16 in network byte order
 */
static void
CopySendInt16(int16 val)
{
	uint16		buf;

	buf = htons((uint16) val);
	CopySendData(&buf, sizeof(buf));
}

/*
 * CopyGetInt16 reads an int16 that appears in network byte order
 */
static int16
CopyGetInt16(void)
{
	uint16		buf;

    CopyGetData(&buf, sizeof(buf));
	
	/* load raw data into line buf for dispatching later */
	if(Mpp_role == MPP_ROLE_DISPATCH)
		appendBinaryStringInfo(&line_buf, (char *)&buf, sizeof(buf));
	
	return (int16) ntohs(buf);
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
void
DoCopy(const CopyStmt *stmt)
{
	RangeVar   *relation = stmt->relation;
	char	   *filename = stmt->filename;
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	ListCell   *option;
	List	   *attnamelist = stmt->attlist;
	List	   *attnumlist;
	bool		fe_copy = false;
	bool		binary = false;
	bool		oids = false;
	bool		escape_off = false;
	bool		csv_mode = false;
	char	   *delim = NULL;
	char	   *quote = NULL;
	char	   *escape = NULL;
	char	   *null_print = NULL;
	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	List	   *force_quote_atts = NIL;
	List	   *force_notnull_atts = NIL;
	Relation	rel;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (oids)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (delim)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (null_print)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			null_print = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "csv") == 0)
		{
			if (csv_mode)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			csv_mode = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (quote)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			quote = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (escape)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			escape = strVal(defel->arg);
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

	if (binary && delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DELIMITER in BINARY mode")));

	if (binary && csv_mode)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify CSV in BINARY mode")));

	if (binary && null_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify NULL in BINARY mode")));

	/* Set defaults */
	if (!delim)
		delim = csv_mode ? "," : "\t";

	if (!null_print)
		null_print = csv_mode ? "" : "\\N";

	if (csv_mode)
	{
		if (!quote)
			quote = "\"";
		if (!escape)
			escape = quote;
	}
	
    if(!binary && !csv_mode && !escape)
		escape = "\\"; /* default escape for text mode */

	/*
	 * Only single-character delimiter strings are supported.
	 */
	if (strlen(delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single character")));
	
	/*
	 * Check quote
	 */
	if (!csv_mode && quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote available only in CSV mode")));

	if (csv_mode && strlen(quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote must be a single character")));

	/*
	 * Check escape
	 */
	if (binary && escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape is not available in binary mode")));
    
	if (csv_mode && strlen(escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape in CSV format must be a single character")));
	
	if (!csv_mode && !binary && strlen(escape) != 1)
	{
		if(!strcmp(escape, "OFF") || !strcmp(escape, "off"))
			escape_off = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY escape must be a single character, or [OFF/off] to disable escapes")));
	}
			
	/*
	 * Check force_quote
	 */
	if (!csv_mode && force_quote != NIL)
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
	if (!csv_mode && force_notnull != NIL)
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
	if (strchr(null_print, delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must not appear in the NULL specification")));

	/*
	 * Don't allow the csv quote char to appear in the null string.
	 */
	if (csv_mode && strchr(null_print, quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSV quote character must not appear in the NULL specification")));

	/*
	 * Open and lock the relation, using the appropriate lock type.
	 */
	rel = heap_openrv(relation, (is_from ? RowExclusiveLock : AccessShareLock));

	/* check read-only transaction */
	if (XactReadOnly && !is_from && !isTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("transaction is read-only")));

	/* Check permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));
	if (!pipe && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to COPY to or from a file"),
				 errhint("Anyone can COPY to stdout or from stdin. "
					   "psql's \\copy command also works for anyone.")));

	/*
	 * Don't allow COPY w/ OIDs to or from a table without them
	 */
	if (oids && !rel->rd_rel->relhasoids)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("table \"%s\" does not have OIDs",
						RelationGetRelationName(rel))));

	/*
	 * Generate or convert list of attributes to process
	 */
	attnumlist = CopyGetAttnums(rel, attnamelist);

	/*
	 * Check that FORCE QUOTE references valid COPY columns
	 */
	if (force_quote)
	{
		TupleDesc	tupDesc = RelationGetDescr(rel);
		Form_pg_attribute *attr = tupDesc->attrs;
		ListCell   *cur;

		force_quote_atts = CopyGetAttnums(rel, force_quote);

		foreach(cur, force_quote_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(attnumlist, attnum))
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
		TupleDesc	tupDesc = RelationGetDescr(rel);
		Form_pg_attribute *attr = tupDesc->attrs;

		force_notnull_atts = CopyGetAttnums(rel, force_notnull);

		foreach(cur, force_notnull_atts)
		{
			int			attnum = lfirst_int(cur);

			if (!list_member_int(attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE NOT NULL column \"%s\" not referenced by COPY",
								NameStr(attr[attnum - 1]->attname))));
		}
	}

	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&attr_buf);
	initStringInfo(&line_buf);
	line_buf_converted = false;

	client_encoding = pg_get_client_encoding();
	server_encoding = GetDatabaseEncoding();

	/*
	 * check if the client encoding is one of the 5 encodings
	 * that are not supported as a server encodings.
	 */
	switch (client_encoding)
	{
		case PG_SJIS:
		case PG_BIG5:
		case PG_GBK:
		case PG_UHC:
		case PG_GB18030:
			client_encoding_only = true;
			break;
		default:
			client_encoding_only = false;
	}
	
	copy_dest = COPY_FILE;		/* default */
	copy_file = NULL;
	copy_msgbuf = NULL;
	fe_eof = false;

	if (is_from)
	{							/* copy from file to database */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy to view \"%s\"",
								RelationGetRelationName(rel))));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy to sequence \"%s\"",
								RelationGetRelationName(rel))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					   errmsg("cannot copy to non-table relation \"%s\"",
							  RelationGetRelationName(rel))));
		}
		if (pipe)
		{
			if (whereToSendOutput == Remote)
				ReceiveCopyBegin(binary, list_length(attnumlist));
			else
				copy_file = stdin;
		}
		else
		{
			struct stat st;

			copy_file = AllocateFile(filename, PG_BINARY_R);

			if (copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							filename)));

			fstat(fileno(copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		if(Mpp_role == MPP_ROLE_DISPATCH)
		{
			CopyFromDispatch(rel, attnumlist, binary, oids, delim, null_print, csv_mode,
									   quote, escape, force_notnull_atts, escape_off);
		}		
		else /* execute or utility mode */
		{
			CopyFromExec(rel, attnumlist, binary, oids, delim, null_print, csv_mode,
									 quote, escape, force_notnull_atts, escape_off);
		}
	
		
	}
	else
	{							/* copy from database to file */
		
        if(Mpp_role == MPP_ROLE_DISPATCH)
            ereport(ERROR,
		           (errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
		            errmsg("COPY TO file from database is not yet supported in bizgres MPP dispatch mode")
				    ));

        if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from view \"%s\"",
								RelationGetRelationName(rel))));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from sequence \"%s\"",
								RelationGetRelationName(rel))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot copy from non-table relation \"%s\"",
							RelationGetRelationName(rel))));
		}
		if (pipe)
		{
			if (whereToSendOutput == Remote)
				fe_copy = true;
			else
				copy_file = stdout;
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
			copy_file = AllocateFile(filename, PG_BINARY_W);
			umask(oumask);

			if (copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for writing: %m",
							filename)));

			fstat(fileno(copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", filename)));
			}
		}

		DoCopyTo(rel, attnumlist, binary, oids, delim, null_print, csv_mode,
				 quote, escape, force_quote_atts, fe_copy);
	}

	if (!pipe)
	{
		/* we assume only the write case could fail here */
		if (FreeFile(copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							filename)));
	}
	pfree(attr_buf.data);
	pfree(line_buf.data);

	/*
	 * Close the relation.	If reading, we can release the AccessShareLock
	 * we got; if writing, we should hold the lock until end of
	 * transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	heap_close(rel, (is_from ? NoLock : AccessShareLock));
}


/*
 * This intermediate routine just exists to localize the effects of setjmp
 * so we don't need to plaster a lot of variables with "volatile".
 */
static void
DoCopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
		 char *delim, char *null_print, bool csv_mode, char *quote,
		 char *escape, List *force_quote_atts, bool fe_copy)
{
	PG_TRY();
	{
		if (fe_copy)
			SendCopyBegin(binary, list_length(attnumlist));

		CopyTo(rel, attnumlist, binary, oids, delim, null_print, csv_mode,
			   quote, escape, force_quote_atts);

		if (fe_copy)
			SendCopyEnd(binary);
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
 * Copy from relation TO file.
 */
static void
CopyTo(Relation rel, List *attnumlist, bool binary, bool oids,
	   char *delim, char *null_print, bool csv_mode, char *quote,
	   char *escape, List *force_quote_atts)
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

	tupDesc = rel->rd_att;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(attnumlist);
	embedded_line_warning = false;

	/*
	 * Get info about the columns we need to process.
	 */
	out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	typioparams = (Oid *) palloc(num_phys_attrs * sizeof(Oid));
	isvarlena = (bool *) palloc(num_phys_attrs * sizeof(bool));
	force_quote = (bool *) palloc(num_phys_attrs * sizeof(bool));
	foreach(cur, attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;

		if (binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
								 &out_func_oid, &typioparams[attnum - 1],
									&isvarlena[attnum - 1]);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid, &typioparams[attnum - 1],
							  &isvarlena[attnum - 1]);
		fmgr_info(out_func_oid, &out_functions[attnum - 1]);

		if (list_member_int(force_quote_atts, attnum))
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

	if (binary)
	{
		/* Generate header for a binary copy */
		int32		tmp;

		/* Signature */
		CopySendData((char *) BinarySignature, 11);
		/* Flags field */
		tmp = 0;
		if (oids)
			tmp |= (1 << 16);
		CopySendInt32(tmp);
		/* No header extension */
		tmp = 0;
		CopySendInt32(tmp);
	}
	else
	{
		/*
		 * For non-binary copy, we need to convert null_print to client
		 * encoding, because it will be sent directly with CopySendString.
		 */
		if (server_encoding != client_encoding)
			null_print = (char *)
				pg_server_to_client((unsigned char *) null_print,
									strlen(null_print));
	}

	scandesc = heap_beginscan(rel, ActiveSnapshot, 0, NULL);

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		bool		need_delim = false;

		CHECK_FOR_INTERRUPTS();

		MemoryContextReset(mycontext);
		oldcontext = MemoryContextSwitchTo(mycontext);

		if (binary)
		{
			/* Binary per-tuple header */
			CopySendInt16(attr_count);
			/* Send OID if wanted --- note attr_count doesn't include it */
			if (oids)
			{
				Oid			oid = HeapTupleGetOid(tuple);

				/* Hack --- assume Oid is same size as int32 */
				CopySendInt32(sizeof(int32));
				CopySendInt32(oid);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			if (oids)
			{
				string = DatumGetCString(DirectFunctionCall1(oidout,
							  ObjectIdGetDatum(HeapTupleGetOid(tuple))));
				CopySendString(string);
				need_delim = true;
			}
		}

		foreach(cur, attnumlist)
		{
			int			attnum = lfirst_int(cur);
			Datum		value;
			bool		isnull;

			value = heap_getattr(tuple, attnum, tupDesc, &isnull);

			if (!binary)
			{
				if (need_delim)
					CopySendChar(delim[0]);
				need_delim = true;
			}

			if (isnull)
			{
				if (!binary)
					CopySendString(null_print); /* null indicator */
				else
					CopySendInt32(-1);	/* null marker */
			}
			else
			{
				if (!binary)
				{
					string = DatumGetCString(FunctionCall3(&out_functions[attnum - 1],
														   value,
							   ObjectIdGetDatum(typioparams[attnum - 1]),
							Int32GetDatum(attr[attnum - 1]->atttypmod)));
					if (csv_mode)
					{
						CopyAttributeOutCSV(string, delim, quote, escape,
									  (strcmp(string, null_print) == 0 ||
									   force_quote[attnum - 1]));
					}
					else
						CopyAttributeOut(string, delim);

				}
				else
				{
					bytea	   *outputbytes;

					outputbytes = DatumGetByteaP(FunctionCall2(&out_functions[attnum - 1],
															   value,
							 ObjectIdGetDatum(typioparams[attnum - 1])));
					/* We assume the result will not have been toasted */
					CopySendInt32(VARSIZE(outputbytes) - VARHDRSZ);
					CopySendData(VARDATA(outputbytes),
								 VARSIZE(outputbytes) - VARHDRSZ);
				}
			}
		}

		CopySendEndOfRow(binary);

		MemoryContextSwitchTo(oldcontext);
	}

	heap_endscan(scandesc);

	if (binary)
	{
		/* Generate trailer for a binary copy */
		CopySendInt16(-1);
	}

	MemoryContextDelete(mycontext);

	pfree(out_functions);
	pfree(typioparams);
	pfree(isvarlena);
	pfree(force_quote);
}


/*
 * error context callback for COPY FROM
 */
static void
copy_in_error_callback(void *arg)
{
	if (copy_binary)
	{
		/* can't usefully display the data */
		if (copy_attname)
			errcontext("COPY %s, line %d, column %s",
					   copy_relname, copy_lineno, copy_attname);
		else
			errcontext("COPY %s, line %d", copy_relname, copy_lineno);
	}
	else
	{
		if (copy_attname)
		{
			/* error is relevant to a particular column */
			limit_printout_length(&attr_buf);
			
			if(Mpp_role == MPP_ROLE_EXECUTE)
				errcontext("COPY %s, line %d of this segment db input data, column %s",
						   copy_relname, copy_lineno, copy_attname);				
			else
				errcontext("COPY %s, line %d, column %s",
						   copy_relname, copy_lineno, copy_attname);				
		}
		else
		{
			/* error is relevant to a particular line */
			if (line_buf_converted ||
				client_encoding == server_encoding)
			{
				limit_printout_length(&line_buf);
				
				if(Mpp_role == MPP_ROLE_EXECUTE) 
					errcontext("COPY %s, line %d of this segment db input data: \"%s\"", copy_relname, copy_lineno, line_buf.data);
				else
					errcontext("COPY %s, line %d: \"%s\"", copy_relname, copy_lineno, line_buf.data);
			}
			else
			{
				/*
				 * Here, the line buffer is still in a foreign encoding,
				 * and indeed it's quite likely that the error is precisely
				 * a failure to do encoding conversion (ie, bad data).  We
				 * dare not try to convert it, and at present there's no way
				 * to regurgitate it without conversion.  So we have to punt
				 * and just report the line number.
				 */
				if(Mpp_role == MPP_ROLE_EXECUTE)				
				    errcontext("COPY %s, line %d of this segment db input data", copy_relname, copy_lineno);
				else
				    errcontext("COPY %s, line %d", copy_relname, copy_lineno);
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
CopyFromDispatch(Relation rel, List *attnumlist, bool binary, bool oids,
			 char *delim, char *null_print, bool csv_mode, char *quote,
			 char *escape, List *force_notnull_atts, bool escape_off)
{
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
	int         p_index;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	int		   *attr_offsets;
	bool		isnull;
	int			null_print_len; /* length of null print */
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
	bool        is_first_col;
	ListCell   *cur;
	
	CdbCopy *cdbCopy;
	
	/*
	 * This stringInfo will contain 2 types of error messages: 
	 *
	 * 1) Data errors refer to errors that are a result of inappropriate 
	 *    input data or constraint violations. All data error messages 
	 *    from the segment databases will be added to this variable and 
	 *    reported back to the client at the end of the copy command 
	 *    execution on the dispatcher.
	 * 2) Any command execution error that occurs during this COPY session. 
	 *    Such errors will usually be failure to send data over the network, 
	 *    a COPY command that was rejected by the segment databases or any I/O
	 *    error. 
	 */
	StringInfoData cdbcopy_err;
	
	/* 
	 * contains a copy of the original COPY command string that was issued by the 
	 * client. This copy of the COPY command string is sent to all the segment
	 * databases to execute there in mpp_role execute
	 */
	StringInfoData cdbcopy_cmd; 
	
    MemoryContext beforeError;  
	
	/*
	 * Variables for cdbpolicy
	 */
	MppPolicy *policy;   /* the partitioning policy for this table            */
	int p_nattrs;		 /* num of partitioning keys in the policy            */
	
	/*
	 * Variables for cdbhash
	 */
	CdbHash *cdbHash;        /* a CdbHash API object      */
	AttrNumber h_attnum;     /* hash key attribute number */ 
	Datum h_key;             /* hash key value			  */
	Oid h_oid;               /* hash key data type        */
	unsigned int target_seg; /* result segment of cdbhash */			
	
	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(attnumlist);
	num_defaults = 0;
	
	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
	 * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(rel->trigdesc);
	
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
		if (binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);
		
		if (list_member_int(force_notnull_atts, attnum))
			force_notnull[attnum - 1] = true;
		else
			force_notnull[attnum - 1] = false;
		
		/* Get default info if needed */
		if (!list_member_int(attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(rel, attnum);
			
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
	
	cdbCopy = makeCdbCopy();

	/*
	 * prepare to COPY data into segDBs:
	 * 1 - re-construct the orignial COPY command sent from the client.
	 * 2 - execute a BEGIN DTM transaction.
	 * 2 - send the COPY command to all segment databases.
	 * 3 - get the partitioning policy for this table.
	 * 4 - prepare cdbhash for hashing on row values.
	 */
	
	/* allocate memory for error and copy strings */
	initStringInfo(&cdbcopy_err); 
	initStringInfo(&cdbcopy_cmd);
		
	/* append schema and tablename */
	appendStringInfo(&cdbcopy_cmd, "COPY %s.%s", 
					 get_namespace_name(RelationGetNamespace(rel)),
					 RelationGetRelationName(rel)); 
    /* 
	 * append column list. NOTE: if not specified originally, attnumlist will 
	 * include all columns of the table by default 
	 */
	appendStringInfo(&cdbcopy_cmd, "(");	
	is_first_col = true;
	foreach(cur, attnumlist)
	{
		int		attnum = lfirst_int(cur);
		int		m = attnum - 1;
		if(is_first_col)
			appendStringInfo(&cdbcopy_cmd, "%s", NameStr(attr[m]->attname));
		else
			appendStringInfo(&cdbcopy_cmd, ",%s", NameStr(attr[m]->attname));
		
		is_first_col = false;
	}
	appendStringInfo(&cdbcopy_cmd, ")");	
	appendStringInfo(&cdbcopy_cmd, " FROM STDIN WITH");
	
	if(binary)
	{	
		appendStringInfo(&cdbcopy_cmd, " BINARY");   	
	}
	else
	{
	    appendStringInfo(&cdbcopy_cmd, " DELIMITER AS '%s'", delim);
		
		if(null_print[0] == '\\') /* escape value is de-escaped. re-escape for segdbs*/
		   appendStringInfo(&cdbcopy_cmd," NULL AS '\\%s'", null_print); 
		else
		   appendStringInfo(&cdbcopy_cmd," NULL AS '%s'", null_print);

		if(!csv_mode) 
		{
		   if(strcmp(escape,"\\") != 0 && !csv_mode) /* if default ("\") leave expression out */
					 appendStringInfo(&cdbcopy_cmd, " ESCAPE AS '%s'", escape);
		}

	}

	if(csv_mode)
	{
		appendStringInfo(&cdbcopy_cmd, " CSV");
		appendStringInfo(&cdbcopy_cmd, " QUOTE AS '%s'", quote);

		if(escape[0] == '\\') /* null value is de-escaped. re-escape for segdbs*/
		   appendStringInfo(&cdbcopy_cmd," ESCAPE AS '\\%s'", escape); 
		else
		   appendStringInfo(&cdbcopy_cmd," ESCAPE AS '%s'", escape);
		   
		/* do NOT include HEADER. Header row is swallowed by dispatcher COPY */
	}
	

   /* 
    * get the CDB policy for this table and prepare for hashing.
    */
    policy = get_table_policy(rel->rd_id);

	if(policy->ptype != POLICYTYPE_PARTITIONED)
		ereport(ERROR, 	
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Unexpected MppPolicyType: %d.", policy->ptype)));


	p_nattrs = policy->nattrs; /* number of partitioning keys */
	cdbHash = makeCdbHash(cdbCopy->total_segs, HASH_FNV_1); /* Create hash API reference */

   	/*
	 * for optimized parsing - get the last field number in the
	 * file that we need to parse to have all values for the hash keys.
	 */
     last_hash_field = 0;

     for(p_index = 0 ; p_index < p_nattrs ; p_index++)
     {
		 i = 1;
		 
	     /*
	      * for this partitioning key, search for its location in the attr list.
	      * (note that fields may be out of order).
	      */
	      foreach(cur, attnumlist)
          {
		      int			attnum = lfirst_int(cur);
		      
		      if(attnum == policy->attrs[p_index] )
		      {
				  if(i > last_hash_field)
					  last_hash_field = i;
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
	if(cdbCopyStart(cdbCopy, cdbcopy_cmd.data) == -1)
    {
		/* get error message from CopyStart */
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);					

		/* end COPY in all the segdbs in progress */
		cdbCopyEnd(cdbCopy); 

		/* get error message from CopyEnd*/
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);					
		
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("%s Closing all other existing COPY sessions.",
						cdbcopy_err.data)));		
    }
		
	
	
	/*
	 * Prepare to catch AFTER triggers.
	 */
	AfterTriggerBeginQuery();
	
	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable whether
	 * we should do this for COPY, since it's not really an "INSERT"
	 * statement as such. However, executing these triggers maintains
	 * consistency with the EACH ROW triggers that we already fire on
	 * COPY.
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);
	
	if (!binary)
		file_has_oids = oids;	/* must rely on user to tell us this... */
	else
	{
		/* Read and verify binary header */
		char		readSig[11];
		int32		tmp;
		StringInfoData header_buf;
		
		initStringInfo(&header_buf);
		
		/* Signature */
		CopyGetData(readSig, 11);

		appendBinaryStringInfo(&header_buf, readSig, 11);
		
		if (CopyGetEof() || memcmp(readSig, BinarySignature, 11) != 0)
        {
			appendStringInfo(&cdbcopy_err, "COPY file signature not recognized");
		    cdbCopy->data_errors = true;
		}
		
		if(!cdbCopy->data_errors)
		{
			/* Flags field */
			tmp = CopyGetInt32();

			appendBinaryStringInfo(&header_buf, (char *)&tmp, sizeof(int32));

			if (CopyGetEof())
				appendStringInfo(&cdbcopy_err, "invalid COPY file header (missing flags)");

			file_has_oids = (tmp & (1 << 16)) != 0;
			tmp &= ~(1 << 16);
			if ((tmp >> 16) != 0)
			{
				appendStringInfo(&cdbcopy_err, "unrecognized critical flags in COPY file header");
				cdbCopy->data_errors = true;
			}
		}
		
		if(!cdbCopy->data_errors)
		{
			/* Header extension length */
			tmp = CopyGetInt32();

			appendBinaryStringInfo(&header_buf, (char *)&tmp, sizeof(int32));

			if (CopyGetEof() || tmp < 0)
			{
				appendStringInfo(&cdbcopy_err, "invalid COPY file header (missing length)");
				cdbCopy->data_errors = true;
			}
		}
		
		if(!cdbCopy->data_errors)
		{
			/* Skip extension header, if present */
			while (tmp-- > 0)
			{
				CopyGetData(readSig, 1);

				if(Mpp_role == MPP_ROLE_DISPATCH)
					appendBinaryStringInfo(&header_buf, readSig, 1);

				if (CopyGetEof())
				{
					appendStringInfo(&cdbcopy_err, "invalid COPY file header (wrong length)");
					cdbCopy->data_errors = true;
				}
			}
		}
		
		/*
		 * Send binary header to all databases if the header is valid.
		 */
		if(!cdbCopy->data_errors)
		{
			for(i = 0 ; i < cdbCopy->total_segs ; i++)
			{
					cdbCopySendData(cdbCopy, i, header_buf.data, header_buf.len);
					
					if(cdbCopy->io_errors)
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
		line_buf.len = 0;		
		line_buf.data[0] = '\0';
		line_buf.cursor = 0;		
	}
	
	if (file_has_oids && binary)
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
	fe_eof = false;
	eol_type = EOL_UNKNOWN;
	copy_binary = binary;
	copy_relname = RelationGetRelationName(rel);
	copy_lineno = 0;
	copy_attname = NULL;
	
	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = NULL;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;
	
	/*
	 * initialize buffered scan variables.
	 */
	if(csv_mode)
	{
	    in_quote = false;
		last_was_esc = false;
	}
	
	null_print_len = strlen(null_print);
	
	/* Set up data buffer to hold a chunk of data */
	MemSet(input_buf, ' ', COPY_BUF_SIZE * sizeof(char));
	input_buf[COPY_BUF_SIZE] = '\0';
	
	no_more_data = false;		/* no more input data to read from file or FE */
	line_done = true;
	buf_done = false;
	
	do
	{
		size_t	   bytesread = 0;
		
		/* read a chunk of data into the buffer */
		if (!binary)
		{
			bytesread = CopyGetData(input_buf, COPY_BUF_SIZE);
			buf_done = false;
			
			/* set buffer pointers to beginning of the buffer */
			begloc = input_buf;
			buffer_index = 0;
	    }
		
		/*
		 * continue if some bytes were read or if we didn't reach EOF. if we
		 * both reached EOF _and_ no bytes were read, quit the loop we are
		 * done
		 */
		if (bytesread > 0 || !fe_eof || binary)
		{
			
			while (!buf_done)
			{
				Oid			loaded_oid = InvalidOid;
				
				CHECK_FOR_INTERRUPTS();
				
				/* Reset the per-tuple exprcontext */
				ResetPerTupleExprContext(estate);
				
				/* Switch into its memory context */
				MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				
				/* Initialize all values for row to NULL */
				MemSet(values, 0, num_phys_attrs * sizeof(Datum));
				MemSet(nulls, 'n', num_phys_attrs * sizeof(char));
				MemSet(attr_offsets, 0, num_phys_attrs * sizeof(int));
				
				if (!binary)
				{
					
					/* Actually read the line into memory here */
					line_done = csv_mode ?
					CopyReadLineCSV(bytesread, quote, escape) : 
					CopyReadLineText(bytesread, escape, escape_off);
					
					/*
					 * if finished processing data line - increment line count.
					 * Otherwise, if eof is not yet reached, we skip att parsing 
					 * and read more data. But if eof _was_ reached it means 
					 * that the original last data line is defective and 
					 * we want to catch that error later on.
					 */
					if (line_done)
						copy_lineno++;
					else if (!fe_eof || end_marker )
						break;
					
					if (file_has_oids)
					{
						char	   *oid_string;
						
						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(delim, null_print, null_print_len,
													 &isnull);
						
						if (isnull)
						{
							appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: null OID in COPY data."
											 "\nRow number: %d\nData row: %s", copy_lineno, line_buf.data);
							cdbCopy->data_errors = true;
						}
						else
						{
							beforeError = CurrentMemoryContext;
							
							PG_TRY(); 
							{
								copy_attname = "oid";
								loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
																				  CStringGetDatum(oid_string)));
							}
							PG_CATCH();
							{
								ErrorData  *edata;
								MemoryContextSwitchTo(beforeError);
								edata = CopyErrorData(); /* get the original error message */
								FlushErrorState();
								appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: Error processing OID column."
												 "\nRow number: %d\nDescription: %s\nData row: %s", 
												 copy_lineno, edata->message, line_buf.data);
								FreeErrorData(edata);
								cdbCopy->data_errors = true;
							}
							PG_END_TRY();
							
							MemoryContextSwitchTo(beforeError);
							
							
							if (loaded_oid == InvalidOid)
							{
								appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: invalid OID in COPY data."
												 "\nRow number: %d\nData row: %s", copy_lineno, line_buf.data);
								cdbCopy->data_errors = true;
							}

							copy_attname = NULL;
							
							if(cdbCopy->data_errors) /* if data error occured, break out of all loops */
							{
								no_more_data = true;
								break;
							}				
						}
					}
					
					beforeError = CurrentMemoryContext;
					
					/* try to read a data line, if failed catch the error, flag it and break */
					PG_TRY(); 
					{
						/* parse all the attributes in the data line */
						if(csv_mode)
							CopyReadAttributesCSV(delim, null_print, quote, escape, null_print_len, 
												  nulls, attnumlist, attr_offsets, num_phys_attrs, attr);
						else
							CopyReadAttributesText(delim, escape, null_print, null_print_len, nulls, 
												   attnumlist, attr_offsets, num_phys_attrs, attr, escape_off);
					}
					PG_CATCH();
					{
						ErrorData  *edata;
						MemoryContextSwitchTo(beforeError);
						edata = CopyErrorData(); /* get the original error message */
						FlushErrorState();
						appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: Error while parsing attributes."
										 "\nRow number: %d\nDescription: %s\nData row: %s", 
										 copy_lineno, edata->message, line_buf.data);
						FreeErrorData(edata);
						cdbCopy->data_errors = true;
					}
					PG_END_TRY();
					
					MemoryContextSwitchTo(beforeError);
					
					if(cdbCopy->data_errors) /* if data error occured, break out of all loops */
					{
						no_more_data = true;
						break;
					}				
					
					/*
					 * Since we only need the internal format of values that
					 * we want to hash on (partitioning keys only), we want to
					 * skip converting the other values so we can run faster.
					 */
					for(p_index = 0 ; p_index < p_nattrs ; p_index++)
					{
						/*
						 * for this partitioning key, search for its location in the attr list.
						 * (note that fields may be out of order, so this is necessary).
						 */
						foreach(cur, attnumlist)
					    {
							int			attnum = lfirst_int(cur);
							int			m = attnum - 1;
							char       *string;
							
							if(attnum == policy->attrs[p_index] )
							{
								string = attr_buf.data + attr_offsets[m];
								
								if (nulls[m] == ' ')
									isnull = false;
								else 
									isnull = true;
								
								if (csv_mode && isnull && force_notnull[m])
								{
									string = null_print;		/* set to NULL string */
									isnull = false;
								}
								
								/* we read an SQL NULL, no need to do anything */
								if (!isnull)
								{
									copy_attname = NameStr(attr[m]->attname);
									
									PG_TRY();
									{
										values[m] = FunctionCall3(&in_functions[m],
																  CStringGetDatum(string),
																  ObjectIdGetDatum(typioparams[m]),
																  Int32GetDatum(attr[m]->atttypmod));
									}
									PG_CATCH();
									{
										limit_printout_length(&line_buf);
										appendStringInfo(&cdbcopy_err, 
														 "\n\nMPPCOPY ERROR: Incompatiable value for partitioning "
														 "column '%s' type. \nRow number: %d\nData Value: '%s'.\nData row: %s",
														 copy_attname, copy_lineno, string, line_buf.data);
										cdbCopy->data_errors = true;
									}
									PG_END_TRY();									
									
									copy_attname = NULL;
								}/* end if (!isnull) */
								
								break; /* go to next partitioning key attribute */
							}
					    }/* end foreach */
					}/* end for partitioning indexes */
					
					if(cdbCopy->data_errors) /* if data error occured, break out of all loops */
					{
						no_more_data = true;
						break;
					}
					
				}
				else
				{
					/* binary */
					int16		fld_count;
					ListCell   *cur;
					
					fld_count = CopyGetInt16();
					if (CopyGetEof() || fld_count == -1)
					{
						buf_done = true;
						no_more_data = true;
						break;
					}
					
					if (fld_count != attr_count)
					{
						appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: row field count is %d, expected %d",
										 (int) fld_count, attr_count);
						cdbCopy->data_errors = true;
						no_more_data = true;
						break;
					}
						
					if (file_has_oids)
					{
						copy_attname = "oid";
						loaded_oid =
							DatumGetObjectId(CopyReadBinaryAttribute(0,
																	 &oid_in_function,
																	 oid_typioparam,
																	 &isnull));
						if (isnull || loaded_oid == InvalidOid)
						{
							appendStringInfo(&cdbcopy_err, "\n\nMPPCOPY ERROR: invalid OID in COPY data");
							cdbCopy->data_errors = true;
							no_more_data = true;
							break;
						}
				
						copy_attname = NULL;
					}
					
					i = 0;
					foreach(cur, attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;
						
						copy_attname = NameStr(attr[m]->attname);
						i++;
						values[m] = CopyReadBinaryAttribute(i,
															&in_functions[m],
															typioparams[m],
															&isnull);
						nulls[m] = isnull ? 'n' : ' ';
						copy_attname = NULL;
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
				
				for(i = 0 ; i < p_nattrs ; i++)
				{
					h_attnum = policy->attrs[i];          /* current att num from cdb policy */
					h_key = values[h_attnum - 1];         /* value of this attr              */
					h_oid = attr[h_attnum - 1]->atttypid; /* data type of this attr          */
					
					if(nulls[h_attnum - 1] != 'n')
						cdbhash(cdbHash, h_key, h_oid); 
					else
						cdbhashnull(cdbHash); 
				}
				
				target_seg = cdbhashreduce(cdbHash); /* hash result segment */
				
				/*
				 * Send data row to all databases for this segment.
				 */		
		        cdbCopySendData(cdbCopy, target_seg, line_buf.data, line_buf.len); 
				
                if(cdbCopy->io_errors)
				{
					appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);					
					no_more_data = true;
					break;
				}
				
				line_buf.len = 0;		/* we can reset line buffer now. */
				line_buf.data[0] = '\0';
				line_buf.cursor = 0;
			}	/* end while(!buf_done) */
		}	/* end if (bytesread > 0 || !fe_eof) */
		else /* no bytes read, end of data */
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
	
	if(cdbCopy->data_errors || cdbCopy->io_errors) 
		appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
	

	/*
	 * report all accumulated errors back to the client.
	 */
	if(cdbCopy->data_errors)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("%s", cdbcopy_err.data)));
	if(cdbCopy->io_errors)
	{
		
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("%s", cdbcopy_err.data)));
	}
	/*
	 * Done, clean up
	 */
	error_context_stack = errcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	/* 	 
     * Execute AFTER STATEMENT insertion triggers
	 */
	ExecASInsertTriggers(estate, resultRelInfo);

	/*
	 * Handle queued AFTER triggers
	 */
	AfterTriggerEndQuery();

	/* 
	 * free all resources besides ones that are needed for error reporting 
	 */
	pfree(cdbHash);
	pfree(values);
	pfree(nulls);
	pfree(attr_offsets);
	pfree(in_functions);
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
CopyFromExec(Relation rel, List *attnumlist, bool binary, bool oids,
		 char *delim, char *null_print, bool csv_mode, char *quote,
		 char *escape, List *force_notnull_atts, bool escape_off)
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
	int			null_print_len; /* length of null print */
	bool		no_more_data;
	ListCell   *cur;
	
	
	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(attnumlist);
	num_defaults = 0;
	
	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
									   * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = rel;
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(rel->trigdesc);
	
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
		if (binary)
			getTypeBinaryInputInfo(attr[attnum - 1]->atttypid,
								   &in_func_oid, &typioparams[attnum - 1]);
		else
			getTypeInputInfo(attr[attnum - 1]->atttypid,
							 &in_func_oid, &typioparams[attnum - 1]);
		fmgr_info(in_func_oid, &in_functions[attnum - 1]);
		
		if (list_member_int(force_notnull_atts, attnum))
			force_notnull[attnum - 1] = true;
		else
			force_notnull[attnum - 1] = false;
		
		/* Get default info if needed */
		if (!list_member_int(attnumlist, attnum))
		{
			/* attribute is NOT to be copied from input */
			/* use default value if one exists */
			Node	   *defexpr = build_column_default(rel, attnum);
			
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
	AfterTriggerBeginQuery();
	
	/*
	 * Check BEFORE STATEMENT insertion triggers. It's debateable whether
	 * we should do this for COPY, since it's not really an "INSERT"
	 * statement as such. However, executing these triggers maintains
	 * consistency with the EACH ROW triggers that we already fire on
	 * COPY.
	 */
	ExecBSInsertTriggers(estate, resultRelInfo);
	
	if (!binary)
		file_has_oids = oids;	/* must rely on user to tell us this... */
	else
	{
		/* Read and verify binary header */
		char		readSig[11];
		int32		tmp;
		
		/* Signature */
		CopyGetData(readSig, 11);
		if (CopyGetEof() || memcmp(readSig, BinarySignature, 11) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("COPY file signature not recognized")));
		/* Flags field */
		tmp = CopyGetInt32();
		if (CopyGetEof())
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
		tmp = CopyGetInt32();
		if (CopyGetEof() || tmp < 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (missing length)")));
		/* Skip extension header, if present */
		while (tmp-- > 0)
		{
			CopyGetData(readSig, 1);
			if (CopyGetEof())
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("invalid COPY file header (wrong length)")));
		}
	}
	
	if (file_has_oids && binary)
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
	fe_eof = false;
	eol_type = EOL_UNKNOWN;
	copy_binary = binary;
	copy_relname = RelationGetRelationName(rel);
	copy_lineno = 0;
	copy_attname = NULL;
	
	/* Set up callback to identify error line number */
	errcontext.callback = copy_in_error_callback;
	errcontext.arg = NULL;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;
	
	/*
	 * initialize buffered scan variables.
	 */
	if(csv_mode)
	{
	    in_quote = false;
		last_was_esc = false;
	}
	
	null_print_len = strlen(null_print);
	
	/* Set up data buffer to hold a chunk of data */
	MemSet(input_buf, ' ', COPY_BUF_SIZE * sizeof(char));
	input_buf[COPY_BUF_SIZE] = '\0';
	
	no_more_data = false;		/* no more input data to read from file or FE */
	line_done = true;
	buf_done = false;
	
	do
	{
		size_t	   bytesread = 0;
		
		/* read a chunk of data into the buffer */
		if (!binary)
		{
			bytesread = CopyGetData(input_buf, COPY_BUF_SIZE);
			buf_done = false;
			
			/* set buffer pointers to beginning of the buffer */
			begloc = input_buf;
			buffer_index = 0;
	    }
		
		/*
		 * continue if some bytes were read or if we didn't reach EOF. if we
		 * both reached EOF _and_ no bytes were read, quit the loop we are
		 * done
		 */
		if (bytesread > 0 || !fe_eof || binary)
		{
			
			while (!buf_done)
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
				
				if (!binary)
				{
					
					/* Actually read the line into memory here */
					line_done = csv_mode ?
					CopyReadLineCSV(bytesread, quote, escape) : 
					CopyReadLineText(bytesread, escape, escape_off);
					
					/*
					 * if finished processing data line - increment line count.
					 * Otherwise, if eof is not yet reached, we skip att parsing 
					 * and read more data. But if eof _was_ reached it means 
					 * that the original last data line is defective and 
					 * we want to catch that error later on.
					 */
					if (line_done)
						copy_lineno++;
					else if (!fe_eof || end_marker )
						break;
					
					if (file_has_oids)
					{
						char	   *oid_string;
						
						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(delim, null_print, null_print_len,
													 &isnull);
						
						if (isnull)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("null OID in COPY data")));
						else
						{
							copy_attname = "oid";
							loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
																			  CStringGetDatum(oid_string)));
							if (loaded_oid == InvalidOid)
								ereport(ERROR,
										(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
										 errmsg("invalid OID in COPY data")));
							copy_attname = NULL;
						}
					}
					
					/* parse all the attribute in the data line */
					if(csv_mode)
						CopyReadAttributesCSV(delim, null_print, quote, escape, null_print_len, 
											  nulls, attnumlist, attr_offsets, num_phys_attrs, attr);
					else
						CopyReadAttributesText(delim, escape, null_print, null_print_len, nulls, 
											   attnumlist, attr_offsets, num_phys_attrs, attr, escape_off);
					
					/*
					 * Loop to read the user attributes on the line.
					 */
					foreach(cur, attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;
						char       *string;
						
						string = attr_buf.data + attr_offsets[m];
						
						if (nulls[m] == ' ')
							isnull = false;
						else
							isnull = true;
						
						if (csv_mode && isnull && force_notnull[m])
						{
							string = null_print;		/* set to NULL string */
							isnull = false;
						}
						
						/* we read an SQL NULL, no need to do anything */
						if (!isnull)
						{
							copy_attname = NameStr(attr[m]->attname);
							values[m] = FunctionCall3(&in_functions[m],
													  CStringGetDatum(string),
													  ObjectIdGetDatum(typioparams[m]),
													  Int32GetDatum(attr[m]->atttypmod));
							nulls[m] = ' ';
							copy_attname = NULL;
						}
					}
				}
				else
				{
					/* binary */
					int16		fld_count;
					ListCell   *cur;
					
					fld_count = CopyGetInt16();
					if (CopyGetEof() || fld_count == -1)
					{
						buf_done = true;
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
						copy_attname = "oid";
						loaded_oid =
							DatumGetObjectId(CopyReadBinaryAttribute(0,
																	 &oid_in_function,
																	 oid_typioparam,
																	 &isnull));
						if (isnull || loaded_oid == InvalidOid)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("invalid OID in COPY data")));
						copy_attname = NULL;
					}
					
					i = 0;
					foreach(cur, attnumlist)
					{
						int			attnum = lfirst_int(cur);
						int			m = attnum - 1;
						
						copy_attname = NameStr(attr[m]->attname);
						i++;
						values[m] = CopyReadBinaryAttribute(i,
															&in_functions[m],
															typioparams[m],
															&isnull);
						nulls[m] = isnull ? 'n' : ' ';
						copy_attname = NULL;
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
				
				if (oids && file_has_oids)
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
					if (rel->rd_att->constr)
						ExecConstraints(resultRelInfo, slot, estate);
					
					/*
					 * OK, store the tuple and create index entries for it
					 */
					simple_heap_insert(rel, tuple);
					
					if (resultRelInfo->ri_NumIndices > 0)
						ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);
					
					/* AFTER ROW INSERT Triggers */
					ExecARInsertTriggers(estate, resultRelInfo, tuple);
				}
				
				line_buf.len = 0;		/* we can reset line buffer now. */
				line_buf.data[0] = '\0';
				line_buf.cursor = 0;
			}					/* end while(!buf_done) */
		}						/* end if (bytesread > 0 || !fe_eof) */
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
	ExecASInsertTriggers(estate, resultRelInfo);

	/*
	 * Handle queued AFTER triggers
	 */
	AfterTriggerEndQuery();

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
CopyReadLineText(size_t bytesread, char *escape, bool escape_off)
{
	int			linesize;
	bool		transcode = (client_encoding != server_encoding);
	char	   *cvt;
	char        escapec = '\0';
	
	/* mark that encoding conversion hasn't occurred yet */
	line_buf_converted = false;
	
	/*
	 * set the escape char for text format ('\\' by default).
	 */
	escapec = escape[0];
	
	/*
	 * Detect end of line type if not already detected.
	 */
	if (eol_type == EOL_UNKNOWN)
	{
		bool		eol_detected = DetectLineEnd(bytesread, NULL, escape);
		
		if (!eol_detected)
		{
			/* load entire input buffer into line buf, and quit */
			appendBinaryStringInfo(&line_buf, input_buf, COPY_BUF_SIZE);
			buf_done = true;
			return false;
		}
	}
	
	/*
	 * Special case: eol is CRNL, last byte of previous buffer was an
	 * unescaped CR and 1st byte of current buffer is NL. We check for
	 * that here.
	 */
	if (eol_type == EOL_CRNL)
	{
		/* if we started scanning from the 1st byte of the buffer */
		if (begloc == input_buf)
		{
			/* and had a CR in last byte of prev buf */
			if (cr_in_prevbuf)
			{
				/*
				 * if this 1st byte in buffer is 2nd byte of line end sequence
				 * (linefeed)
				 */
				if (*begloc == eol_ch[1])
				{
					/*  
					* load that one linefeed byte and indicate we are done
					* with the data line
					*/ 
					appendBinaryStringInfo(&line_buf, begloc, 1);
					buffer_index++;
					begloc++;
					esc_in_prevbuf = false;
					cr_in_prevbuf = false;
					return true;
				}
			}
			
			cr_in_prevbuf = false;
		}
	}
	
	/*
	 * (we need a loop so that if eol_ch is found, but prev ch is backslash,
		* we can search for the next eol_ch)
	 */ 
	while (true)
	{
		/* reached end of buffer */
		if ((endloc = scanTextLine(begloc, eol_ch[0], bytesread - buffer_index)) == NULL)
		{
			linesize = COPY_BUF_SIZE - (begloc - input_buf);
			appendBinaryStringInfo(&line_buf, begloc, linesize);
			
			if (line_buf.len > 1)
			{
				char	   *last_ch = line_buf.data + line_buf.len - 1; /* before terminating \0 */
				
				if (*last_ch == escapec && !escape_off)
				{
					esc_in_prevbuf = true;
					
					if (line_buf.len > 2)
					{
						last_ch--;
						if (*last_ch == escapec)
							esc_in_prevbuf = false;
					}
				}
				else if (*last_ch == '\r')
				{
					if (eol_type == EOL_CRNL)
						cr_in_prevbuf = true;
				}
			}
			
			line_done = false;
			buf_done = true;
			break;
		}
		else
			/* found the 1st eol ch in input_buf. */
		{
			bool		eol_found = true;
			bool		eol_escaped = (escape_off ? false : true);
			
			/*
			 * Load that piece of data (potentially a data line) into the line buffer,
			 * and update the pointers for the next scan.
			 */
			linesize = endloc - begloc + 1;
			appendBinaryStringInfo(&line_buf, begloc, linesize);
			buffer_index += linesize;
			begloc = endloc + 1;
			
			if (eol_type == EOL_CRNL)
			{
				/* check if there is a '\n' after the '\r' */
				if (*(endloc + 1) == '\n')
				{
					/* this is a line end */
					appendBinaryStringInfo(&line_buf, begloc, 1);		/* load that '\n' */
					buffer_index++;
					begloc++;
				}
				else
					/* just a CR, not a line end */
					eol_found = false;
			}
			
			/*
			 * in some cases, this end of line char happens to be the
			 * last character in the buffer. we need to catch that.
			 */
			if (buffer_index >= bytesread)
				buf_done = true;
			
			/*
			 * Check if the 1st end of line ch is escaped.
			 */
			if (endloc != input_buf)	/* can we look 1 char back? */
			{
				if (*(endloc - 1) != escapec)	/* prev char is not an escape */
					eol_escaped = false;
				else
					/* prev char is an escape */
				{
					if (endloc != (input_buf + 1))		/* can we look another
						* char back? */
					{
						/* it's a double escape char, so it's not an escape */
						if (*(endloc - 2) == escapec)
							eol_escaped = false;
						/* else it's a single escape char, so EOL is ascaped */
					}
					else
					{
						/* we need to check in the last buffer */
						if (esc_in_prevbuf)		
							eol_escaped = false;
					}
				}
			}
			else
				/*
				 * this eol ch is first ch in buffer, check for escape in prev buf
				 */
			{
				if (!esc_in_prevbuf)
					eol_escaped = false;
			}
			
			esc_in_prevbuf = false;		/* reset variable */
			
			/*
			 * if eol was found, and it isn't escaped, line is done
			 */
			if ((eol_escaped == false) && eol_found)
			{
				line_done = true;
				break;
			}
			else
			{
				/* stay in the loop and process some more data. */
				line_done = false;
				
				if(eol_found)
					copy_lineno++; /* increase line index for error reporting */
			}
						
		}						/* end of found eol_ch */
	}

	/*
	 * Done reading the line. Convert it to server encoding.
	 */
	if (transcode)
	{
		cvt = (char *) pg_client_to_server((unsigned char *) line_buf.data,
										   line_buf.len);
		if (cvt != line_buf.data)
		{
			/* transfer converted data back to line_buf */
			line_buf.len = 0;
			line_buf.data[0] = '\0';
			line_buf.cursor = 0;
			appendBinaryStringInfo(&line_buf, cvt, strlen(cvt));
		}
	}
			
	/* indicate that conversion had occured */
	line_buf_converted = true;

	/*
	 * check if this line is an end marker -- "\."
	 */
	end_marker = false;

	switch (eol_type)
	{
		case EOL_NL:
			if (!strcmp(line_buf.data, "\\.\n"))
				end_marker = true;
			break;
		case EOL_CR:
			if (!strcmp(line_buf.data, "\\.\r"))
				end_marker = true;
			break;
		case EOL_CRNL:
			if (!strcmp(line_buf.data, "\\.\r\n"))
				end_marker = true;
			break;
		case EOL_UNKNOWN:
			break;
	}

	if (end_marker)
	{
		/*
		 * Reached end marker. In protocol version 3 we
		 * should ignore anything after \. up to protocol
		 * end of copy data.
		 */
		if(copy_dest == COPY_NEW_FE)
		{
			while(!fe_eof) 
			{
				CopyGetData(input_buf, COPY_BUF_SIZE); /* eat data */
			}
		}
		
		fe_eof = true;
		/* we don't want to process a \. as data line, want to quit. */
		line_done = false;
		buf_done = true;
	}

	return line_done;
}

/*
 * Finds the next CSV line that is in the input buffer and loads 
 * it into line_buf. Returns an indication if the line that was read 
 * is complete (if an unescaped line-end was encountered). If we 
 * reached the end of buffer before the whole line was written into the
 * line buffer then returns false.
 */
static bool
CopyReadLineCSV(size_t bytesread, char *quote, char *escape)
{
	int			linesize;
	char	   *cvt;
	char        quotec = '\0',
		        escapec = '\0';
	bool		transcode = (client_encoding != server_encoding);
	
	/* mark that encoding conversion hasn't occurred yet */
	line_buf_converted = false;
	
	escapec = escape[0];
	quotec = quote[0];
	
    /* ignore special escape processing if it's the same as quotec */
	if (quotec == escapec)
		escapec = '\0';
	
	/*
	 * Detect end of line type if not already detected.
	 */
	if (eol_type == EOL_UNKNOWN)
	{
		bool		eol_detected = DetectLineEnd(bytesread, quote, escape);
		
		if (!eol_detected)
		{
			/* load entire input buffer into line buf, and quit */
			appendBinaryStringInfo(&line_buf, input_buf, COPY_BUF_SIZE);
			line_done = false;
			buf_done = true;
			
			return line_done;
		}
	}
	
	/*
	 * Special case: eol is CRNL, last byte of previous buffer was an
	 * unescaped CR and 1st byte of current buffer is NL. We check for
	 * that here.
	 */
	if (eol_type == EOL_CRNL)
	{
		/* if we started scanning from the 1st byte of the buffer */
		if (begloc == input_buf)
		{
			/* and had a CR in last byte of prev buf */
			if (cr_in_prevbuf)
			{
				/*
				 * if this 1st byte in buffer is 2nd byte of line end sequence
				 * (linefeed)
				 */
				if (*begloc == eol_ch[1])
				{
					/*
					 * load that one linefeed byte and indicate we are done
					 * with the data line
					 */
					appendBinaryStringInfo(&line_buf, begloc, 1);
					buffer_index++;
					begloc++;
					
					line_done = true;
					esc_in_prevbuf = false;
					cr_in_prevbuf = false;
					
					return line_done;
				}
			}
			
			cr_in_prevbuf = false;
		}
	}

	/*
	 * (we need a loop so that if eol_ch is found, but we are in quotes,
		* we can search for the next eol_ch)
	 */
	while (true)
	{
		/* reached end of buffer */
		if ((endloc = scanCSVLine(begloc, eol_ch[0], escapec, quotec, bytesread - buffer_index)) == NULL)
		{
			linesize = COPY_BUF_SIZE - (begloc - input_buf);
			appendBinaryStringInfo(&line_buf, begloc, linesize);
			
			if (last_was_esc)
				esc_in_prevbuf = true;
			
			if (line_buf.len > 1)
			{
				char	   *last_ch = line_buf.data + line_buf.len - 1; /* before terminating \0 */
				
				if (*last_ch == '\r')
				{
					if (eol_type == EOL_CRNL)
						cr_in_prevbuf = true;
				}
			}
			
			line_done = false;
			buf_done = true;
			break;
		}
		else
			/* found 1st eol char in input_buf. */
		{
			bool		eol_found = true;
			
			/*
			 * Load that piece of data (potentially a data line) into the line buffer,
			 * and update the pointers for the next scan.
			 */
			linesize = endloc - begloc + 1;
			appendBinaryStringInfo(&line_buf, begloc, linesize);
			buffer_index += linesize;
			begloc = endloc + 1;
			
			/* end of line only if not in quotes */
			if(in_quote)
			{
				line_done = false;
				
				/* update file line for error message */
				/* TODO: for dos line end we need to do check before incrementing! */
				copy_lineno++;
				
				/* buf done, but still in quote */
				if (buffer_index >= bytesread)
				{
					buf_done = true;
					break;
				}
			}
			else
			{
				/* if dos eol, check for '\n' after the '\r' */
				if (eol_type == EOL_CRNL)
				{
					if (*(endloc + 1) == '\n')
					{
						/* this is a line end */
						appendBinaryStringInfo(&line_buf, begloc, 1);		/* load that '\n' */
						buffer_index++;
						begloc++;
					}
					else
						/* just a CR, not a line end */
						eol_found = false;
				}
				
				/*
				 * in some cases, this end of line char happens to be the
				 * last character in the buffer. we need to catch that.
				 */
				if (buffer_index >= bytesread)
					buf_done = true;
				
				/*
				 * if eol was found line is done
				 */
				if (eol_found)
				{
					line_done = true;
					break;
				}
			}
		}						/* end of found eol_ch */
	}

	/*
	 * Done reading the line. Convert it to server encoding.
	 */
	if (transcode)
	{
		cvt = (char *) pg_client_to_server((unsigned char *) line_buf.data,
										   line_buf.len);
		if (cvt != line_buf.data)
		{
			/* transfer converted data back to line_buf */
			line_buf.len = 0;
			line_buf.data[0] = '\0';
			line_buf.cursor = 0;
			appendBinaryStringInfo(&line_buf, cvt, strlen(cvt));
		}
	}

	/* indicate that conversion had occured */
	line_buf_converted = true;

	/*
	 * check if this line is an end marker -- "\."
	 */
	end_marker = false;

	switch (eol_type)
	{
		case EOL_NL:
			if (!strcmp(line_buf.data, "\\.\n"))
				end_marker = true;
			break;
		case EOL_CR:
			if (!strcmp(line_buf.data, "\\.\r"))
				end_marker = true;
			break;
		case EOL_CRNL:
			if (!strcmp(line_buf.data, "\\.\r\n"))
				end_marker = true;
			break;
		case EOL_UNKNOWN:
			break;
	}

	if (end_marker)
	{
		/*
		 * Reached end marker. In protocol version 3 we
		 * should ignore anything after \. up to protocol
		 * end of copy data.
		 */
		if(copy_dest == COPY_NEW_FE)
		{
			while(!fe_eof) 
			{
				CopyGetData(input_buf, COPY_BUF_SIZE); /* eat data */
			}
		}
		
		fe_eof = true;
		/* we don't want to process a \. as data line, want to quit. */
		line_done = false;
		buf_done = true;
	}

	return line_done;
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
DetectLineEnd(size_t bytesread, char *quote, char *escape)
{
	int         index = 0;
	char		c;
	char		quotec = '\0',
		        escapec = '\0';
	bool        csv = false;
	
	if (quote) /* CSV format */
	{
		csv = true;
		quotec = quote[0];
		escapec = escape[0];
		/* ignore special escape processing if it's the same as quotec */
		if (quotec == escapec)
			escapec = '\0';
	}
	
	while (index < COPY_BUF_SIZE)
	{
		c = input_buf[index];
		
		if(csv)
		{
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;
		}
		
		if (c == '\n')
		{
			if(!csv || (csv && !in_quote))
			{
				eol_type = EOL_NL;
				eol_ch[0] = '\n';
				eol_ch[1] = '\0';
				
				in_quote = false;
				last_was_esc = false;
				return true;
			}
		}
		if (c == '\r')
		{
			if(!csv || (csv && !in_quote))
            {
				if (input_buf[index + 1] == '\n') /* always safe */
					{
					eol_type = EOL_CRNL;
					eol_ch[0] = '\r';
					eol_ch[1] = '\n';
					}
					else
					{
						eol_type = EOL_CR;
						eol_ch[0] = '\r';
						eol_ch[1] = '\0';
					}
					
					in_quote = false;
					last_was_esc = false;
					return true;
			}
		}

	index++;
	}

return false;
}

/*
 * Read all TEXT attributes. Attributes are parsed from line_buf and
 * inserted (all at once) to attr_buf, while saving pointers to
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
 * at byte offset 0, "b" in 20 and "e" in 55, in attr_buf.
 *
 * In the attribute buffer (attr_buf) each attribute
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
 * attr_buf.cursor -- an offset from the beginning of the
 * attribute buffer that indicates where the current attribute begins.
 */
static void 
CopyReadAttributesText(const char *delim, const char *escape, const char *null_print,
					   int null_print_len, char *nulls, List *attnumlist, 
					   int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr,
					   bool escape_off)
{
	char		delimc = delim[0];		/* delimiter character */
	char        escapec = escape[0];    /* escape character    */
	char	   *scan_start;		/* pointer to line buffer for scan start. */
	char	   *scan_end;		/* pointer to line buffer where char was found */
    char	   *stop;
	char	   *scanner;			
	int			attr_pre_len;	/* attr raw len, before processing escapes */
	int			attr_post_len;	/* current attr len after escaping */
	int			m;				/* attribute index being parsed */
	int			bytes_remaining;/* num bytes remaining to be scanned in line buf */
	int			chunk_start;	/* offset to beginning of line chunk to load */
	int			chunk_len;		/* length of chunk of data to load to attr buf */
	int			oct_val;		/* byte value for octal escapes */
	int			attnum;			/* attribute number being parsed */
	ListCell   *cur;			/* cursor to attribute list used for this COPY */
	int			attribute;
		
		/*
		 * init variables for attribute scan
		 */
		attr_buf.len = 0;
		attr_buf.data[0] = '\0';
		attr_buf.cursor = 0;
		/* cursor is now > 0 if we copy WITH OIDS */
		scan_start = line_buf.data + line_buf.cursor;
		cur = list_head(attnumlist);
		attnum = lfirst_int(cur);
		m = attnum - 1;
		chunk_start = line_buf.cursor;
		chunk_len = 0;
		attr_pre_len = 0;
		attr_post_len = 0;
		attribute = 1;
		
		if(escape_off)
			escapec = delimc; /* look only for delimiters, escapes are disabled */
		
		/*
		 * Scan through the line buffer to read all attributes data
		 */
		while (line_buf.cursor < line_buf.len)
		{
			bytes_remaining = line_buf.len - line_buf.cursor;
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
				if (eol_type == EOL_CRNL)
				{
					attr_pre_len += bytes_remaining - 2;
					chunk_len = line_buf.len - chunk_start - 2;
				}
				else
				{
					attr_pre_len += bytes_remaining - 1;
					chunk_len = line_buf.len - chunk_start - 1;
				}
				
				/* check if this is a NULL value or data value (assumed NULL) */
				if (attr_pre_len == null_print_len
					&&
					strncmp(line_buf.data + line_buf.len - attr_pre_len - 1, null_print, attr_pre_len)
					== 0)
					nulls[m] = 'n';
				else
					nulls[m] = ' ';
				
				attr_offsets[m] = attr_buf.cursor;
				
				
				/* load the last chunk, the whole buffer in most cases */
				appendBinaryStringInfo(&attr_buf, line_buf.data + chunk_start, chunk_len);
				
				line_buf.cursor += attr_pre_len + 2;		/* skip eol char and
					* '\0' to exit loop */
				
				if (lnext(cur) != NULL)
				{
					/*
					 * For an empty data line, the previous COPY code will
					 * fail it during the conversion stage. We can fail it here
					 * already, but then we will fail the regression tests b/c
					 * of a different error message. that's why we return so we
					 * can get the same error message that regress expects. ahh...
					 * this conditional is unnecessary and should be removed soon.
					 */
					if (line_buf.len > 1)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("missing data for column \"%s\"",
										NameStr(attr[lfirst_int(lnext(cur)) - 1]->attname))));
					else
						return;
				}
			}
			else
				/* FOUND A DELIMITER OR ESCAPE */
			{
				if (cur == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("extra data after last expected column")));
				
				if (*scan_end == delimc)		/* found a delimiter */
				{
					attnum = lfirst_int(cur);
					m = attnum - 1;
					
					/* (we don't include the delimiter ch in length) */
					attr_pre_len += scan_end - scan_start;
					/* (we don't include the delimiter ch in length) */
					attr_post_len += scan_end - scan_start;
					
					/* check if this is a null print or data (assumed NULL) */
					if (attr_pre_len == null_print_len &&
						strncmp(scan_end - attr_pre_len, null_print, attr_pre_len) == 0)
						nulls[m] = 'n';
					else
						nulls[m] = ' ';
					
					/* set the pointer to next attribute position */
					attr_offsets[m] = attr_buf.cursor;
					
					/*
					 * update buffer cursors to our current location, +1 to skip
					 * the delimc
					 */
					line_buf.cursor = scan_end - line_buf.data + 1;
					attr_buf.cursor += attr_post_len + 1;
					
					/* prepare scan for next attr */
					scan_start = line_buf.data + line_buf.cursor;
					cur = lnext(cur);
					attr_pre_len = 0;
					attr_post_len = 0;

					/*
					 * for the dispatcher - stop parsing once we have
					 * all the hash field values. We don't need the rest.
					 */
					if(Mpp_role == MPP_ROLE_DISPATCH)
					{
						if(attribute == last_hash_field)
						{
							/* load the chunk from chunk_start to end of current attribute, not including delimiter */
							chunk_len = line_buf.cursor - chunk_start - 1;
							appendBinaryStringInfo(&attr_buf, line_buf.data + chunk_start, chunk_len);
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
					
					chunk_len = (scan_end - line_buf.data) - chunk_start + 1;
					
					/* load a chunk of data */
					appendBinaryStringInfo(&attr_buf, line_buf.data + chunk_start, chunk_len);
					
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
					 * into the attribute buffer. Then overwritting the backslash
					 * with the escaped sequence or char, and continuing to scan
					 * from *after* the char than is after the escape in line buf.
					 */
					*(attr_buf.data + attr_buf.len - 1) = newc;
					line_buf.cursor = scan_end - line_buf.data + skip;
					scan_start = scan_end + skip;
					chunk_start = line_buf.cursor;
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
			*(attr_buf.data + attr_offsets[attribute] - 1) = '\0';
	}

}

/*
 * Read a binary attribute
 */
static Datum
CopyReadBinaryAttribute(int column_no, FmgrInfo *flinfo, Oid typioparam,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;
	
	fld_size = CopyGetInt32();
	
	if (CopyGetEof())
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	if (fld_size == -1)
	{
		*isnull = true;
		return (Datum) 0;
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	
	/* reset attr_buf to empty, and load raw data in it */
	attr_buf.len = 0;
	attr_buf.data[0] = '\0';
	attr_buf.cursor = 0;
	
	enlargeStringInfo(&attr_buf, fld_size);
	
	CopyGetData(attr_buf.data, fld_size);
	if (CopyGetEof())
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	
	/* load raw data into line buf for dispatching later */
	if(Mpp_role == MPP_ROLE_DISPATCH)
		appendBinaryStringInfo(&line_buf, attr_buf.data, fld_size);
	
	attr_buf.len = fld_size;
	attr_buf.data[fld_size] = '\0';
	
	/* Call the column type's binary input converter */
	result = FunctionCall2(flinfo,
						   PointerGetDatum(&attr_buf),
						   ObjectIdGetDatum(typioparam));
	
	/* Trouble if it didn't eat the whole buffer */
	if (attr_buf.cursor != attr_buf.len)
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
CopyReadAttributesCSV(const char *delim, const char *null_print, char *quote,
					  char *escape, int null_print_len, char *nulls, List *attnumlist, 
					  int *attr_offsets, int num_phys_attrs, Form_pg_attribute *attr)
{
	char		delimc = delim[0];
	char		quotec = quote[0];
	char		escapec = escape[0];
	char		c;
	int			start_cursor = line_buf.cursor;
	int			end_cursor = start_cursor;
	int			input_len;
	bool		in_quote = false;
	bool		saw_quote = false;
	int			attnum;			/* attribute number being parsed */
	int			m;				/* attribute index being parsed */
	ListCell   *cur;			/* cursor to attribute list used for this COPY */
	int         attribute;
	
	/*
	 * init variables for attribute scan
	 */
	attr_buf.len = 0;
	attr_buf.data[0] = '\0';
	attr_buf.cursor = 0;
	
	cur = list_head(attnumlist);
	attnum = lfirst_int(cur);
	m = attnum - 1;
	input_len = 0;
	attribute = 1;
	
	
	for (;;)
	{
		end_cursor = line_buf.cursor;
		
		/* finished processing attributes in line */
		if (line_buf.cursor >= line_buf.len - 1)
		{
			/* check whether raw input matched null marker */
			input_len = end_cursor - start_cursor;
			if (!saw_quote && input_len == null_print_len &&
				strncmp(&line_buf.data[start_cursor], null_print, input_len) == 0)
				nulls[m] = 'n';
			else
				nulls[m] = ' ';
			
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
		
		c = line_buf.data[line_buf.cursor++];
		
		/* unquoted field delimiter  */
		if (!in_quote && c == delimc)
		{
			/* check whether raw input matched null marker */
			input_len = end_cursor - start_cursor;
			if (!saw_quote && input_len == null_print_len &&
				strncmp(&line_buf.data[start_cursor], null_print, input_len) == 0)
				nulls[m] = 'n';
			else
				nulls[m] = ' ';
            
			/* terminate attr string with '\0' */
			appendStringInfoCharMacro(&attr_buf, '\0');
			attr_buf.cursor++;
			
			/* setup next attribute scan */
			cur = lnext(cur);
			
			if (cur == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("extra data after last expected column")));
			
			saw_quote = false;
			attnum = lfirst_int(cur);
			m = attnum - 1;
			attr_offsets[m] = attr_buf.cursor;
			start_cursor = line_buf.cursor;
			
			/*
			 * for the dispatcher - stop parsing once we have
			 * all the hash field values. We don't need the rest.
			 */
			if(Mpp_role == MPP_ROLE_DISPATCH)
			{
				if(attribute == last_hash_field)
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
			if (line_buf.cursor <= line_buf.len)
			{
				char		nextc = line_buf.data[line_buf.cursor];
				
				if (nextc == escapec || nextc == quotec)
				{
					appendStringInfoCharMacro(&attr_buf, nextc);
					line_buf.cursor++;
					attr_buf.cursor++;
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
		appendStringInfoCharMacro(&attr_buf, c);
		attr_buf.cursor++;
	}
	
}

/*
 * Read the first attribute. This is mainly used to maintain support
 * for an OID column. All the rest of the columns will be read at once with
 * CopyReadAttributesText.
 */
static char *
CopyReadOidAttr(const char *delim, const char *null_print, int null_print_len,
				bool *isnull)
{
	char		delimc = delim[0];
	char	   *start_loc = line_buf.data + line_buf.cursor;
	char	   *end_loc;
	int			attr_len = 0;
	int			bytes_remaining;
	
	/* reset attribute buf to empty */
	attr_buf.len = 0;
	attr_buf.data[0] = '\0';
	attr_buf.cursor = 0;
	
	/* # of bytes that were not yet processed in this line */
	bytes_remaining = line_buf.len - line_buf.cursor;
	
	/* got to end of line */
	if ((end_loc = scanTextLine(start_loc, delimc, bytes_remaining)) == NULL)
	{
		attr_len = bytes_remaining - 1; /* don't count '\n' in len calculation */
		appendBinaryStringInfo(&attr_buf, start_loc, attr_len);
		line_buf.cursor += attr_len + 2;		/* skip '\n' and '\0' */
	}
	else
		/* found a delimiter */
	{
		/*
		 * (we don't care if delim was preceded with a backslash, because it's
		 * an invalid OID anyway)
		 */
		
		attr_len = end_loc - start_loc; /* we don't include the delimiter ch */
		
		appendBinaryStringInfo(&attr_buf, start_loc, attr_len);
		line_buf.cursor += attr_len + 1;
	}
	
	
	/* check whether raw input matched null marker */
	if (attr_len == null_print_len && strncmp(start_loc, null_print, attr_len) == 0)
		*isnull = true;
	else
		*isnull = false;
	
	return attr_buf.data;
}


/*
 * Send text representation of one attribute, with conversion and escaping
 */
static void
CopyAttributeOut(char *server_string, char *delim)
{
	char	   *string;
	char		c;
	char		delimc = delim[0];
	bool		same_encoding;
	int			mblen;
	int			i;

	same_encoding = (server_encoding == client_encoding);
	if (!same_encoding)
		string = (char *) pg_server_to_client((unsigned char *) server_string,
											  strlen(server_string));
	else
		string = server_string;

	for (; (c = *string) != '\0'; string += mblen)
	{
		mblen = 1;

		switch (c)
		{
			case '\b':
				CopySendString("\\b");
				break;
			case '\f':
				CopySendString("\\f");
				break;
			case '\n':
				CopySendString("\\n");
				break;
			case '\r':
				CopySendString("\\r");
				break;
			case '\t':
				CopySendString("\\t");
				break;
			case '\v':
				CopySendString("\\v");
				break;
			case '\\':
				CopySendString("\\\\");
				break;
			default:
				if (c == delimc)
					CopySendChar('\\');
				CopySendChar(c);

				/*
				 * We can skip pg_encoding_mblen() overhead when encoding
				 * is same, because in valid backend encodings, extra
				 * bytes of a multibyte character never look like ASCII.
				 */
				if (!same_encoding)
				{
					/* send additional bytes of the char, if any */
					mblen = pg_encoding_mblen(client_encoding, string);
					for (i = 1; i < mblen; i++)
						CopySendChar(string[i]);
				}
				break;
		}
	}
}

/*
 * Send CSV representation of one attribute, with conversion and
 * CSV type escaping
 */
static void
CopyAttributeOutCSV(char *server_string, char *delim, char *quote,
					char *escape, bool use_quote)
{
	char	   *string;
	char		c;
	char		delimc = delim[0];
	char		quotec = quote[0];
	char		escapec = escape[0];
	char	   *test_string;
	bool		same_encoding;
	int			mblen;
	int			i;

	same_encoding = (server_encoding == client_encoding);
	if (!same_encoding)
		string = (char *) pg_server_to_client((unsigned char *) server_string,
											  strlen(server_string));
	else
		string = server_string;

	/*
	 * have to run through the string twice, first time to see if it needs
	 * quoting, second to actually send it
	 */

	for (test_string = string;
		 !use_quote && (c = *test_string) != '\0';
		 test_string += mblen)
	{
		/*
		 * We don't know here what the surrounding line end characters
		 * might be. It might not even be under postgres' control. So
		 * we simple warn on ANY embedded line ending character.
		 *
		 * This warning will disappear when we make line parsing field-aware,
		 * so that we can reliably read in embedded line ending characters
		 * regardless of the file's line-end context.
		 *
		 */

		if (!embedded_line_warning  && (c == '\n' || c == '\r') )
		{
			embedded_line_warning = true;
			elog(WARNING,
				 "CSV fields with embedded linefeed or carriage return "
				 "characters might not be able to be reimported");
		}

		if (c == delimc || c == quotec || c == '\n' || c == '\r')
			use_quote = true;
		if (!same_encoding)
			mblen = pg_encoding_mblen(client_encoding, test_string);
		else
			mblen = 1;
	}

	if (use_quote)
		CopySendChar(quotec);

	for (; (c = *string) != '\0'; string += mblen)
	{
		if (use_quote && (c == quotec || c == escapec))
			CopySendChar(escapec);

		CopySendChar(c);

		if (!same_encoding)
		{
			/* send additional bytes of the char, if any */
			mblen = pg_encoding_mblen(client_encoding, string);
			for (i = 1; i < mblen; i++)
				CopySendChar(string[i]);
		}
		else
			mblen = 1;
	}

	if (use_quote)
		CopySendChar(quotec);
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
scanTextLine(const char *s, char eol, size_t len)
{
	const char *start;
	
	if (client_encoding_only && !line_buf_converted)
	{
		int			mblen = pg_encoding_mblen(client_encoding, (unsigned char*)s);
		
		for (start = s; *s != eol && s < start + len; s += mblen)
			mblen = pg_encoding_mblen(client_encoding, (unsigned char*)s);
		
		return ((*s == eol) ? (char *) s : NULL);
	}
	else
		return memchr(s,eol,len);
}


char *
scanCSVLine(const char *s, char eol, char escapec, char quotec, size_t len)
{
	const char *start;
	
	if (client_encoding_only && !line_buf_converted)
	{
		int			mblen = pg_encoding_mblen(client_encoding, (unsigned char*)s);
		
		for (start = s; *s != eol &&  s < start + len; s += mblen)
		{
			if (in_quote && *s == escapec)
				last_was_esc = !last_was_esc;
			if (*s == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (*s != escapec)
				last_was_esc = false;
			
			mblen = pg_encoding_mblen(client_encoding, (unsigned char*)s);
        }
	}
	else
		/* safe to scroll byte by byte */
	{
		for (start = s; *s != eol  && s < start + len; s++)
		{
			if (in_quote && *s == escapec)
				last_was_esc = !last_was_esc;
			if (*s == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (*s != escapec)
				last_was_esc = false;
		}
		
	}

	if(*s == eol)
		last_was_esc = false;
	
	return ((*s == eol) ? (char *) s : NULL);
}



