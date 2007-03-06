/*-------------------------------------------------------------------------
*
* fileam.c
*	  file access method routines
*       
* This access layer mimics the heap access API with respect to how it
* communicates with its respective scan node (external scan node) but
* instead of accessing the heap pages, it actually "scans" data by 
* reading it from a local flat file or a remote data source.
*
* The actual data access, whether local or remote, is done with the
* curl c library ('libcurl') which uses a 'c-file like' API but behind
* the scenes actually does all the work of parsing the URI and communicating
* with the target. In this case if the URI uses the file protocol (file://)
* curl will try to open the specified file locally. If the URI uses the
* http protocol (http://) then curl will reach out to that address and
* get the data from there.
*
* As data is being read it gets parsed with the COPY command parsing rules,
* as if it is data meant for COPY. Therefore, currently, with the lack of
* single row error handling the first error will raise an error and the
* query will terminate.
*
* Copyright (c) 2007, greenplum inc
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include <sys/stat.h>

#include "access/fileam.h"
#include "access/heapam.h"
#include "access/valid.h"
#include "pgstat.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "commands/copy.h"

#include "cdb/cdbvars.h"

#include "url.i"

static HeapTuple filegettup(FileScanDesc scan, ScanDirection dir, int nkeys, ScanKey key);
static void initParseState(CopyState pstate, Relation relation, List *fmtOpts, bool isText, char *uri);
static void open_external_file(FileScanDesc scan);
static void close_external_file(FileScanDesc scan);
static int	file_getdata(URL_FILE *extfile, CopyState pstate, int maxread);
static void file_scan_error_callback(void *arg);


/* ----------------------------------------------------------------
*				   file_ interface functions
* ----------------------------------------------------------------
*/

#ifdef FILEDEBUGALL
#define FILEDEBUG_1 \
elog(DEBUG2, "file_getnext([%s,nkeys=%d],dir=%d) called", \
	 RelationGetRelationName(scan->fs_rd), scan->fs_nkeys, (int) direction)
#define FILEDEBUG_2 \
elog(DEBUG2, "file_getnext returning EOS")
#define FILEDEBUG_3 \
elog(DEBUG2, "file_getnext returning tuple")
#else
#define FILEDEBUG_1
#define FILEDEBUG_2
#define FILEDEBUG_3
#endif   /* !defined(FILEDEBUGALL) */


/* ----------------
*		file_beginscan	- begin file scan
* ----------------
*/
FileScanDesc
file_beginscan(Relation relation, List *uriList, List *fmtOpts,
			   bool isText, int nkeys, ScanKey key)
{
	FileScanDesc scan;
	TupleDesc	tupDesc = NULL;
	int			attnum;
	char		*uri = NULL;

	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * allocate and initialize scan descriptor
	 */
	scan = (FileScanDesc) palloc(sizeof(FileScanDescData));

	scan->fs_inited = false;
	scan->fs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->fs_ctup.t_self);
	scan->fs_cbuf = InvalidBuffer;
	scan->fs_rd = relation;
	scan->fs_nkeys = nkeys;
	scan->fs_noop = false;

	/*
	 * get the external URI assigned to us. 
	 */
	if(Mpp_role == MPP_ROLE_EXECUTE)
		uri = (char *) strVal(list_nth(uriList, MppIdentity.segindex));
	
	/*
	 * if a uri is assigned to us - open it. Some executors
	 * don't have a uri to scan (if # of uri's < # of primary segdbs). 
	 * in which case uri will be NULL. If that's the case for this 
	 * segdb set to no-op.
	 */
	if(uri)
	{
		/* set external source (uri) */
		scan->fs_uri = (char *) palloc(strlen(uri) + 1);
		scan->fs_uri = uri;
		
		/* open the external file for read */
		open_external_file(scan);
	}
	else
	{
		/* set segdb has no work to do. set to no-op */
		scan->fs_noop = true;
		scan->fs_uri = NULL;
	}
			
	/* we only need to set this up once */
	scan->fs_ctup.t_tableOid = RelationGetRelid(relation);

	if (nkeys > 0)
		scan->fs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->fs_key = NULL;
	
	tupDesc = RelationGetDescr(relation);
	scan->fs_tupDesc = tupDesc;
	scan->attr = tupDesc->attrs;
	scan->num_phys_attrs = tupDesc->natts;

	scan->values = (Datum *) palloc(scan->num_phys_attrs * sizeof(Datum));
	scan->nulls = (char *) palloc(scan->num_phys_attrs * sizeof(char));

	/*
	 * Pick up the required catalog information for each attribute in the
	 * relation, including the input function and the element type (to pass
	 * to the input function).
	 */
	scan->in_functions = (FmgrInfo *) palloc(scan->num_phys_attrs * sizeof(FmgrInfo));
	scan->typioparams = (Oid *) palloc(scan->num_phys_attrs * sizeof(Oid));

	for (attnum = 1; attnum <= scan->num_phys_attrs; attnum++)
	{
		/* We don't need info for dropped attributes */
		if (scan->attr[attnum - 1]->attisdropped)
			continue;

		getTypeInputInfo(scan->attr[attnum - 1]->atttypid,
						 &scan->in_func_oid, &scan->typioparams[attnum - 1]);
		fmgr_info(scan->in_func_oid, &scan->in_functions[attnum - 1]);
	}

	/*
	 * Allocate and init our structure that keeps track of data parsing state
	 */
	scan->fs_pstate = (CopyStateData *) palloc0(sizeof(CopyStateData));
	initParseState(scan->fs_pstate, relation, fmtOpts, isText, scan->fs_uri);

	/* Set up callback to identify error line number */
	scan->errcontext.callback = file_scan_error_callback;
	scan->errcontext.arg = (void *) scan->fs_pstate;
	scan->errcontext.previous = error_context_stack;

	/* pgstat_initstats(&scan->rs_pgstat_info, relation); */

	return scan;
}


/* ----------------
*		file_rescan  - (re)start a scan of an external file
* ----------------
*/
void
file_rescan(FileScanDesc scan, ScanKey key)
{
	
	/* seek to beginning of file so we can start over */
	fseek(scan->fs_file, 0L, SEEK_SET);

	/* reset some parse state variables */
	scan->fs_pstate->fe_eof = false;
	scan->fs_pstate->cur_lineno = 0;
	scan->fs_pstate->cur_attname = NULL;
	scan->fs_pstate->raw_buf_done = true;		/* true so we will read data
												 * in first run */
	scan->fs_pstate->line_done = true;
	scan->fs_pstate->no_more_data = false;
	scan->fs_pstate->bytesread = 0;
}

/* ----------------
*		file_endscan - end a scan
* ----------------
*/
void
file_endscan(FileScanDesc scan)
{
	
	/*
	 * decrement relation reference count and free scan descriptor storage
	 */
	RelationDecrementReferenceCount(scan->fs_rd);

	if (scan->fs_key)
		pfree(scan->fs_key);
	if (scan->values)
		pfree(scan->values);
	if (scan->nulls)
		pfree(scan->nulls);
	if (scan->in_functions)
		pfree(scan->in_functions);
	if (scan->typioparams)
		pfree(scan->typioparams);

    /*
     * delete the row context
     */
    MemoryContextDelete(scan->fs_pstate->rowcontext);
    
	/*
	 * free parse state memory
	 */
	pfree(scan->fs_pstate->attribute_buf.data);
	pfree(scan->fs_pstate->line_buf.data);
	pfree(scan->fs_pstate->attr_offsets);
	pfree(scan->fs_pstate);
	
    /*
	 * clean up error context
	 */
	error_context_stack = scan->errcontext.previous;

	/*
	 * Close the external file
	 */
	if(!scan->fs_noop)
		close_external_file(scan);

	pfree(scan);
}


/* ----------------------------------------------------------------
*		file_getnext
*
*		Parse a data file and return its rows in heap tuple form
* ----------------------------------------------------------------
*/
HeapTuple
file_getnext(FileScanDesc scan, ScanDirection direction)
{
	HeapTuple	tuple;
	
	if(scan->fs_noop)
		return NULL;
	
	/* Note: no locking manipulations needed */
	FILEDEBUG_1;

	tuple = filegettup(scan, direction, scan->fs_nkeys, scan->fs_key);

	if (tuple == NULL)
	{
		FILEDEBUG_2;			/* file_getnext returning EOS */

		return NULL;
	}

	/*
	 * if we get here it means we have a new current scan tuple
	 */
	FILEDEBUG_3;				/* file_getnext returning tuple */

	/* pgstat_count_file_getnext(&scan->rs_pgstat_info); */

	return tuple;
}


/* ----------------
*		filegettup â form another tuple from the data file.
*		This is the workhorse - make sure it's fast!
*
*		Initialize the scan if not already done.
*		Verify that we are scanning forward only.
*
* ----------------
*/
static HeapTuple
filegettup(FileScanDesc scan,
		   ScanDirection dir,
		   int nkeys,
		   ScanKey key)
{
	HeapTuple	tuple = NULL;
	bool		valid;
	bool		isnull;
	ListCell   *cur;
	CopyState	pstate = scan->fs_pstate;
    MemoryContext oldcontext;
    

	Assert(ScanDirectionIsForward(dir));

	error_context_stack = &scan->errcontext;

	if (!scan->fs_inited)
	{
		/* more init stuff here... */
		scan->fs_inited = true;
	}
	else
	{
		/* continue from previously returned tuple */
		/* (set current state...) */
	}

	/* while didn't finish reading the entire file */
	while (!pstate->no_more_data)
	{
		/* need to fill our buffer with data? */
		if (pstate->raw_buf_done)
		{
		    pstate->bytesread = file_getdata((URL_FILE*)scan->fs_file, pstate, RAW_BUF_SIZE);
			pstate->begloc = pstate->raw_buf;
			pstate->raw_buf_done = false;
			pstate->raw_buf_index = 0;
		}

		if (pstate->bytesread > 0 || !pstate->fe_eof)
		{
			/* while there is still data in our buffer */
			while (!pstate->raw_buf_done)
			{
				CHECK_FOR_INTERRUPTS();

				/* Initialize all values for row to NULL */
				MemSet(scan->values, 0, scan->num_phys_attrs * sizeof(Datum));
				MemSet(scan->nulls, 'n', scan->num_phys_attrs * sizeof(char));

				MemSet(pstate->attr_offsets, 0, scan->num_phys_attrs * sizeof(int));

				/* get a data line */
				pstate->line_done = pstate->csv_mode ?
					CopyReadLineCSV(pstate, pstate->bytesread) :
					CopyReadLineText(pstate, pstate->bytesread);

				if (pstate->line_done)
					pstate->cur_lineno++;
				else if (!pstate->fe_eof || pstate->end_marker)
					break;

				/* parse attributes */
				if (pstate->csv_mode)
					CopyReadAttributesCSV(pstate, scan->nulls, pstate->attr_offsets, scan->num_phys_attrs, scan->attr);
				else
					CopyReadAttributesText(pstate, scan->nulls, pstate->attr_offsets, scan->num_phys_attrs, scan->attr);

                oldcontext = MemoryContextSwitchTo(pstate->rowcontext);

				/*
				 * Loop to read the user attributes on the line.
				 */
				foreach(cur, pstate->attnumlist)
				{
					int			attnum = lfirst_int(cur);
					int			m = attnum - 1;
					char	   *string;

					string = pstate->attribute_buf.data + pstate->attr_offsets[m];

					if (scan->nulls[m] == ' ')
						isnull = false;
					else
						isnull = true;

					/* we read an SQL NULL, no need to do anything */
					if (!isnull)
					{
						pstate->cur_attname = NameStr(scan->attr[m]->attname);
						scan->values[m] = InputFunctionCall(&scan->in_functions[m],
															string,
														scan->typioparams[m],
												   scan->attr[m]->atttypmod);
						scan->nulls[m] = ' ';
						pstate->cur_attname = NULL;
					}
				}

                MemoryContextSwitchTo(oldcontext);

				/* we can reset line buffer now. */
				pstate->line_buf.len = 0;
				pstate->line_buf.data[0] = '\0';
				pstate->line_buf.cursor = 0;

				/* convert to heap tuple */
				tuple = heap_formtuple(scan->fs_tupDesc, scan->values, scan->nulls);

                MemoryContextReset(pstate->rowcontext);

				/*
				 * We ignore anything that has to do with tuple visibility and
				 * snapshot. Each external tuple will always have one version.
				 */
				valid = true;

				if (key != NULL)
					HeapKeyTest(tuple, scan->fs_tupDesc,
								nkeys, key, valid);

				/* If this tuple qualified we are done for now */
				if (valid)
					return tuple;

				/* otherwise move to the next data row */
			}

		}
		else
		{
			pstate->no_more_data = true;
		}
	}

	/*
	 * if we got here we finished reading all the data.
	 */
	Assert(pstate->no_more_data);
	scan->fs_inited = false;

	return NULL;
}

/*
 * These macros are helpers for verifying and extracting format
 * clauses. They could eventually do assert checking instead of
 * reporting an error, since we expect to never get an error here.
 * However, leave the ereport initially for debugging purposes.
 */
#define FILEAM_VERIFY_CLAUSE(actual, desired) \
if(!actual)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. expected %s", desired)));\
}\
if(pg_strcasecmp(actual, desired) != 0)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. expected %s, got %s", desired, actual)));\
}

#define FILEAM_GET_CHAR(val, clause) \
if(!val)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error for %s clause value", clause)));\
}\
if(strlen(val) != 3)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. %s value %s too long", clause, val)));\
}\
val[2] = '\0';\
val++;

#define FILEAM_GET_STRING(val, clause) \
len = strlen(val);\
if(!val)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error for %s clause value", clause)));\
}\
if(len < 2)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. invalid length for %s value", clause)));\
}\
else if(len == 2)\
{\
	val = "";\
}\
else\
{\
	val[len - 1] = '\0';\
	val++;\
}


/*
 * Initialize the data parsing state.
 *
 * This includes format descriptions (delimiter, quote...), format type
 * (text, csv), encoding converstion information, etc...
 */
static void
initParseState(CopyState pstate, Relation relation, List *fmtOpts, bool isText, char *uri)
{
	TupleDesc	tupDesc = NULL;
	char	   *format_str = NULL;
	char	   *clause;
	char	   *value;
	int			len;

	pstate->fe_eof = false;
	pstate->eol_type = EOL_UNKNOWN;
	pstate->cur_relname = RelationGetRelationName(relation);
	pstate->cur_lineno = 0;
	pstate->cur_attname = NULL;
	pstate->raw_buf_done = true;	/* true so we will read data in first run */
	pstate->line_done = true;
	pstate->no_more_data = false;
	pstate->bytesread = 0;
	pstate->csv_mode = !isText;
	pstate->header_line = false;
	pstate->line_buf_converted = false;
	pstate->raw_buf_index = 0;
	pstate->processed = 0;
	pstate->filename = uri;

	initStringInfo(&pstate->attribute_buf);
	initStringInfo(&pstate->line_buf);

	/* Set up data buffer to hold a chunk of data */
	MemSet(pstate->raw_buf, ' ', RAW_BUF_SIZE * sizeof(char));
	pstate->raw_buf[RAW_BUF_SIZE] = '\0';

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to validate
	 * data in multibyte encodings.
	 */
	pstate->client_encoding = pg_get_client_encoding();
	pstate->need_transcoding = (pstate->client_encoding != GetDatabaseEncoding() ||
								pg_database_encoding_max_length() > 1);
	pstate->encoding_embeds_ascii = PG_ENCODING_IS_CLIENT_ONLY(pstate->client_encoding);

	/*
	 * Now parse the format options. They will always appear
	 * in the following order:
	 *
	 * delimiter
	 * null
	 * escape
	 * quote (if csv)
	 * header (if csv)
	 *
	 * NOTE: Eventually when we support user defined types
	 * the user will supply a function that will know how
	 * to parse these parameters. For now we can hardcode
	 * it for the 2 supported types - text and csv.
	 */
	format_str = pstrdup((char *) strVal(linitial(fmtOpts)));
	
	/*
	 * DELIMITER
	 */
	clause = strtok(format_str, " ");
	FILEAM_VERIFY_CLAUSE(clause, "delimiter");

	value = strtok(NULL, " ");
	FILEAM_GET_CHAR(value, "delimiter");

	pstate->delim = value;

	/*
	 * NULL
	 */
	clause = strtok(NULL, " ");
	FILEAM_VERIFY_CLAUSE(clause, "null");

	value = strtok(NULL, " ");
	FILEAM_GET_STRING(value, "null");

	pstate->null_print = value;
	pstate->null_print_len = strlen(pstate->null_print);


	/*
	 * ESCAPE
	 */
	clause = strtok(NULL, " ");
	FILEAM_VERIFY_CLAUSE(clause, "escape");

	value = strtok(NULL, " ");
	FILEAM_GET_STRING(value, "escape");

	pstate->escape = value;

	if (pg_strcasecmp(value, "off") == 0)
		pstate->escape_off = true;

	if (pstate->csv_mode)
	{
		/*
		 * QUOTE
		 */
		clause = strtok(NULL, " ");
		FILEAM_VERIFY_CLAUSE(clause, "quote");

		value = strtok(NULL, " ");
		FILEAM_GET_CHAR(value, "quote");

		pstate->quote = value;

		/*
		 * HEADER
		 */
		clause = strtok(NULL, " ");

		if (clause)
		{
			FILEAM_VERIFY_CLAUSE(clause, "header");
			pstate->header_line = true;
		}

	}

	/*
	 * elog(NOTICE, "delim %s null %s escape %s quote %s %s", pstate->delim,
	 * pstate->null_print, pstate->escape, pstate->quote, pstate->header_line
	 * ? "header" : "");
	 */

	if (pstate->csv_mode)
	{
		if (!pstate->quote)
			pstate->quote = "\"";
	}

	tupDesc = RelationGetDescr(relation);
	pstate->attr_offsets = (int *) palloc(tupDesc->natts * sizeof(int));

	/* Generate or convert list of attributes to process */
	pstate->attnumlist = CopyGetAttnums(tupDesc, relation, NIL);
    
    /*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input routines, and should be faster than retail pfree's
	 * anyway.
	 */
	pstate->rowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "ExternalScanMemCtxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
    

}

/*
 * open the external source for scanning.
 *
 * This could either be a local file or a remote source (via http).
 */
static void
open_external_file(FileScanDesc scan)
{
	scan->fs_file = (FILE*) url_fopen(scan->fs_uri, PG_BINARY_R);

	if (scan->fs_file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						scan->fs_uri)));

}

/*
 * close the external file.
 */
static void
close_external_file(FileScanDesc scan)
{
    if (url_fclose((URL_FILE*) scan->fs_file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						scan->fs_uri)));
}

/*
 * get a chunk of data from the external data file.
 */
static int
file_getdata(URL_FILE *extfile, CopyState pstate, int maxread)
{
	int			bytesread = 0;

	bytesread = url_fread((void *) pstate->raw_buf, 1, maxread, extfile);
	if (url_feof(extfile))
		pstate->fe_eof = true;
	if (bytesread == -1) 
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from external file: %m")));

	return bytesread;
}

/*
 * error context callback for external table scan
 */
static void
file_scan_error_callback(void *arg)
{

	CopyState	cstate = (CopyState) arg;

	if (cstate->cur_attname)
	{
		/* error is relevant to a particular column */
		limit_printout_length(&cstate->attribute_buf);

		errcontext("External table %s, line %d of %s, column %s",
				   cstate->cur_relname, cstate->cur_lineno, cstate->filename,
				   cstate->cur_attname);
	}
	else
	{
		int			eol_len = 1;
		int			eol_loc = 0;

		if (cstate->eol_type == EOL_CRNL)
			eol_len = 2;

		eol_loc = cstate->line_buf.len - eol_len;

		if (eol_loc < 0)		/* just to be sure */
			eol_loc = 0;

		/* remove LF from line before displaying it */
		cstate->line_buf.data[cstate->line_buf.len - eol_len] = '\0';

		/* error is relevant to a particular line */
		if (cstate->line_buf_converted || !cstate->need_transcoding)
		{
			limit_printout_length(&cstate->line_buf);

			errcontext("External table %s, line %d of %s: \"%s\"", cstate->cur_relname, cstate->cur_lineno, cstate->filename, cstate->line_buf.data);
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
			errcontext("External table %s, line %d of file %s", cstate->cur_relname, cstate->cur_lineno, cstate->filename);
		}
	}

}
