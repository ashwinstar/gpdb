/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/copy.c,v 1.273 2006/10/06 17:13:58 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "storage/fd.h"
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
#include "cdb/cdbsreh.h"
#include "cdb/cdbrelsize.h"


/* DestReceiver for COPY (SELECT) TO */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	CopyState	cstate;			/* CopyStateData for the command */
} DR_copy;

static const char BinarySignature[] = "PGCOPY\n\377\r\n";   /* 11 bytes total */

/* non-export function prototypes */
static void DoCopyTo(CopyState cstate);
extern void CopyToDispatch(CopyState cstate);
static void CopyTo(CopyState cstate);
static void CopyOneRowTo(CopyState cstate, Oid tupleOid,
						                         Datum *values, bool *nulls);
extern void CopyFromDispatch(CopyState cstate);
static void CopyFrom(CopyState cstate);
static Datum CopyReadBinaryAttribute(CopyState cstate, int column_no __attribute__((unused)), FmgrInfo *flinfo,
						Oid typioparam, int32 typmod, bool *isnull);
static char *CopyReadOidAttr(CopyState cstate, bool *isnull);
static void CopyAttributeOutText(CopyState cstate, char *string);
static void CopyAttributeOutCSV(CopyState cstate, char *string,
								bool use_quote, bool single_attr);
static bool DetectLineEnd(CopyState cstate, size_t bytesread  __attribute__((unused)));

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

static int CopyExtractLineNum(CopyState cstate);
static void concatenateEol(CopyState cstate);
static char *escape_quotes(const char *src);

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
		/* Shouldn't have any unsent data */
		Assert(cstate->fe_msgbuf->len == 0);

		/* Send Copy Done message */
		pq_putemptymessage('c');
	}
	else
	{
		CopySendData(cstate, "\\.", 2);
		/* Need to flush out the trailer (this also appends a newline) */
 	 	CopySendEndOfRow(cstate);
		pq_endcopyout(false);
	}
}


/*----------
 * CopySendData sends output data to the destination (file or frontend)
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 * CopySendEndOfRow does the appropriate thing at end of each data row
 *  (data is not actually flushed except by CopySendEndOfRow)
 *
 * NB: no data conversion is applied by these functions
 *----------
 */
static void
CopySendData(CopyState cstate, const void *databuf, int datasize)
{
	if(!cstate->is_copy_in) /* copy out */
	{
		appendBinaryStringInfo(cstate->fe_msgbuf, (char *) databuf, datasize);
	}
	else /* hack for: copy in */
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

/* AXG: Note that this will both add a newline AND flush the data.
 * For the dispatcher COPY TO we don't want to use this method since
 * our newlines already exist. We use another new method similar to 
 * this one to flush the data
 */
static void
CopySendEndOfRow(CopyState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;
	
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
			
			(void) fwrite(fe_msgbuf->data, fe_msgbuf->len,
						  1, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to COPY file: %m")));
				break;
		case COPY_OLD_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			
			if (pq_putbytes(fe_msgbuf->data, fe_msgbuf->len))
			{
				/* no hope of recovering connection sync, so FATAL */
				ereport(FATAL,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection lost during COPY to stdout")));
			}
				break;
		case COPY_NEW_FE:
			/* The FE/BE protocol uses \n as newline for all platforms */
			if (!cstate->binary)
				CopySendChar(cstate, '\n');
			
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', fe_msgbuf->data, fe_msgbuf->len);
			break;
	}
	
	/* Reset fe_msgbuf to empty */
	fe_msgbuf->len = 0;
	fe_msgbuf->data[0] = '\0';
}

/*
 * AXG: This one is equivalent to CopySendEndOfRow() besides that
 * it doesn't send end of row - it just flushed the data. We need
 * this method for the dispatcher COPY TO since it already has data
 * with newlines (from the executors).
 */
static void
CopyToDispatchFlush(CopyState cstate)
{
	StringInfo	fe_msgbuf = cstate->fe_msgbuf;
	
	switch (cstate->copy_dest)
	{
		case COPY_FILE:

			(void) fwrite(fe_msgbuf->data, fe_msgbuf->len,
						  1, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to COPY file: %m")));
				break;
		case COPY_OLD_FE:

			if (pq_putbytes(fe_msgbuf->data, fe_msgbuf->len))
			{
				/* no hope of recovering connection sync, so FATAL */
				ereport(FATAL,
						(errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection lost during COPY to stdout")));
			}
				break;
		case COPY_NEW_FE:
			
			/* Dump the accumulated row as one CopyData message */
			(void) pq_putmessage('d', fe_msgbuf->data, fe_msgbuf->len);
			break;
	}
	
	/* Reset fe_msgbuf to empty */
	fe_msgbuf->len = 0;
	fe_msgbuf->data[0] = '\0';
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
 * (<from> = TRUE means we are inserting into the table.) In the "TO" case
 * we also support copying the output of an arbitrary SELECT query.
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
 * a file), recognize a "\." on a line by itself as EOF. Also recognize
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
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL || Gp_role == GP_ROLE_EXECUTE);
	List	   *attnamelist = stmt->attlist;
	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;
	ListCell   *option;
	TupleDesc   tupDesc;
	int         num_phys_attrs;
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
						
			/* 
			 * MPP-2010: unfortunately serialization function doesn't
			 * distinguish between 0x0 and empty string. Therefore we
			 * must assume that if NULL AS was indicated and has no value
			 * the actual value is an empty string.
			 */
			if(!cstate->null_print)
				cstate->null_print = "";
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
	
	cstate->err_loc_type = ROWNUM_ORIGINAL; 
		
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
	 * Error handling setup
	 */
	if(stmt->sreh)
	{		
		/* Single row error handling requested */
		SingleRowErrorDesc *sreh;
		sreh = (SingleRowErrorDesc *)stmt->sreh;
		
		if (!is_from)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling only available using COPY FROM")));
		
		if (cstate->binary)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling is not supported in BINARY format")));
		
		if (sreh->errtable)
		{
			cstate->errMode = SREH_LOG;
		}
		else
		{
			cstate->errMode = SREH_IGNORE;
			if (sreh->is_keep)
				ereport(ERROR,
						(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
						errmsg("KEEP may only be specified with a LOG INTO errortable clause")));
			
		}
		
		if (!sreh->is_limit_in_rows)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_YET),
					 errmsg("Segment reject limit in percentage is not yet supported")));			
		
	
		cstate->cdbsreh = makeCdbSreh(sreh->is_keep, 
									  sreh->reusing_existing_errtable,
									  sreh->rejectlimit, 
									  sreh->is_limit_in_rows, 
									  sreh->errtable,
									  stmt->filename, 
									  stmt->relation->relname);
	}
	else
	{
		/* No single row error handling requested. Use "all or nothing" */
		cstate->cdbsreh = NULL; /* default - no SREH */
		cstate->errMode = ALL_OR_NOTHING; /* default */
	}
	
	/*
	 * Only single-character delimiter strings are supported.
	 */
	if (strlen(cstate->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single character")));
	
	/* Disallow end-of-line characters */
	if (strchr(cstate->delim, '\r') != NULL ||
	    strchr(cstate->delim, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be newline or carriage return")));
	
	if (strchr(cstate->null_print, '\r') != NULL ||
		strchr(cstate->null_print, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY null representation cannot use newline or carriage return")));
	
	/* Disallow backslash in non-CSV mode */
	if (!cstate->csv_mode && strchr(cstate->delim, '\\') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be backslash")));
	
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
		if (!pg_strcasecmp(cstate->escape, "off"))
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

	/* Disallow file COPY except to superusers. */
	if (!pipe && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to COPY to or from a file"),
				 errhint("Anyone can COPY to stdout or from stdin. "
						 "psql's \\copy command also works for anyone.")));

	elog(DEBUG1,"DoCopy starting");
	if (stmt->relation)
	{
		Assert(!stmt->query);
		cstate->queryDesc = NULL;
		
		/* Open and lock the relation, using the appropriate lock type. */
		cstate->rel = heap_openrv(stmt->relation,
								  (is_from ? RowExclusiveLock : AccessShareLock));
		
		/* Check relation permissions. */
		aclresult = pg_class_aclcheck(RelationGetRelid(cstate->rel),
									  GetUserId(),
									  required_access);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(cstate->rel));
		
		/* check read-only transaction */
		if (XactReadOnly && is_from &&
			!isTempNamespace(RelationGetNamespace(cstate->rel)))
			ereport(ERROR,
					(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
					 errmsg("transaction is read-only")));
		
		/* Don't allow COPY w/ OIDs to or from a table without them */
		if (cstate->oids && !cstate->rel->rd_rel->relhasoids)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("table \"%s\" does not have OIDs",
							RelationGetRelationName(cstate->rel))));
		
		tupDesc = RelationGetDescr(cstate->rel);
	}
	else
	{
		Query	   *query = stmt->query;
		List	   *rewritten;
		Plan	   *plan;
		DestReceiver *dest;
		
		Assert(query);
		Assert(!is_from);
		cstate->rel = NULL;
		
		/* Don't allow COPY w/ OIDs from a select */
		if (cstate->oids)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT) WITH OIDS is not supported")));
		
		if (query->into)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY (SELECT INTO) is not supported")));
		
		/*
		 * The query has already been through parse analysis, but not
		 * rewriting or planning.  Do that now.
		 *
		 * Because the planner is not cool about not scribbling on its input,
		 * we make a preliminary copy of the source querytree.  This prevents
		 * problems in the case that the COPY is in a portal or plpgsql
		 * function and is executed repeatedly.  (See also the same hack in
												  * EXPLAIN, DECLARE CURSOR and PREPARE.)  XXX the planner really
		 * shouldn't modify its input ... FIXME someday.
		 */
		query = copyObject(query);
		Assert(query->commandType == CMD_SELECT);
		
		/*
		 * Must acquire locks in case we didn't come fresh from the parser.
		 * XXX this also scribbles on query, another reason for copyObject
		 */
		AcquireRewriteLocks(query);
		
		/* Rewrite through rule system */
		rewritten = QueryRewrite(query);
		
		/* We don't expect more or less than one result query */
		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result");
		
		query = (Query *) linitial(rewritten);
		Assert(query->commandType == CMD_SELECT);
		
		/* plan the query */
		plan = planner(query, false, 0, NULL);
		
		/*
		 * Update snapshot command ID to ensure this query sees results of any
		 * previously executed queries.  (It's a bit cheesy to modify
		 * ActiveSnapshot without making a copy, but for the limited ways in
		 * which COPY can be invoked, I think it's OK, because the active
		 * snapshot shouldn't be shared with anything else anyway.)
		 */
		ActiveSnapshot->curcid = GetCurrentCommandId();
		
		/* Create dest receiver for COPY OUT */
		dest = CreateDestReceiver(DestCopyOut, NULL);
		((DR_copy *) dest)->cstate = cstate;
		
		/* Create a QueryDesc requesting no output */
		cstate->queryDesc = CreateQueryDesc(query, plan,
											ActiveSnapshot, InvalidSnapshot,
											dest, NULL, false);
		
		/*
		 * Call ExecutorStart to prepare the plan for execution.
		 *
		 * ExecutorStart computes a result tupdesc for us
		 */
		ExecutorStart(cstate->queryDesc, 0);
		
		tupDesc = cstate->queryDesc->tupDesc;
	}
	
	/* Generate or convert list of attributes to process */
	cstate->attnumlist = CopyGetAttnums(tupDesc, cstate->rel, attnamelist);
	
	num_phys_attrs = tupDesc->natts;
	
	/* Convert FORCE QUOTE name list to per-column flags, check validity */
	cstate->force_quote_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (force_quote)
	{
		List	   *attnums;
		ListCell   *cur;
		
		attnums = CopyGetAttnums(tupDesc, cstate->rel, force_quote);
		
		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			
			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE QUOTE column \"%s\" not referenced by COPY",
								NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_quote_flags[attnum - 1] = true;
		}
	}
	
	/* Convert FORCE NOT NULL name list to per-column flags, check validity */
	cstate->force_notnull_flags = (bool *) palloc0(num_phys_attrs * sizeof(bool));
	if (force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;
		
		attnums = CopyGetAttnums(tupDesc, cstate->rel, force_notnull);
		
		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);
			
			if (!list_member_int(cstate->attnumlist, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("FORCE NOT NULL column \"%s\" not referenced by COPY",
								NameStr(tupDesc->attrs[attnum - 1]->attname))));
			cstate->force_notnull_flags[attnum - 1] = true;
		}
	}
	
	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->processed = 0;

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to
	 * validate data in multibyte encodings. However, transcoding must
	 * be skipped for COPY FROM in executor mode since data already arrived 
	 * in server encoding (was validated and trancoded by dispatcher mode 
	 * COPY).
	 */
	cstate->client_encoding = pg_get_client_encoding();
	cstate->need_transcoding =
		((cstate->client_encoding != GetDatabaseEncoding() ||
		  pg_database_encoding_max_length() > 1) &&
		 (!is_from || (Gp_role != GP_ROLE_EXECUTE)) );
	if (Gp_role == GP_ROLE_DISPATCHAGENT)
		cstate->need_transcoding = false;
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->client_encoding);
	cstate->line_buf_converted = (Gp_role == GP_ROLE_EXECUTE ? true : false);

	cstate->copy_dest = COPY_FILE;		/* default */
	cstate->filename = (Gp_role == GP_ROLE_EXECUTE ? NULL : stmt->filename); /* QE COPY always uses STDIN */
	cstate->copy_file = NULL;
	cstate->fe_msgbuf = NULL;
	cstate->fe_eof = false;

	/*
	 * some greenplum db specific vars
	 */
	cstate->is_copy_in = (is_from ? true : false);
	if (is_from)
	{
		cstate->error_on_executor = false;
		initStringInfo(&(cstate->executor_err_context));		
	}
	
	if (is_from)				/* copy from file to database */
	{			
		bool		pipe = (cstate->filename == NULL);
		
		Assert(cstate->rel);
		
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
		
		if(stmt->sreh && !cstate->rel->rd_cdbpolicy)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling only available for distributed user tables")));		
		
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
			
			cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_R);
			
			if (cstate->copy_file == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" for reading: %m",
								cstate->filename)));
			
			fstat(fileno(cstate->copy_file), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(cstate->copy_file);
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a directory", cstate->filename)));
			}
		}
		
		if ((Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) 
			&& cstate->rel->rd_cdbpolicy != NULL)
			/* data needs to get dispatched to segment databases */
			CopyFromDispatch(cstate);
		else
			/* data needs to get inserted locally */
			CopyFrom(cstate);
		
		if (!pipe)
		{
			if (FreeFile(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to file \"%s\": %m",
								cstate->filename)));
		}
	}
	else		/* copy from database to file */
		DoCopyTo(cstate);

	/*
	 * Close the relation or query.  If reading, we can release the
	 * AccessShareLock we got; if writing, we should hold the lock until end
	 * of transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	if (cstate->heapInsertDesc)
		heap_endinsert(cstate->heapInsertDesc);
	if (cstate->rel)
		heap_close(cstate->rel, (is_from ? NoLock : AccessShareLock));
	else
	{
		/* Close down the query and free resources. */
		ExecutorEnd(cstate->queryDesc);
		FreeQueryDesc(cstate->queryDesc);
	}
	
	/* Clean up single row error handling related memory */
	if(cstate->cdbsreh)
		destroyCdbSreh(cstate->cdbsreh);
	
	/* Clean up storage (probably not really necessary) */
	processed = cstate->processed;
	
	pfree(cstate->attribute_buf.data);
	pfree(cstate->line_buf.data);
	pfree(cstate);

	return processed;
}

/*
 * This intermediate routine exists mainly to localize the effects of setjmp
 * so we don't need to plaster a lot of variables with "volatile".
 */
static void
DoCopyTo(CopyState cstate)
{
	bool		pipe = (cstate->filename == NULL);
	
	if (cstate->rel)
	{
		if (cstate->rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (cstate->rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from view \"%s\"",
								RelationGetRelationName(cstate->rel)),
						 errhint("Try the COPY (SELECT ...) TO variant.")));
			else if (cstate->rel->rd_rel->relkind == RELKIND_EXTERNAL)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot copy from external relation \"%s\"",
								RelationGetRelationName(cstate->rel)),
						 errhint("Try the COPY (SELECT ...) TO variant.")));			
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
		 * Prevent write to relative path ... too easy to shoot oneself in the
		 * foot by overwriting a database file ...
		 */
		if (!is_absolute_path(cstate->filename))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("relative path not allowed for COPY to file")));
		
		oumask = umask((mode_t) 022);
		cstate->copy_file = AllocateFile(cstate->filename, PG_BINARY_W);
		umask(oumask);
		
		if (cstate->copy_file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for writing: %m",
							cstate->filename)));
		
		fstat(fileno(cstate->copy_file), &st);
		if (S_ISDIR(st.st_mode))
		{
			FreeFile(cstate->copy_file);
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a directory", cstate->filename)));
		}
	}
	
	PG_TRY();
	{
		if (cstate->fe_copy)
			SendCopyBegin(cstate);
		
		/*
		 * We want to dispatch COPY TO commands only in the case that
		 * we are the dispatcher and we are copying from a user relation
		 * (a relation where data is distributed in the segment databases).
		 * Otherwize, if we are not the dispatcher *or* if we are
		 * doing COPY (SELECT) we just go straight to work, without
		 * dispatching COPY commands to executors.
		 */
		if ((Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) 
			&& cstate->rel && cstate->rel->rd_cdbpolicy)
			CopyToDispatch(cstate);		
		else
			CopyTo(cstate);		
		
		if (cstate->fe_copy)
			SendCopyEnd(cstate);
	}
	PG_CATCH();
	{
		/*
		 * Make sure we turn off old-style COPY OUT mode upon error. It is
		 * okay to do this in all cases, since it does nothing if the mode is
		 * not on.
		 */
		pq_endcopyout(true);
		PG_RE_THROW();
	}
	PG_END_TRY();
	
	if (!pipe)
	{
		if (FreeFile(cstate->copy_file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							cstate->filename)));
	}
}


/*
 * Copy from relation or query TO file.
 */
static void
CopyTo(CopyState cstate)
{
	TupleDesc	tupDesc;
	int			num_phys_attrs;
	Form_pg_attribute *attr;
	ListCell   *cur;
	
	if (cstate->rel)
		tupDesc = RelationGetDescr(cstate->rel);
	else
		tupDesc = cstate->queryDesc->tupDesc;
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	cstate->null_print_client = cstate->null_print;		/* default */
	
	/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
	cstate->fe_msgbuf = makeStringInfo();

	/* Get info about the columns we need to process. */
	cstate->out_functions = (FmgrInfo *) palloc(num_phys_attrs * sizeof(FmgrInfo));
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Oid			out_func_oid;
		bool		isvarlena;
		
		if (cstate->binary)
			getTypeBinaryOutputInfo(attr[attnum - 1]->atttypid,
									&out_func_oid,
									&isvarlena);
		else
			getTypeOutputInfo(attr[attnum - 1]->atttypid,
							  &out_func_oid,
							  &isvarlena);
		fmgr_info(out_func_oid, &cstate->out_functions[attnum - 1]);
	}
	
	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype output routines, and should be faster than retail pfree's
	 * anyway.	(We don't need a whole econtext as CopyFrom does.)
	 */
	cstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "COPY TO",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
		
	if (cstate->binary)
	{
		/*
		 * We don't want binary header in exec mode. it's
		 * taken care of in the dispatcher code.
		 */
		if (Gp_role != GP_ROLE_EXECUTE && Gp_role != GP_ROLE_DISPATCHAGENT)
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
			cstate->null_print_client = pg_server_to_client(cstate->null_print,
															cstate->null_print_len);
		
		/* if a header has been requested send the line */
		if (cstate->header_line)
		{
			/* header should not be printed in execute mode. */
			if (Gp_role != GP_ROLE_EXECUTE && Gp_role != GP_ROLE_DISPATCHAGENT)
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
	
	if (cstate->rel)
	{
		Datum	   *values;
		bool	   *nulls;
		HeapScanDesc scandesc;
		HeapTuple	tuple;
		
		values = (Datum *) palloc(num_phys_attrs * sizeof(Datum));
		nulls = (bool *) palloc(num_phys_attrs * sizeof(bool));
		
		scandesc = heap_beginscan(cstate->rel, ActiveSnapshot, 0, NULL);
		
		while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
		{
			CHECK_FOR_INTERRUPTS();
			
			/* Deconstruct the tuple ... faster than repeated heap_getattr */
			heap_deform_tuple(tuple, tupDesc, values, nulls);
			
			/* Format and send the data */
			CopyOneRowTo(cstate, HeapTupleGetOid(tuple), values, nulls);
		}
		
		heap_endscan(scandesc);
	}
	else
	{
		Assert(Gp_role != GP_ROLE_EXECUTE);
		
		/* run the plan --- the dest receiver will send tuples */
		ExecutorRun(cstate->queryDesc, ForwardScanDirection, 0L);
	}
	
	if (cstate->binary)
	{
		if (Gp_role != GP_ROLE_EXECUTE && Gp_role != GP_ROLE_DISPATCHAGENT)
		{
			/* Generate trailer for a binary copy */
			CopySendInt16(cstate, -1);

			/* Need to flush out the trailer */
			CopySendEndOfRow(cstate);
		}
	}
	

	MemoryContextDelete(cstate->rowcontext);
}

/*
 * Emit one row during CopyTo().
 */
static void
CopyOneRowTo(CopyState cstate, Oid tupleOid, Datum *values, bool *nulls)
{
	bool		need_delim = false;
	FmgrInfo   *out_functions = cstate->out_functions;
	MemoryContext oldcontext;
	ListCell   *cur;
	char	   *string;
	
	MemoryContextReset(cstate->rowcontext);
	oldcontext = MemoryContextSwitchTo(cstate->rowcontext);
	
	if (cstate->binary)
	{
		/* Binary per-tuple header */
		CopySendInt16(cstate, list_length(cstate->attnumlist));
		/* Send OID if wanted --- note attnumlist doesn't include it */
		if (cstate->oids)
		{
			/* Hack --- assume Oid is same size as int32 */
			CopySendInt32(cstate, sizeof(int32));
			CopySendInt32(cstate, tupleOid);
		}
	}
	else
	{
		/* Text format has no per-tuple header, but send OID if wanted */
		/* Assume digits don't need any quoting or encoding conversion */
		if (cstate->oids)
		{
			string = DatumGetCString(DirectFunctionCall1(oidout,
														 ObjectIdGetDatum(tupleOid)));
			CopySendString(cstate, string);
			need_delim = true;
		}
	}
	
	foreach(cur, cstate->attnumlist)
	{
		int			attnum = lfirst_int(cur);
		Datum		value = values[attnum - 1];
		bool		isnull = nulls[attnum - 1];
		
		if (!cstate->binary)
		{
			if (need_delim)
				CopySendChar(cstate, cstate->delim[0]);
			need_delim = true;
		}
		
		if (isnull)
		{
			if (!cstate->binary)
				CopySendString(cstate, cstate->null_print_client);
			else
				CopySendInt32(cstate, -1);
		}
		else
		{
			if (!cstate->binary)
			{
				string = OutputFunctionCall(&out_functions[attnum - 1],
											value);
				if (cstate->csv_mode)
					CopyAttributeOutCSV(cstate, string,
										cstate->force_quote_flags[attnum - 1],
										list_length(cstate->attnumlist) == 1);
				else
					CopyAttributeOutText(cstate, string);
			}
			else
			{
				bytea	   *outputbytes;
				
				outputbytes = SendFunctionCall(&out_functions[attnum - 1],
											   value);
				CopySendInt32(cstate, VARSIZE(outputbytes) - VARHDRSZ);
				CopySendData(cstate, VARDATA(outputbytes),
							 VARSIZE(outputbytes) - VARHDRSZ);
			}
		}
	}
	
	CopySendEndOfRow(cstate);
	
	MemoryContextSwitchTo(oldcontext);
	
	cstate->processed++;
}

/*
 * Copy from relation TO file. Starts a COPY TO command on each of
 * the executors and gathers all the results and writes it out.
 */
 void
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

	/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
	cstate->fe_msgbuf = makeStringInfo();

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
	
	if (cstate->oids)
		appendStringInfo(&cdbcopy_cmd, " OIDS");
	
	if (cstate->binary)
	{
		appendStringInfo(&cdbcopy_cmd, " BINARY");
	}
	else
	{
		appendStringInfo(&cdbcopy_cmd, " DELIMITER AS E'%s'", cstate->delim);
		appendStringInfo(&cdbcopy_cmd, " NULL AS E'%s'", escape_quotes(cstate->null_print));
		
		/* if default escape in text format ("\") leave expression out */
		if (!cstate->csv_mode && strcmp(cstate->escape, "\\") != 0) 
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS E'%s'", cstate->escape);
	}
	
	if (cstate->csv_mode)
	{
		appendStringInfo(&cdbcopy_cmd, " CSV");
		appendStringInfo(&cdbcopy_cmd, " QUOTE AS E'%s'", escape_quotes(cstate->quote));
		appendStringInfo(&cdbcopy_cmd, " ESCAPE AS E'%s'", escape_quotes(cstate->escape));
		
		/* do NOT include HEADER. Header row is "swallowed" by dispatcher COPY */
	}
	
	/*
	 * Start a COPY command in every db of every segment in Greenplum Database.
	 *
	 * From this point in the code we need to be extra careful
	 * about error handling. ereport() must not be called until
	 * the COPY command sessions are closed on the executors.
	 * Calling ereport() will leave the executors hanging in
	 * COPY state.
	 */
	if (Gp_role != GP_ROLE_DISPATCHAGENT)
	{
		elog(DEBUG5, "COPY command sent to segdbs: %s", cdbcopy_cmd.data);
		if (cdbCopyStart(cdbCopy, cdbcopy_cmd.data) == -1)
		{
			/* get error message from CopyStart */
			appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
			
			/* TODO: end COPY in all the segdbs in progress */
			//cdbCopyEnd(cdbCopy);
			
			ereport(ERROR,
					(errcode(ERRCODE_CDB_INTERNAL_ERROR),
					 errmsg("%s", cdbcopy_err.data)));
		}
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
			
			/* add a newline and flush the data */
			CopySendEndOfRow(cstate);
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
		{
			/*
			 * in the dispatcher we receive chunks of whole rows with row endings.
			 * We don't want to use CopySendEndOfRow() b/c it adds row endings and
			 * also b/c it's intended for a single row at a time. Therefore we need
			 * to fill in the out buffer and just flush it instead.
			 */
			CopySendData(cstate, (void *) cdbCopy->copy_out_buf.data, cdbCopy->copy_out_buf.len);
			CopyToDispatchFlush(cstate);
		}
			
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
		CopyToDispatchFlush(cstate);
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
	 * Make sure to not modify the actual line_buf here (for example
	 * in limit_printout_length() ) as we may got here not because of
	 * a data error but because of an innocent elog(NOTICE) for example
	 * somewhere in the code. We make a copy of line_buf and use it for 
	 * the error context purposes so that a non-data related elog call
	 * won't corrupt our data in line_buf.
	 */
	StringInfoData copy_of_line_buf;
				
	initStringInfo(&copy_of_line_buf);
	appendStringInfoString(&copy_of_line_buf, cstate->line_buf.data);
	
	/*
	 * If our (copy of) linebuf has the embedded original row number, remove 
     * it. It is not part of the actual data, and should not be displayed.
	 */
	if(cstate->err_loc_type == ROWNUM_EMBEDDED)
	{
		/* the following is a compacted mod of CopyExtractLineNum */
		int lineno_str_len = 0;
		char *line_start = copy_of_line_buf.data;
		char *lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
		
		/* Calling Insist or Assert here will cause recursive errors */
		if (!(lineno_delim && (lineno_delim != line_start)))
			write_log("Error in copy_in_error_callback: !(lineno_delim && (lineno_delim != line_start))\n");
		else
		{
			lineno_str_len = lineno_delim - line_start + 1;
			copy_of_line_buf.data += lineno_str_len; /* advance beyond original lineno */
			copy_of_line_buf.len -= lineno_str_len;
	
			/* skip target_seg */
			line_start = copy_of_line_buf.data;
			lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
			
			if (lineno_delim && (lineno_delim != line_start))
			{	
				lineno_str_len = lineno_delim - line_start + 1;
				copy_of_line_buf.data += lineno_str_len; /* advance beyond target_seg */
				copy_of_line_buf.len -= lineno_str_len;
			}
		}
	}
	
	/*
	 * If we saved the error context from a QE in cdbcopy.c append it here.
	 */
	if((Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)
			&& cstate->executor_err_context.len > 0)
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

			if(!cstate->error_on_executor) /* don't print out context if error wasn't local */
				errcontext("COPY %s, line %d, column %s",
						   cstate->cur_relname, cstate->cur_lineno, cstate->cur_attname);
		}
		else
		{

			/* error is relevant to a particular line */
			if (cstate->line_buf_converted || !cstate->need_transcoding)
			{
				truncateEol(&copy_of_line_buf, cstate->eol_type);
				limit_printout_length(&copy_of_line_buf);

				if(!cstate->error_on_executor) /* don't print out context if error wasn't local */
					errcontext("COPY %s, line %d: \"%s\"", cstate->cur_relname, cstate->cur_lineno, copy_of_line_buf.data);
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
				if(!cstate->error_on_executor)
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
void
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

/* ==========================================================================
 * The follwing macros aid in major refactoring of data processing code (in
 * CopyFrom(+Dispatch)). We use macros because in some cases the code must be in
 * line in order to work (for example elog_dismiss() in PG_CATCH) while in 
 * other cases we'd like to inline the code for performance reasons.
 *
 * NOTE that an almost identical set of macros exists in fileam.c. If you make 
 * changes here you may want to consider taking a look there as well.
 * ==========================================================================
 */

#define RESET_LINEBUF \
cstate->line_buf.len = 0; \
cstate->line_buf.data[0] = '\0'; \
cstate->line_buf.cursor = 0;

#define RESET_ATTRBUF \
cstate->attribute_buf.len = 0; \
cstate->attribute_buf.data[0] = '\0'; \
cstate->attribute_buf.cursor = 0;

#define RESET_LINEBUF_WITH_LINENO \
line_buf_with_lineno.len = 0; \
line_buf_with_lineno.data[0] = '\0'; \
line_buf_with_lineno.cursor = 0;

/*
 * A data error happened. This code block will always be inside a PG_CATCH()
 * block right when a higher stack level produced an error. We handle the error
 * by checking which error mode is set (SREH or all-or-nothing) and do the right
 * thing accordingly. Note that we MUST have this code in a macro (as opposed 
 * to a function) as elog_dismiss() has to be inlined with PG_CATCH in order to 
 * access local error state variables.
 *
 * changing me? take a look at FILEAM_HANDLE_ERROR in fileam.c as well.
 */
#define COPY_HANDLE_ERROR \
if(cstate->errMode == ALL_OR_NOTHING) \
{ \
	/* re-throw error and abort */ \
	if(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) \
		cdbCopyEnd(cdbCopy); \
	PG_RE_THROW(); \
} \
else \
{ \
	/* SREH - release error state and handle error */ \
\
	ErrorData	*edata; \
	MemoryContext oldcontext;\
	bool	should_free = false; \
	cur_row_rejected = true; \
\
	/* save a copy of the error info */ \
	oldcontext = MemoryContextSwitchTo(cstate->cdbsreh->badrowcontext);\
	edata = CopyErrorData();\
	MemoryContextSwitchTo(oldcontext);\
\
	if(!elog_dismiss(DEBUG5)) \
		PG_RE_THROW(); /* <-- hope to never get here! */ \
\
	if(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)\
	{\
		/* count this row in total rows processed on QD */ \
		cstate->processed++; \
\
		Insist(cstate->err_loc_type == ROWNUM_ORIGINAL);\
		cstate->cdbsreh->rawdata = (char *) palloc(strlen(cstate->line_buf.data) * \
												   sizeof(char) + 1 + 24); \
\
		should_free = true; \
		sprintf(cstate->cdbsreh->rawdata, "%d%c%d%c%s", \
			    original_lineno_for_qe, \
				ORIGINAL_LINENO_DELIM, \
				target_seg, \
				ORIGINAL_LINENO_DELIM, \
				cstate->line_buf.data);	\
	}\
	else\
	{\
		/* truncate trailing eol chars if we need to store this row in errtbl */ \
		if(cstate->cdbsreh->errtbl) \
			truncateEol(&cstate->line_buf, cstate->eol_type); \
\
		if(Gp_role == GP_ROLE_EXECUTE)\
		{\
			/* if line has embedded rownum, update the cursor to the pos right after */ \
			Insist(cstate->err_loc_type == ROWNUM_EMBEDDED);\
			cstate->line_buf.cursor = 0;\
			cstate->cur_lineno = CopyExtractLineNum(cstate); \
		}\
\
		cstate->cdbsreh->rawdata = cstate->line_buf.data + cstate->line_buf.cursor; \
	}\
\
	cstate->cdbsreh->is_server_enc = cstate->line_buf_converted; \
	cstate->cdbsreh->linenumber = cstate->cur_lineno; \
	cstate->cdbsreh->errmsg = edata->message; \
\
	HandleSingleRowError(cstate->cdbsreh); \
\
	if(should_free) \
		pfree(cstate->cdbsreh->rawdata); \
}

/*
 * if in SREH mode and data error occured it was already handled in 
 * COPY_HANDLE_ERROR. Therefore, skip to the next row before attempting
 * to do any further processing on this one. There's a QE and QD versions
 * since the QE doesn't have a linebuf_with_lineno stringInfo.
 */
#define QD_GOTO_NEXT_ROW \
RESET_LINEBUF_WITH_LINENO; \
RESET_LINEBUF; \
cur_row_rejected = false; /* reset for next run */ \
continue; /* move on to the next data line */

#define QE_GOTO_NEXT_ROW \
RESET_LINEBUF; \
cur_row_rejected = false; /* reset for next run */ \
continue; /* move on to the next data line */

#define IF_REJECT_LIMIT_REACHED_ABORT \
if(cstate->cdbsreh->rejectcount >= cstate->cdbsreh->rejectlimit) \
{ \
	if(Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) \
		cdbCopyEnd(cdbCopy); \
			\
	ereport(ERROR, \
			(errcode(ERRCODE_T_R_GP_REJECT_LIMIT_REACHED), \
			 (errmsg("Segment reject limit reached after encountering %d data errors. " \
					 "Aborting operation.",cstate->cdbsreh->rejectcount) \
			  ))); \
}


/*
 * Copy FROM file to relation.
 */
 void
CopyFromDispatch(CopyState cstate)
{
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	num_phys_attrs,
				attr_count,
				extra_attr_count = 0, /* count extra attributes we add in the dispatcher COPY
							         usually non constant defaults we pre-evaluate in here */
				num_defaults;
	FmgrInfo   *in_functions;
	FmgrInfo   *out_functions; /* for handling defaults in Greenplum Database */
	FmgrInfo	oid_in_function;
	Oid		   *typioparams;
	Oid			oid_typioparam;
	int			attnum;
	int			i;
	int			p_index;
	Oid			in_func_oid;
	Oid			out_func_oid;
	Datum	   *values;
	char	   *nulls;
	int		   *attr_offsets;
	int			total_rejeted_from_qes = 0;
	bool		isnull;
	bool	   *isvarlena;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
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
	bool		cur_row_rejected = false;

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
	 * databases to execute there in gp_role execute
	 */
	StringInfoData cdbcopy_cmd;

	/*
	 * Variables for cdbpolicy
	 */
	GpPolicy  *policy;			/* the partitioning policy for this table			 */
	int			p_nattrs;		/* num of partitioning keys in the policy			 */

	/*
	 * Variables for original row number tracking
	 */
	StringInfoData line_buf_with_lineno;
	int			original_lineno_for_qe;

	/*
	 * Variables for cdbhash
	 */
	CdbHash    *cdbHash;		/* a CdbHash API object		 */
	AttrNumber	h_attnum;		/* hash key attribute number */
	Datum		h_key;			/* hash key value			 */
	Oid			h_oid;			/* hash key data type		 */
	unsigned int target_seg = 0;	/* result segment of cdbhash */
	
	tupDesc = RelationGetDescr(cstate->rel);
	attr = tupDesc->attrs;
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);
	num_defaults = 0;

	/*
	 * Init original row number tracking vars
	 */
	initStringInfo(&line_buf_with_lineno);
	original_lineno_for_qe = 1;
	
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

	/* Set up a tuple slot too */
	slot = MakeSingleTupleTableSlot(tupDesc);

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
	
	/* add cdbCopy reference to cdbSreh (if needed) */
	if(cstate->errMode != ALL_OR_NOTHING)
		cstate->cdbsreh->cdbcopy = cdbCopy;
	
	/*
	 * get the CDB policy for this table and prepare for hashing.
	 */
	policy = GpPolicyCopy(CurrentMemoryContext, cstate->rel->rd_cdbpolicy);
	
	/* policy should be PARTITIONED (normal tables) or ENTRY (no column tables) */
	Insist(Gp_role == GP_ROLE_DISPATCHAGENT || (policy &&
												policy->ptype != POLICYTYPE_UNDEFINED));
	
	if (policy)
		p_nattrs = policy->nattrs;	/* number of partitioning keys */
	else
		p_nattrs = 0;
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
	
	/* 
	 * NOTE: we used to always pass STDIN here to the QEs. But since we want
	 * the QEs to know the original file name for recording it in an error table
	 * (if they use one) we actually pass the filename here, and in the QE COPY
	 * we get it, save it, and then always revert back to actually using STDIN.
	 * (if we originally use STDIN we just pass it along and record that in the
	 * error table).
	 */
	if(cstate->filename)		
		appendStringInfo(&cdbcopy_cmd, " FROM '%s' WITH", cstate->filename);
	else
		appendStringInfo(&cdbcopy_cmd, " FROM STDIN WITH");

	if (cstate->oids)
		appendStringInfo(&cdbcopy_cmd, " OIDS");
	
	if (cstate->binary)
	{
		appendStringInfo(&cdbcopy_cmd, " BINARY");
	}
	else
	{
		appendStringInfo(&cdbcopy_cmd, " DELIMITER AS E'%s'", cstate->delim);
		appendStringInfo(&cdbcopy_cmd, " NULL AS E'%s'", escape_quotes(cstate->null_print));

		/* if default escape in text format ("\") leave expression out */
		if (!cstate->csv_mode && strcmp(cstate->escape, "\\") != 0) 
			appendStringInfo(&cdbcopy_cmd, " ESCAPE AS E'%s'", cstate->escape);
	}

	if (cstate->csv_mode)
	{
		appendStringInfo(&cdbcopy_cmd, " CSV");
		appendStringInfo(&cdbcopy_cmd, " QUOTE AS E'%s'", escape_quotes(cstate->quote));
		appendStringInfo(&cdbcopy_cmd, " ESCAPE AS E'%s'", escape_quotes(cstate->escape));

		/* do NOT include HEADER. Header row is "swallowed" by dispatcher COPY */
	}
	
	/* add single row error handling clauses if necessary */
	if (cstate->errMode != ALL_OR_NOTHING)
	{
		if (cstate->errMode == SREH_LOG)
		{
			appendStringInfo(&cdbcopy_cmd, " LOG ERRORS INTO %s.%s",
							 quote_identifier(get_namespace_name(RelationGetNamespace(cstate->cdbsreh->errtbl))),
							 quote_identifier(RelationGetRelationName(cstate->cdbsreh->errtbl)));
		}
		
		appendStringInfo(&cdbcopy_cmd, " SEGMENT REJECT LIMIT %d %s", 
						 cstate->cdbsreh->rejectlimit, (cstate->cdbsreh->is_limit_in_rows ? "ROWS" : "PERCENT"));
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
				 errmsg("%s", cdbcopy_err.data)));
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

				if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)
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
				cdbCopySendData(cdbCopy, i, header_buf.data, header_buf.len, true);

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
		RESET_LINEBUF;
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
	cstate->err_loc_type = ROWNUM_ORIGINAL;
		
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
				 * re-throw error. Note that we don't handle this error in
				 * any special way in SREH mode as it's not a data error.
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
					/* 
					 * TODO: use COPY_HANDLE_ERROR here, but make sure to
					 * ignore this error per the "note:" below.
					 */

					/* 
					 * got here? encoding conversion error occured on the 
					 * header line (first row).
					 */
					if(cstate->errMode == ALL_OR_NOTHING)
					{
						/* re-throw error and abort */
						cdbCopyEnd(cdbCopy);
						PG_RE_THROW();											
					}
					else
					{
						/* SREH - release error state */
						if(!elog_dismiss(DEBUG5))
							PG_RE_THROW(); /* hope to never get here! */
						
						/* 
						 * note: we don't bother doing anything special here.
						 * we are never interested in logging a header line
						 * error. just continue the workflow.
						 */
					}			
				}
				PG_END_TRY();
				
				cstate->cur_lineno++;
				RESET_LINEBUF;
				
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
					/* Get the line number of the first line of this data row */
					original_lineno_for_qe = cstate->cur_lineno + 1;

					PG_TRY();
					{
						/* Actually read the line into memory here */
						cstate->line_done = cstate->csv_mode ?
							CopyReadLineCSV(cstate, bytesread) :
							CopyReadLineText(cstate, bytesread);
					}
					PG_CATCH();
					{
						/* got here? encoding conversion/check error occured */
						COPY_HANDLE_ERROR;
					}
					PG_END_TRY();					

					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QD_GOTO_NEXT_ROW;
					}
						
					
					if(!cstate->line_done)
					{
						/*
						 * We did not finish reading a complete data line.
						 *
						 * If eof is not yet reached, we skip att parsing
						 * and read more data. But if eof _was_ reached it means
						 * that the original last data line is defective and
						 * we want to catch that error later on.
						 */
						if (!cstate->fe_eof || cstate->end_marker)
							break;
					}

					if (file_has_oids)
					{
						char	   *oid_string;

						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(cstate, &isnull);

						if (isnull)
						{
							/* got here? null in OID column error */
							
							if(cstate->errMode == ALL_OR_NOTHING)
							{
								/* report error and abort */
								cdbCopyEnd(cdbCopy);
								
								ereport(ERROR,
										(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
										 errmsg("null OID in COPY data.")));									
							}
							else
							{
								/* SREH */
								cstate->cdbsreh->rejectcount++;
								cur_row_rejected = true;
							}					
							
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
								/* got here? oid column conversion failed */
								COPY_HANDLE_ERROR;
							}
							PG_END_TRY();

							if (loaded_oid == InvalidOid)
							{
								if(cstate->errMode == ALL_OR_NOTHING)
								{
									/* report error and abort */
									cdbCopyEnd(cdbCopy);
									
									ereport(ERROR,
											(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
											 errmsg("invalid OID in COPY data.")));
								}
								else
								{
									/* SREH */
									cstate->cdbsreh->rejectcount++;
									cur_row_rejected = true;
								}					
							}

							cstate->cur_attname = NULL;
						}
						
						if(cur_row_rejected)
						{
							IF_REJECT_LIMIT_REACHED_ABORT
							QD_GOTO_NEXT_ROW;							
						}
					}
					

					/*
					 * try to parse the data line attributes.
					 */
					PG_TRY();
					{
						if (cstate->csv_mode)
							CopyReadAttributesCSV(cstate, nulls, attr_offsets, num_phys_attrs, attr);
						else
							CopyReadAttributesText(cstate, nulls, attr_offsets, num_phys_attrs, attr);
					}
					PG_CATCH();
					{
						/* Got here? missing or extra column error occured */
						COPY_HANDLE_ERROR;
					}
					PG_END_TRY();
					
					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QD_GOTO_NEXT_ROW;						
					}

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

								if (cstate->csv_mode && isnull && cstate->force_notnull_flags[m])
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
										values[m] = InputFunctionCall(&in_functions[m],
														  string,
														  typioparams[m],
														  attr[m]->atttypmod);
									}
									PG_CATCH();
									{
										/* Got here? datum conversion error */
										COPY_HANDLE_ERROR;
									}
									PG_END_TRY();

									cstate->cur_attname = NULL;
								}		/* end if (!isnull) */

								break;	/* go to next partitioning key
										 * attribute */
							}
						}		/* end foreach */
					}			/* end for partitioning indexes */

					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QD_GOTO_NEXT_ROW;						
					}

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
						appendStringInfo(&cdbcopy_err, "\nrow field count is %d, expected %d",
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
							appendStringInfo(&cdbcopy_err, "\ninvalid OID in COPY data");
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
							/* TODO: is an error really possible here? */
							COPY_HANDLE_ERROR;
						}
						PG_END_TRY();
						
						if(cur_row_rejected)
						{
							IF_REJECT_LIMIT_REACHED_ABORT;
							QD_GOTO_NEXT_ROW;							
						}

						/*
						 * prepare to concatinate next value:
						 * remove eol characters from end of line buf
						 */
						if(!cstate->binary)
							truncateEol(&cstate->line_buf, cstate->eol_type);
						
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
					 * get the data type of this attribute. If it's an
					 * array type use anyarray, or else just use as is.
					 * (base types for domains are handled in cdbhash.c).
					 */
					if(attr[h_attnum - 1]->attndims > 0)
						h_oid = ANYARRAYOID;
					else
						h_oid = attr[h_attnum - 1]->atttypid;
					
					if (nulls[h_attnum - 1] != 'n')
						cdbhash(cdbHash, h_key, h_oid);
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
				 * Send data row to all databases for this segment. If not in 
				 * BINARY format, also send the original row number with the data.
				 */
				if(cstate->binary)
				{
					/* send the data row the way it is */
					cdbCopySendData(cdbCopy, 
									target_seg, 
									cstate->line_buf.data, 
									cstate->line_buf.len, true);									
				}
				else
				{
					/* modify the data to look like: "<lineno>^<target_seg><space>^<data>" */
					/* The space will be filled in by cdbCopySendData with P or M (primary or mirror) */
					appendStringInfo(&line_buf_with_lineno, "%d%c%d %c%s", 
									 original_lineno_for_qe, 
									 ORIGINAL_LINENO_DELIM,
									 target_seg, 
									 ORIGINAL_LINENO_DELIM,
									 cstate->line_buf.data);
					
					/* send modified data */
					cdbCopySendData(cdbCopy, 
									target_seg, 
									line_buf_with_lineno.data, 
									line_buf_with_lineno.len, false);
					
					RESET_LINEBUF_WITH_LINENO;
					
				}


				cstate->processed++;
				
				if (cdbCopy->io_errors)
				{
					appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
					no_more_data = true;
					break;
				}

				RESET_LINEBUF;

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
	total_rejeted_from_qes = cdbCopyEnd(cdbCopy);

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
	 * report all accumulated errors back to the client. We get here if an error
	 * happened in all-or-nothing error handling mode or if reject limit was
	 * reached in single-row error handling mode.
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
	 * switch back away from COPY error context callback. don't want line 
	 * error information anymore 
	 */
	error_context_stack = errcontext.previous;

	/*
	 * If we got here it means that either all the data was loaded or some rows
	 * were rejected in SREH mode. In other words - nothing caused an abort.
	 * We now want to report the actual number of rows loaded and rejected.
	 * If any rows were rejected from the QE COPY processes subtract this number 
	 * from the number of rows that were successfully processed on the QD COPY 
	 * so that we can report the correct number. 
	 */
	if(cstate->cdbsreh)
	{
		int total_rejected = 0;
		
		total_rejected = cstate->cdbsreh->rejectcount + total_rejeted_from_qes;
		cstate->processed -= total_rejected;
		
		/* emit a NOTICE with number of rejected rows */
		ReportSrehResults(cstate->cdbsreh, total_rejected);
		
		/* See if we want to DROP error table when destroying cdbsreh */
		if(cstate->cdbsreh->errtbl)
			SetErrorTableVerdict(cstate->cdbsreh, total_rejected);
	}


	/*
	 * Done, clean up
	 */
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
	pfree(cdbcopy_cmd.data);
	pfree(cdbcopy_err.data);
	pfree(line_buf_with_lineno.data);
	pfree(cdbCopy);
	
	if(policy)
		pfree(policy);
	
	ExecDropSingleTupleTableSlot(slot);
	
	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
}

/*
 * Copy FROM file to relation.
 */
static void
CopyFrom(CopyState cstate)
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
	int			attnum;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	bool		isnull;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
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
	bool		cur_row_rejected = false;
	int			original_lineno_for_qe = 0; /* keep compiler happy (var referenced by macro) */
	CdbCopy    *cdbCopy = NULL; /* never used... for compiling COPY_HANDLE_ERROR */
	int			target_seg = -1; /* not used in utility mode */

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

	/* Set up a tuple slot too */
	slot = MakeSingleTupleTableSlot(tupDesc);

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
	if(Gp_role == GP_ROLE_EXECUTE)
		cstate->err_loc_type = ROWNUM_EMBEDDED; /* get original row num from QD COPY */
	else
		cstate->err_loc_type = ROWNUM_ORIGINAL; /* we can count rows by ourselves */

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
					
					PG_TRY();
					{
						/* Actually read the line into memory here */
						cstate->line_done = cstate->csv_mode ?
						CopyReadLineCSV(cstate, bytesread) :
						CopyReadLineText(cstate, bytesread);
					}
					PG_CATCH();
					{
						/* got here? encoding conversion/check error occured */
						COPY_HANDLE_ERROR;
					}
					PG_END_TRY();					
					
					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QE_GOTO_NEXT_ROW;
					}
					
					if(!cstate->line_done)
					{
						/*
						 * We did not finish reading a complete date line
						 *
						 * If eof is not yet reached, we skip att parsing
						 * and read more data. But if eof _was_ reached it means
						 * that the original last data line is defective and
						 * we want to catch that error later on.
						 */
						if (!cstate->fe_eof || cstate->end_marker)
							break;
					}
					else
					{
						/* 
						 * SREH special case - encoding error
						 *
						 * If data is already in server encoding then it means
						 * that it was converted successfully by QD COPY. Then
						 * we are done. However, if it's not in a valid server
						 * encoding this must mean that the QD COPY failed to
						 * convert to server encoding and sent us the raw text
						 * from the original data stream. In order to 
						 * reconstruct the error for logging it we attempt to
						 * do the conversion here again, and then catch the 
						 * error and log it.
						 */
						if(Gp_role == GP_ROLE_EXECUTE && cstate->errMode == SREH_LOG)
						{
							Assert(!cstate->need_transcoding);
							
							if(!pg_verify_mbstr(GetDatabaseEncoding(), 
												cstate->line_buf.data, 
												cstate->line_buf.len, 
												true /* don't ereport */))
							{
								PG_TRY();
								{
									/* invalid server encoding. reconstruct QD error */
									cstate->line_buf_converted = false;
									(void) pg_client_to_server(cstate->line_buf.data,
																	   cstate->line_buf.len);
									
									/* previous statement MUST report an error! */
									Assert(false);
								}
								PG_CATCH();
								{
									/* got here? encoding conversion/check error occured */
									COPY_HANDLE_ERROR;
								}
								PG_END_TRY();					
								
								if(cur_row_rejected)
								{
									IF_REJECT_LIMIT_REACHED_ABORT;
									QE_GOTO_NEXT_ROW;
								}
								
							}
						}
					}

					if (file_has_oids)
					{
						char	   *oid_string;

						/* can't be in CSV mode here */
						oid_string = CopyReadOidAttr(cstate, &isnull);

						if (isnull)
						{
							/* got here? null in OID column error */
							
							if(cstate->errMode == ALL_OR_NOTHING)
							{
								/* report error and abort */
								cdbCopyEnd(cdbCopy);
								
								ereport(ERROR,
										(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
										 errmsg("null OID in COPY data.")));									
							}
							else
							{
								/* SREH */
								cstate->cdbsreh->rejectcount++;
								cur_row_rejected = true;
							}					
							
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
								/* got here? oid column conversion failed */
								COPY_HANDLE_ERROR;
							}
							PG_END_TRY();
							
							if (loaded_oid == InvalidOid)
							{
								if(cstate->errMode == ALL_OR_NOTHING)
									ereport(ERROR,
											(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
											 errmsg("invalid OID in COPY data.")));
								else /* SREH */
								{
									cstate->cdbsreh->rejectcount++;
									cur_row_rejected = true;
								}
							}
							cstate->cur_attname = NULL;
						}
						
						if(cur_row_rejected)
						{
							IF_REJECT_LIMIT_REACHED_ABORT;
							QE_GOTO_NEXT_ROW;							
						}
						
					}

					PG_TRY();
					{
						if (cstate->csv_mode)
							CopyReadAttributesCSV(cstate, nulls, attr_offsets, num_phys_attrs, attr);
						else
							CopyReadAttributesText(cstate, nulls, attr_offsets, num_phys_attrs, attr);
					}
					PG_CATCH();
					{
						/* Got here? missing or extra column error occured */
						COPY_HANDLE_ERROR;
					}
					PG_END_TRY();
					
					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QE_GOTO_NEXT_ROW;						
					}
					
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

						if (cstate->csv_mode && isnull && cstate->force_notnull_flags[m])
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
								values[m] = InputFunctionCall(&in_functions[m],
															  string,
															  typioparams[m],
															  attr[m]->atttypmod);
							}
							PG_CATCH();
							{
								COPY_HANDLE_ERROR;
							}
							PG_END_TRY();
							
							if(cur_row_rejected)
								break; /* get out of loop, don't parse next attributes */
														
							nulls[m] = ' ';
							cstate->cur_attname = NULL;
							
						}
					}
					
					/* if this row had an error in SREH mode, on to the next.. */
					if(cur_row_rejected)
					{
						IF_REJECT_LIMIT_REACHED_ABORT;
						QE_GOTO_NEXT_ROW;	
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
					PG_TRY();
					{
						values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
														 &isnull, NULL);
					}
					PG_CATCH();
					{
						/* TODO: is an error really possible here? */
						COPY_HANDLE_ERROR;
					}
					PG_END_TRY();
					
					if (!isnull)
						nulls[defmap[i]] = ' ';
				}

				if(cur_row_rejected)
				{
					IF_REJECT_LIMIT_REACHED_ABORT;
					QE_GOTO_NEXT_ROW;							
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
					if (heap_enableHeapInsertDesc)
					{
						if (cstate->heapInsertDesc == NULL)
						{
							cstate->heapInsertDesc = heap_begininsert(cstate->rel, 
																	  GetCurrentCommandId(), true, true);
						}
						heap_insertnext(cstate->heapInsertDesc, tuple);
					}
					else 
					{
						simple_heap_insert(cstate->rel, tuple);
					}

					if (resultRelInfo->ri_NumIndices > 0)
						ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

					/* AFTER ROW INSERT Triggers */
					ExecARInsertTriggers(estate, resultRelInfo, tuple);
					
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

				RESET_LINEBUF;
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
	ExecASInsertTriggers(estate, resultRelInfo);

	/*
	 * Handle queued AFTER triggers
	 */
	AfterTriggerEndQuery(estate);

	/*
	 * If SREH and in executor mode send the number of rejected
	 * rows to the client (QD COPY).
	 */
	if(cstate->errMode != ALL_OR_NOTHING && Gp_role == GP_ROLE_EXECUTE)
		SendNumRowsRejected(cstate->cdbsreh->rejectcount);


	pfree(attr_offsets);

	pfree(values);
	pfree(nulls);

	pfree(in_functions);
	pfree(typioparams);
	pfree(defmap);
	pfree(defexprs);

	ExecDropSingleTupleTableSlot(slot);
	
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
bool
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
			appendBinaryStringInfo(&cstate->line_buf, cstate->raw_buf, bytesread);
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
			linesize = bytesread - (cstate->begloc - cstate->raw_buf);
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
	 * Done reading a complete data line. Set input row number for error report 
	 * purposes and convert it to server encoding if transcoding is needed.
	 */
	if (cstate->line_done)
	{
		/*
		 * Increment line count by 1 if we have access to all the original
		 * data rows and can count them reliably (ROWNUM_ORIGINAL). However 
		 * if we have ROWNUM_EMBEDDED the original row number for this row
		 * was sent to us with the data (courtesy of the data distributor), so 
		 * get that number instead.
		 */
		if(cstate->err_loc_type == ROWNUM_ORIGINAL)
			cstate->cur_lineno++; 
		else if(cstate->err_loc_type == ROWNUM_EMBEDDED)
			cstate->cur_lineno = CopyExtractLineNum(cstate);
		else
			Assert(false); /* byte offset not yet supported */
		
		if (cstate->need_transcoding)
		{
			cvt = (char *) pg_client_to_server(cstate->line_buf.data,
											   cstate->line_buf.len);
			if (cvt != cstate->line_buf.data)
			{
				/* transfer converted data back to line_buf */
				RESET_LINEBUF;
				appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
				pfree(cvt);
			}
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
bool
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
			appendBinaryStringInfo(&cstate->line_buf, cstate->raw_buf, bytesread);
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
			linesize = bytesread - (cstate->begloc - cstate->raw_buf);
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
	 * Done reading a complete data line. Set input row number for error report 
	 * purposes and convert it to server encoding if transcoding is needed.
	 */
	if (cstate->line_done)
	{
		/*
		 * Increment line count by 1 if we have access to all the original
		 * data rows and can count them reliably (ROWNUM_ORIGINAL). However 
		 * if we have ROWNUM_EMBEDDED the original row number for this row
		 * was sent to us with the data (courtesy of the data distributor), so 
		 * get that number instead.
		 */
		if(cstate->err_loc_type == ROWNUM_ORIGINAL)
			cstate->cur_lineno++; 
		else if(cstate->err_loc_type == ROWNUM_EMBEDDED)
			cstate->cur_lineno = CopyExtractLineNum(cstate);
		else
			Assert(false); /* byte offset not yet supported */
		
		if (cstate->need_transcoding)
		{
			cvt = (char *) pg_client_to_server(cstate->line_buf.data,
											   cstate->line_buf.len);
			if (cvt != cstate->line_buf.data)
			{
				/* transfer converted data back to line_buf */
				RESET_LINEBUF;
				appendBinaryStringInfo(&cstate->line_buf, cvt, strlen(cvt));
			}
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
DetectLineEnd(CopyState cstate, size_t bytesread  __attribute__((unused)))
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
 * if we have WITH OIDS or if we ran CopyExtractLineNum this cursor is 
 * already shifted and is not in the beginning of line buf anymore.
 *
 * attribute_buf.cursor -- an offset from the beginning of the
 * attribute buffer that indicates where the current attribute begins.
 */
void
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

	/* init variables for attribute scan */
	RESET_ATTRBUF;

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
				if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)
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
CopyReadBinaryAttribute(CopyState cstate, int column_no __attribute__((unused)), FmgrInfo *flinfo, Oid typioparam,
						int32 typmod, bool *isnull)
{
	int32		fld_size;
	uint32		buf;
	Datum		result;

	if(!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		buf = htonl((uint32) fld_size); /* change to network byte order */
		appendBinaryStringInfo(&cstate->line_buf, (char *) &buf, sizeof(buf));
	}

	if (fld_size == -1)
	{
		*isnull = true;
		return ReceiveFunctionCall(flinfo, NULL, typioparam, typmod);
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));


	/* reset attribute_buf to empty, and load raw data in it */
	RESET_ATTRBUF;
	
	enlargeStringInfo(&cstate->attribute_buf, fld_size);

	CopyGetData(cstate, cstate->attribute_buf.data, fld_size);
	if (cstate->fe_eof)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	/* load raw data into line buf for dispatching later */
	if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)
		appendBinaryStringInfo(&cstate->line_buf, cstate->attribute_buf.data, fld_size);

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* Call the column type's binary input converter */
	result = ReceiveFunctionCall(flinfo, &cstate->attribute_buf,
								 typioparam, typmod);

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
void
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
	int			m = 0;			/* attribute index being parsed */
	ListCell   *cur;			/* cursor to attribute list used for this COPY */
	int			attribute;

	/* init variables for attribute scan */
	RESET_ATTRBUF;
	
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
			input_len = end_cursor - start_cursor;

			if (cstate->eol_type == EOL_CRNL)
			{
				/* ignore the leftover CR */
				input_len--;
				cstate->attribute_buf.data[cstate->attribute_buf.cursor - 1] = '\0';
			}

			/* check whether raw input matched null marker */
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
			if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)
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
	RESET_ATTRBUF;
	
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
#define DUMPSOFAR() \
do { \
	if (ptr > start) \
		CopySendData(cstate, start, ptr - start); \
} while (0)

/*
 * Send text representation of one attribute, with conversion and escaping
 */
static void
CopyAttributeOutText(CopyState cstate, char *string)
{
	char	   *ptr;
	char	   *start;
	char		c;
	char		delimc = cstate->delim[0];
	
	if (cstate->need_transcoding)
		ptr = pg_server_to_client(string, strlen(string));
	else
		ptr = string;
	
	/*
	 * We have to grovel through the string searching for control characters
	 * and instances of the delimiter character.  In most cases, though, these
	 * are infrequent.  To avoid overhead from calling CopySendData once per
	 * character, we dump out all characters between replaceable characters
	 * in a single call.  The loop invariant is that the data from "start"
	 * to "ptr" can be sent literally, but hasn't yet been.
	 */
	start = ptr;
	while ((c = *ptr) != '\0')
	{
		switch (c)
		{
			case '\b':
				DUMPSOFAR();
				CopySendString(cstate, "\\b");
				start = ++ptr;
				break;
			case '\f':
				DUMPSOFAR();
				CopySendString(cstate, "\\f");
				start = ++ptr;
				break;
			case '\n':
				DUMPSOFAR();
				CopySendString(cstate, "\\n");
				start = ++ptr;
				break;
			case '\r':
				DUMPSOFAR();
				CopySendString(cstate, "\\r");
				start = ++ptr;
				break;
			case '\t':
				DUMPSOFAR();
				CopySendString(cstate, "\\t");
				start = ++ptr;
				break;
			case '\v':
				DUMPSOFAR();
				CopySendString(cstate, "\\v");
				start = ++ptr;
				break;
			case '\\':
				DUMPSOFAR();
				CopySendString(cstate, "\\\\");
				start = ++ptr;
				break;
			default:
				if (c == delimc)
				{
					DUMPSOFAR();
					CopySendChar(cstate, '\\');
					start = ptr; /* we include char in next run */
				}
				
				/*
				 * We can skip pg_encoding_mblen() overhead when encoding is
				 * safe, because in valid backend encodings, extra bytes of a
				 * multibyte character never look like ASCII.
				 */
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					ptr += pg_encoding_mblen(cstate->client_encoding, ptr);
				else
					ptr++;
				break;
		}
	}
	
	DUMPSOFAR();
}

/*
 * Send text representation of one attribute, with conversion and
 * CSV-style escaping
 */
static void
CopyAttributeOutCSV(CopyState cstate, char *string,
					bool use_quote, bool single_attr)
{
	char	   *ptr;
	char	   *start;
	char		c;
	char		delimc = cstate->delim[0];
	char		quotec = cstate->quote[0];
	char		escapec = cstate->escape[0];
	
	/* force quoting if it matches null_print (before conversion!) */
	if (!use_quote && strcmp(string, cstate->null_print) == 0)
		use_quote = true;
	
	if (cstate->need_transcoding)
		ptr = pg_server_to_client(string, strlen(string));
	else
		ptr = string;
	
	/*
	 * Make a preliminary pass to discover if it needs quoting
	 */
	if (!use_quote)
	{
		/*
		 *	Because '\.' can be a data value, quote it if it appears
		 *	alone on a line so it is not interpreted as the end-of-data
		 *	marker.
		 */
		if (single_attr && strcmp(ptr, "\\.") == 0)
 			use_quote = true;
 		else
 		{
			char	   *tptr = ptr;
			
			while ((c = *tptr) != '\0')
			{
				if (c == delimc || c == quotec || c == '\n' || c == '\r')
				{
					use_quote = true;
					break;
				}
				if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
					tptr += pg_encoding_mblen(cstate->client_encoding, tptr);
				else
					tptr++;
			}
		}
	}
	
	if (use_quote)
	{
		CopySendChar(cstate, quotec);
		
		/*
		 * We adopt the same optimization strategy as in CopyAttributeOutText
		 */
		start = ptr;
		while ((c = *ptr) != '\0')
		{
			if (c == quotec || c == escapec)
			{
				DUMPSOFAR();
				CopySendChar(cstate, escapec);
				start = ptr;	/* we include char in next run */
			}
			if (IS_HIGHBIT_SET(c) && cstate->encoding_embeds_ascii)
				ptr += pg_encoding_mblen(cstate->client_encoding, ptr);
			else
				ptr++;
		}
		DUMPSOFAR();
		
		CopySendChar(cstate, quotec);
	}
	else
	{
		/* If it doesn't need quoting, we can just dump it as-is */
		CopySendString(cstate, ptr);
	}
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 *
 * rel can be NULL ... it's only used for error reports.
 */
List *
CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;
	
	if (attnamelist == NIL)
	{
		/* Generate default column list */
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
			int			i;
			
			/* Lookup column name */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				if (tupDesc->attrs[i]->attisdropped)
					continue;
				if (namestrcmp(&(tupDesc->attrs[i]->attname), name) == 0)
				{
					attnum = tupDesc->attrs[i]->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" of relation \"%s\" does not exist",
									name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
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
 * CopyExtractLineNum - extract embedded row number from data.
 *
 * If data is being parsed in execute mode the parser (QE) doesn't
 * know the original line number (in the original file) of the current 
 * row. Therefore the QD sends this information along with the data.
 * The same goes for external table scans with gpfdist. gpfdist will
 * send embedded line numbers with data.
 *
 * in:
 *    line_buf: <original_num>^<data for this row>
 *    lineno: ?
 *
 * out:
 *    line_buf: <data for this row>
 *    lineno: <original_num>
 */
static
int CopyExtractLineNum(CopyState cstate)
{
	char *lineno_delim = NULL;
	
	/* 
	 * Line_buf may have already skipped an OID column if WITH OIDS defined,
	 * so we need to start from cursor not always from beginning of linebuf.
	 */
	char *line_start = cstate->line_buf.data + cstate->line_buf.cursor;
	int  lineno_str_len = 0;
	int  lineno = 0;
		
	lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
	Insist(lineno_delim && (lineno_delim != line_start));		
	lineno_str_len = lineno_delim - line_start + 1;
	
	/* change the lineno delim to null '\0' to look at the lineno */
	*lineno_delim = '\0';
	lineno = atoi(line_start);
	
	/* restore the line_buf byte */
	*lineno_delim = ORIGINAL_LINENO_DELIM;

	/* reposition line buf cursor to see real data (skip lineno) */
	cstate->line_buf.cursor += lineno_str_len;

	/* skip target_seg */
	line_start = cstate->line_buf.data + cstate->line_buf.cursor;
	lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
	Insist(lineno_delim && (lineno_delim != line_start));		
	lineno_str_len = lineno_delim - line_start + 1;

	/* reposition line buf cursor to see real data (skip target_seg) */
	cstate->line_buf.cursor += lineno_str_len;

			
	return lineno;
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

	if (cstate->encoding_embeds_ascii && !cstate->line_buf_converted)
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

	if (cstate->encoding_embeds_ascii && !cstate->line_buf_converted)
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

/* remove end of line chars from end of a buffer */
void truncateEol(StringInfo buf, EolType eol_type)
{
	int one_back = buf->len - 1;
	int two_back = buf->len - 2;
	
	if(eol_type == EOL_CRNL)
	{
		if(buf->len < 2)
			return;
		
		if(buf->data[two_back] == '\r' &&
		   buf->data[one_back] == '\n')
		{
			buf->data[two_back] = '\0';
			buf->data[one_back] = '\0';
			buf->len -= 2;			
		}
	}
	else
	{
		if(buf->len < 1)
			return;
		
		if(buf->data[one_back] == '\r' ||
		   buf->data[one_back] == '\n')
		{
			buf->data[one_back] = '\0';
			buf->len--;			
		}
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

/*
 * Escape any single quotes or backslashes in given string (from initdb.c)
 */
static char *
escape_quotes(const char *src)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = palloc(len * 2 + 1);
	
	for (i = 0, j = 0; i < len; i++)
	{
		if ((src[i]) == '\'' || (src[i]) == '\\')
			result[j++] = src[i];
		result[j++] = src[i];
	}
	result[j] = '\0';
	return result;
}

/*
 * copy_dest_startup --- executor startup
 */
static void
copy_dest_startup(DestReceiver *self __attribute__((unused)), int operation __attribute__((unused)), TupleDesc typeinfo __attribute__((unused)))
{
	/* no-op */
}

/*
 * copy_dest_receive --- receive one tuple
 */
static void
copy_dest_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_copy    *myState = (DR_copy *) self;
	CopyState	cstate = myState->cstate;
	
	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);
	
	/* And send the data */
	CopyOneRowTo(cstate, InvalidOid, slot->tts_values, slot->tts_isnull);
}

/*
 * copy_dest_shutdown --- executor end
 */
static void
copy_dest_shutdown(DestReceiver *self __attribute__((unused)))
{
	/* no-op */
}

/*
 * copy_dest_destroy --- release DestReceiver object
 */
static void
copy_dest_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * CreateCopyDestReceiver -- create a suitable DestReceiver object
 */
DestReceiver *
CreateCopyDestReceiver(void)
{
	DR_copy    *self = (DR_copy *) palloc(sizeof(DR_copy));
	
	self->pub.receiveSlot = copy_dest_receive;
	self->pub.rStartup = copy_dest_startup;
	self->pub.rShutdown = copy_dest_shutdown;
	self->pub.rDestroy = copy_dest_destroy;
	self->pub.mydest = DestCopyOut;
	
	self->cstate = NULL;		/* will be set later */
	
	return (DestReceiver *) self;
}



uint64
DoCopyDispatchAgent(const CopyStmt *stmt)
{		
	CopyState	cstate;
	bool		is_from = stmt->is_from;


	List	   *force_quote = NIL;
	List	   *force_notnull = NIL;
	ListCell   *option;
	//TupleDesc   tupDesc;
	//int         num_phys_attrs;
	uint64      processed;
	CdbCopy    *cdbCopy;

	/* Allocate workspace and zero all fields */
	cstate = (CopyStateData *) palloc0(sizeof(CopyStateData));

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "binary") == 0)
		{
			cstate->binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			cstate->oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			cstate->delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			cstate->null_print = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "csv") == 0)
		{
			cstate->csv_mode = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			cstate->header_line = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			cstate->quote = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			cstate->escape = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			force_quote = (List *) defel->arg;
		}
		else if (strcmp(defel->defname, "force_notnull") == 0)
		{
			force_notnull = (List *) defel->arg;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}


	/* Set defaults */
	
	cstate->err_loc_type = ROWNUM_ORIGINAL; 
		
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

	cstate->cdbsreh = NULL; /* default - no SREH */
	cstate->errMode = ALL_OR_NOTHING; /* default */
	
	/* Set defaults */
	
	cstate->err_loc_type = ROWNUM_ORIGINAL; 
		
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

#if 0
	/*
	 * Error handling setup
	 */
	if(stmt->sreh)
	{		
		/* Single row error handling requested */
		SingleRowErrorDesc *sreh;
		sreh = (SingleRowErrorDesc *)stmt->sreh;
		
		if (!is_from)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY single row error handling only available using COPY FROM")));
		
		if (sreh->errtable)
		{
			cstate->errMode = SREH_LOG;
		}
		else
		{
			cstate->errMode = SREH_IGNORE;
			if (sreh->is_keep)
				ereport(ERROR,
						(errcode(ERRCODE_GP_FEATURE_NOT_SUPPORTED),
						errmsg("KEEP may only be specified with a LOG INTO errortable clause")));
			
		}
		
		if (!sreh->is_limit_in_rows)
			ereport(ERROR,
					(errcode(ERRCODE_GP_FEATURE_NOT_YET),
					 errmsg("Segment reject limit in percentage is not yet supported")));			
		
	
		cstate->cdbsreh = makeCdbSreh(sreh->is_keep, 
									  sreh->reusing_existing_errtable,
									  sreh->rejectlimit, 
									  sreh->is_limit_in_rows, 
									  sreh->errtable,
									  stmt->filename, 
									  stmt->relation->relname);
	}
	else
	{
		/* No single row error handling requested. Use "all or nothing" */
		cstate->cdbsreh = NULL; /* default - no SREH */
		cstate->errMode = ALL_OR_NOTHING; /* default */
	}
#endif 

	
	elog(LOG,"DoCopyDispatchAgent starting");

	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&cstate->attribute_buf);
	initStringInfo(&cstate->line_buf);
	cstate->processed = 0;

	/*
	 * Set up encoding conversion info.  
	 */
	cstate->client_encoding = GetDatabaseEncoding();
	cstate->need_transcoding = false;
	cstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(cstate->client_encoding);
	cstate->line_buf_converted = true;

	cstate->copy_dest = COPY_NEW_FE;	
	cstate->filename = NULL; /* QE COPY always uses STDIN */
	cstate->copy_file = NULL;
	cstate->fe_msgbuf = NULL;
	cstate->fe_eof = false;
	cstate->eol_type = EOL_UNKNOWN;
	cstate->cur_relname = NULL;
	cstate->cur_lineno = 0;
	cstate->cur_attname = NULL;
	cstate->err_loc_type = ROWNUM_ORIGINAL;
	cstate->in_quote = false;
	cstate->last_was_esc = false;


	/*
	 * some greenplum db specific vars
	 */
	cstate->is_copy_in = (is_from ? true : false);
	if (is_from)
	{
		cstate->error_on_executor = false;
		initStringInfo(&(cstate->executor_err_context));		
	}

	cdbCopy = makeCdbCopy(is_from);

	if (is_from)				/* copy from file to database */
	{			

		bool no_more_data = false;
		bool		cur_row_rejected = false;
		int original_lineno_for_qe = -1;
		int target_seg = -1;
		int total_rejeted_from_qes = 0;

	    elog(DEBUG1,"Copy is from file to database");
	  
		ReceiveCopyBegin(cstate);
		
		clear_relsize_cache();
		
		cstate->err_loc_type = ROWNUM_EMBEDDED;
 
		/* Set up data buffer to hold a chunk of data */
		MemSet(cstate->raw_buf, ' ', RAW_BUF_SIZE * sizeof(char));
		cstate->raw_buf[RAW_BUF_SIZE] = '\0';

		no_more_data = false;		/* no more input data to read from file or FE */
		cstate->line_done = true;
		cstate->raw_buf_done = false;
		RESET_LINEBUF;

		do
		{
			size_t		bytesread = 0;

			/* read a chunk of data into the buffer */
			if (!cstate->binary)
			{
				PG_TRY();
				{
					bytesread = CopyGetData(cstate, cstate->raw_buf, RAW_BUF_SIZE);	
					elog(DEBUG1,"CopyGetData, bytesread=%ld",(long)bytesread);
				}
				PG_CATCH();
				{
					/* 
					 * If we are here, we got some kind of communication error
					 * with the client or a bad protocol message. clean up and 
					 * re-throw error. Note that we don't handle this error in
					 * any special way in SREH mode as it's not a data error.
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
				
				while (!cstate->raw_buf_done)
				{


					if (QueryCancelPending)
					{
						/* quit processing loop */
						no_more_data = true;
						break;
					}

						/* Get the line number of the first line of this data row */
						//original_lineno_for_qe = cstate->cur_lineno + 1;
						

						PG_TRY();
						{
							/* Actually read the line into memory here */
							cstate->line_done = cstate->csv_mode ?
								CopyReadLineCSV(cstate, bytesread) :
								CopyReadLineText(cstate, bytesread);
						}
						PG_CATCH();
						{
							/* got here? encoding conversion/check error occured */
							COPY_HANDLE_ERROR;
						}
						PG_END_TRY();					
							
						/* CopyReadLine moves the cursor past the line no and target info */
						cstate->line_buf.cursor = 0;
						
						if(!cstate->line_done)
						{
							/*
							 * We did not finish reading a complete data line.
							 *
							 * If eof is not yet reached, we skip att parsing
							 * and read more data. But if eof _was_ reached it means
							 * that the original last data line is defective and
							 * we want to catch that error later on.
							 */
							if (!cstate->fe_eof || cstate->end_marker)
								break;
						}

						if (cstate->line_buf.len > 0)
						{
						char *lineno_delim = NULL;
						
						/* 
						 * Line_buf may have already skipped an OID column if WITH OIDS defined,
						 * so we need to start from cursor not always from beginning of linebuf.
						 */
						char *line_start = cstate->line_buf.data + cstate->line_buf.cursor;
						int  lineno_str_len = 0;
						bool primary = true;
												
						lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
						Insist(lineno_delim && (lineno_delim != line_start));		
						lineno_str_len = lineno_delim - line_start + 1;
						
						/* change the lineno delim to null '\0' to look at the lineno */
						*lineno_delim = '\0';
						original_lineno_for_qe = atoi(line_start);
						
						/* restore the line_buf byte */
						*lineno_delim = ORIGINAL_LINENO_DELIM;
						
						/* reposition line buf cursor to see real data (skip lineno) */
						cstate->line_buf.cursor += lineno_str_len;

						/* skip target_seg */
						line_start = cstate->line_buf.data + cstate->line_buf.cursor;
						lineno_delim = memchr(line_start, ORIGINAL_LINENO_DELIM, 32);
						Insist(lineno_delim && (lineno_delim != line_start));		
						lineno_str_len = lineno_delim - line_start + 1;

						/* change the lineno delim to null '\0' to look at the lineno */
						*lineno_delim = '\0';
						target_seg = atoi(line_start);
						Assert(*(lineno_delim-1) == 'P' || *(lineno_delim-1) == 'M');
						primary = *(lineno_delim-1) == 'P';
						
						/* restore the line_buf byte */
						*lineno_delim = ORIGINAL_LINENO_DELIM;


						/* reposition line buf cursor to see real data (skip target_seg) */
						cstate->line_buf.cursor += lineno_str_len;

								
						
						Assert(target_seg >= 0 && target_seg <= 10000);

						/* send unmodified data */
						cdbCopySendDataSingle(cdbCopy, 
										target_seg, 
										cstate->line_buf.data, 
										cstate->line_buf.len, primary);
							
							
						}
						
					


					cstate->processed++;
					

					RESET_LINEBUF;

				}					/* end while(!raw_buf_done) */
			}						/* end if (bytesread > 0 || !cstate->fe_eof) */
			else
				/* no bytes read, end of data */
			{
				no_more_data = true;
			}
		} while (!no_more_data);
		
		Assert(cstate->fe_eof);
		
		/*
		 * Done reading input data and sending it off to the segment
		 * databases Now we would like to end the copy command on
		 * all segment databases across the cluster.
		 */
		total_rejeted_from_qes = cdbCopyEnd(cdbCopy);
	
		
		//if(cstate->errMode != ALL_OR_NOTHING)
		if(stmt->sreh)
			SendNumRowsRejected(total_rejeted_from_qes);
		
	
	}
	else		/* copy from database to file */
	{
		cstate->fe_copy = true;

		/* We use fe_msgbuf as a per-row buffer regardless of copy_dest */
		cstate->fe_msgbuf = makeStringInfo();

		while(true)
		{
			
			bool done;
			bool copy_cancel = (QueryCancelPending ? true : false);
			
			/* get a chunk of data rows from the QE's and relay to QD */
			done = cdbCopyRelayData(cdbCopy, copy_cancel, &cstate->processed);

			if(done)
			{
				 //if(cdbCopy->remote_data_err || cdbCopy->local_data_err || cdbCopy->io_errors)
				 //	appendBinaryStringInfo(&cdbcopy_err, cdbCopy->err_msg.data, cdbCopy->err_msg.len);
	
				break;
			}

		}
		
		/* we can throw the error now if QueryCancelPending was set previously */
		CHECK_FOR_INTERRUPTS();
	
		SendCopyEnd(cstate);
	}

	
	
	if(cstate->cdbsreh)
		destroyCdbSreh(cstate->cdbsreh);
		
	/* Clean up storage (probably not really necessary) */
	processed = cstate->processed;
	
	pfree(cstate->attribute_buf.data);
	pfree(cstate->line_buf.data);
	pfree(cstate);

	return processed;
}
