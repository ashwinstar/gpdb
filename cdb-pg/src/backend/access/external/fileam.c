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
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *            $Id$
 *        $Change$
 *      $DateTime$
 *        $Author$
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fstream/gfile.h>

#include "access/fileam.h"
#include "access/heapam.h"
#include "access/valid.h"
#include "catalog/namespace.h"
#include "commands/copy.h"
#include "commands/dbcommands.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"  /*postmaster port*/
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/uri.h"
#include "utils/guc.h"
#include "utils/builtins.h"

#include "cdb/cdbsreh.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/url.h"

static HeapTuple filegettup(FileScanDesc scan, ScanDirection dir);
static void initParseState(CopyState pstate, Relation relation, List *fmtOpts, 
						   bool isText, bool isMasterOnly, char *uri, int rejectlimit, 
						   bool islimitinrows, Oid fmterrtbl, int encoding);
static void open_external_file(FileScanDesc scan);
static int	file_getdata(URL_FILE *extfile, CopyState pstate, int maxread);
static void file_scan_error_callback(void *arg);
void readHeaderLine(CopyState pstate);
static void close_external_file(FILE *dataSource, char *dataSourceName);


/* ----------------------------------------------------------------
*				   file_ interface functions
* ----------------------------------------------------------------
*/

#ifdef FILEDEBUGALL
#define FILEDEBUG_1 \
elog(DEBUG2, "file_getnext([%s],dir=%d) called", \
	 RelationGetRelationName(scan->fs_rd), (int) direction)
#define FILEDEBUG_2 \
elog(DEBUG2, "file_getnext returning EOS")
#define FILEDEBUG_3 \
elog(DEBUG2, "file_getnext returning tuple")
#else
#define FILEDEBUG_1
#define FILEDEBUG_2
#define FILEDEBUG_3
#endif   /* !defined(FILEDEBUGALL) */

/*
 * A global reference to our data source so it could be freed
 * from the outside during an error/abort (in AbortTransaction).
 */
static FILE *g_dataSource = NULL;
static char *g_dataSourceName = NULL;


/* ----------------
*		file_beginscan	- begin file scan
* ----------------
*/
FileScanDesc
file_beginscan(Relation relation, Index scanrelid, uint32 scancounter,
			   List *uriList, List *fmtOpts, bool isText, bool isMasterOnly,
			   int rejLimit, bool rejLimitInRows, Oid fmterrtbl, int encoding)
{
	FileScanDesc scan;
	TupleDesc	tupDesc = NULL;
	int			attnum;
	int			segindex = GpIdentity.segindex;
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
	scan->fs_scanrelid = scanrelid;
	scan->fs_scancounter = scancounter;
	scan->fs_noop = false;
	scan->fs_file = NULL;

	/*
	 * get the external URI assigned to us.
	 *
	 * The URI assigned for this segment is normally in the uriList list
	 * at the index of this segment id. However, if we are executing on
	 * MASTER ONLY the (one and only) entry which is destined for the master
	 * will be at the first entry of the uriList list.
	 */
	if (Gp_role == GP_ROLE_EXECUTE)
	{
		/* this is the normal path for most ext tables */
		Value *v;
		int idx = segindex;

		/* 
		 * Segindex may be -1, for the following case.
		 * A slice is executed on entry db, (for example, gp_configuration),
		 * then external table is executed on another slice.
		 * Entry db slice will still call ExecInitExternalScan (probably we
		 * should fix this?), then segindex = -1 will bomb out here.
		 */
		if (isMasterOnly && idx == -1)
			idx = 0;

		if (idx >= 0) 
		{
			v = (Value *)list_nth(uriList, idx); 

			if (v->type == T_Null)
				uri = NULL;
			else
				uri = (char *) strVal(v);	
		}
	}
	else if (Gp_role == GP_ROLE_DISPATCH && isMasterOnly)
	{
		/* this is a ON MASTER table. Only get uri if we are the master */
		if (segindex == -1)
		{
			Value *v = list_nth(uriList, 0);
			if (v->type == T_Null)
				uri = NULL;
			else
				uri = (char *) strVal(v);
		}
	}
	
	/*
	 * if a uri is assigned to us - get a reference to it. Some executors
	 * don't have a uri to scan (if # of uri's < # of primary segdbs). 
	 * in which case uri will be NULL. If that's the case for this 
	 * segdb set to no-op.
	 */
	if (uri)
	{
		/* set external source (uri) */
		scan->fs_uri = uri;
		
		/* NOTE: we delay actually opening the data source until file_getnext() */
	}
	else
	{
		/* segdb has no work to do. set to no-op */
		scan->fs_noop = true;
		scan->fs_uri = NULL;
	}
			
	tupDesc = RelationGetDescr(relation);
	scan->fs_tupDesc = tupDesc;
	scan->attr = tupDesc->attrs;
	scan->num_phys_attrs = tupDesc->natts;

	scan->values = (Datum *) palloc(scan->num_phys_attrs * sizeof(Datum));
	scan->nulls = (bool *) palloc(scan->num_phys_attrs * sizeof(bool));

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
	initParseState(scan->fs_pstate, relation, fmtOpts, isText, isMasterOnly,
				   scan->fs_uri, rejLimit, rejLimitInRows, fmterrtbl, encoding);

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
file_rescan(FileScanDesc scan)
{
	
	/* seek to beginning of data source so we can start over */
	url_rewind((URL_FILE*)scan->fs_file);

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

	if (scan->values)
	{
		pfree(scan->values);
		scan->values = NULL;
	}
	if (scan->nulls)
	{
		pfree(scan->nulls);
		scan->nulls = NULL;
	}
	if (scan->in_functions)
	{
		pfree(scan->in_functions);
		scan->in_functions = NULL;
	}
	if (scan->typioparams)
	{
		pfree(scan->typioparams);
		scan->typioparams = NULL;
	}

    /*
     * delete the row context
     */
    MemoryContextDelete(scan->fs_pstate->rowcontext);
    
	/*
	 * if SREH was active:
	 * 1) QEs: send a libpq message to QD with num of rows rejected in this segment
	 * 2) Free SREH resources (includes closeing the error table if used).
	 */
	if (scan->fs_pstate->errMode != ALL_OR_NOTHING)
	{
		if (Gp_role == GP_ROLE_EXECUTE)
			SendNumRowsRejected(scan->fs_pstate->cdbsreh->rejectcount);
		
		destroyCdbSreh(scan->fs_pstate->cdbsreh);
	}	

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
	if (!scan->fs_noop && scan->fs_file)
	{
		close_external_file(scan->fs_file, scan->fs_uri);
		scan->fs_file = NULL;
	}

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
	
	if (scan->fs_noop)
		return NULL;
	
	/* 
	 * open the external source (local file or http).
	 *
	 * NOTE: file_beginscan() seems like the natural place for this call. However,
	 * in queries with more than one gang each gang will initialized all the nodes
	 * of the plan (but actually executed only the nodes in it's local slice)
	 * This means that file_beginscan() (and file_endscan() too) will get called 
	 * more than needed and we'll end up opening too many http connections when 
	 * they are not expected (see MPP-1261). Therefore we instead do it here on the 
	 * first time around only.
	 */
	if (!scan->fs_file)
		open_external_file(scan);

	/* Note: no locking manipulations needed */
	FILEDEBUG_1;

	tuple = filegettup(scan, direction);

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

/* ==========================================================================
 * The follwing macros aid in major refactoring of data processing code (in
 * filegettup() ). We use macros because in some cases the code must be in
 * line in order to work (for example elog_dismiss() in PG_CATCH) while in 
 * other cases we'd like to inline the code for performance reasons.
 *
 * NOTE that an almost identical set of macros exists in copy.c for the COPY
 * command. If you make changes here you may want to consider taking a look.
 * ==========================================================================
 */

#define EXT_RESET_LINEBUF \
pstate->line_buf.len = 0; \
pstate->line_buf.data[0] = '\0'; \
pstate->line_buf.cursor = 0;

/*
 * A data error happened. This code block will always be inside a PG_CATCH()
 * block right when a higher stack level produced an error. We handle the error
 * by checking which error mode is set (SREH or all-or-nothing) and do the right
 * thing accordingly. Note that we MUST have this code in a macro (as opposed 
 * to a function) as elog_dismiss() has to be inlined with PG_CATCH in order to 
 * access local error state variables.
 */
#define FILEAM_HANDLE_ERROR \
if (pstate->errMode == ALL_OR_NOTHING) \
{ \
	/* re-throw error and abort */ \
	PG_RE_THROW(); \
} \
else \
{ \
	/* SREH - release error state */ \
\
	ErrorData	*edata; \
	MemoryContext oldcontext;\
	bool	errmsg_is_a_copy = false; \
\
	/* SREH must only handle data errors. all other errors must not be caught */\
	if(ERRCODE_TO_CATEGORY(elog_geterrcode()) != ERRCODE_DATA_EXCEPTION)\
	{\
		PG_RE_THROW(); \
	}\
\
	/* save a copy of the error info */ \
	oldcontext = MemoryContextSwitchTo(pstate->cdbsreh->badrowcontext);\
	edata = CopyErrorData();\
 	MemoryContextSwitchTo(oldcontext);\
\
	if (!elog_dismiss(DEBUG5)) \
		PG_RE_THROW(); /* <-- hope to never get here! */ \
\
	truncateEol(&pstate->line_buf, pstate->eol_type); \
	pstate->cdbsreh->rawdata = pstate->line_buf.data; \
	pstate->cdbsreh->is_server_enc = pstate->line_buf_converted; \
	pstate->cdbsreh->linenumber = pstate->cur_lineno; \
	pstate->cdbsreh->processed = ++pstate->processed; \
 	pstate->cdbsreh->consec_csv_err = pstate->num_consec_csv_err; \
\
	/* set the error message. Use original msg and add column name if availble */ \
	if (pstate->cur_attname)\
	{\
		pstate->cdbsreh->errmsg = (char *) palloc((strlen(edata->message) + \
												   strlen(pstate->cur_attname) + \
												   10 + 1) * sizeof(char)); \
		errmsg_is_a_copy = true; \
		sprintf(pstate->cdbsreh->errmsg, "%s, column %s", \
				edata->message, \
				pstate->cur_attname); \
	}\
	else\
	{\
		pstate->cdbsreh->errmsg = edata->message; \
	}\
\
	HandleSingleRowError(pstate->cdbsreh); \
\
	if (errmsg_is_a_copy && !IsRejectLimitReached(pstate->cdbsreh)) \
		pfree(pstate->cdbsreh->errmsg); \
}

/*
 * if in SREH mode and data error occured it was already handled in 
 * FILEAM_HANDLE_ERROR. Therefore, skip to the next row before attempting
 * to do any further processing on this one. 
 */
#define FILEAM_IF_REJECT_LIMIT_REACHED_ABORT \
if (IsRejectLimitReached(pstate->cdbsreh)) \
{ \
	char *rejectmsg_normal = "Segment reject limit reached. Aborting operation. Last error was:";\
	char *rejectmsg_allbad = "All 1000 first rows in this segment were rejected. Aborting operation regardless of REJECT LIMIT value. Last error was:";\
 	char *rejectmsg_csv_unparsable = "Input includes invalid CSV data that corrupts the ability to parse data rows. This usually means several unescaped embedded QUOTE characters. Data is not parsable.Last error was:";\
	char *finalmsg;\
\
 	if (FIRST_1000_BAD(pstate->cdbsreh))\
 	{\
 		/* the special "first 1000 are bad" case */\
 		finalmsg = (char *) palloc((strlen(pstate->cdbsreh->errmsg) + \
 									strlen(rejectmsg_allbad) + 12 + 1)\
 								   * sizeof(char)); \
 		sprintf(finalmsg, "%s %s", rejectmsg_allbad, pstate->cdbsreh->errmsg);\
 	}\
 	else if (CSV_IS_UNPARSABLE(pstate->cdbsreh))\
  	{\
 		/* the special "csv un-parsable" case */\
  		finalmsg = (char *) palloc((strlen(pstate->cdbsreh->errmsg) + \
 									strlen(rejectmsg_csv_unparsable) + 12 + 1)\
  								   * sizeof(char)); \
 		sprintf(finalmsg, "%s %s", rejectmsg_csv_unparsable, pstate->cdbsreh->errmsg);\
  	}\
  	else\
  	{\
 		/* the normal case */\
  		finalmsg = (char *) palloc((strlen(pstate->cdbsreh->errmsg) + \
 									strlen(rejectmsg_normal) + 12 + 1)\
  								   * sizeof(char)); \
 		sprintf(finalmsg, "%s %s", rejectmsg_normal, pstate->cdbsreh->errmsg);\
  	}\
\
	ereport(ERROR, \
			(errcode(ERRCODE_T_R_GP_REJECT_LIMIT_REACHED), \
			(errmsg(finalmsg) \
			))); \
}

/*
 * parse_next_line
 * 
 * Given a buffer of data, extract the next data line from it and parse it
 * to attributes according to the data format specifications.
 * 
 * Returns:
 * 	LINE_OK		    - line parsed successfully.			
 *	LINE_ERROR		- line was mal-formatted. error caught and handled.
 *	NEED_MORE_DATA  - line not parsed all the way through. need more data.
 *	END_MARKER      - saw line end marker. skip attr parsing, we're done.
 */
static DataLineStatus parse_next_line(FileScanDesc scan)
{
	CopyState pstate = scan->fs_pstate;
	MemoryContext oldctxt = CurrentMemoryContext;
	MemoryContext err_ctxt = oldctxt;

	DataLineStatus ret_mode = LINE_OK;

	ListCell   *cur;
	
	/* Initialize all values for row to NULL */
	MemSet(scan->values, 0, scan->num_phys_attrs * sizeof(Datum));
	MemSet(scan->nulls, true, scan->num_phys_attrs * sizeof(bool));
	MemSet(pstate->attr_offsets, 0, scan->num_phys_attrs * sizeof(int));

	PG_TRY(); 
	{
		/* Get a line */
		pstate->line_done = pstate->csv_mode ?
			CopyReadLineCSV(pstate, pstate->bytesread) :
			CopyReadLineText(pstate, pstate->bytesread);

		/* Did not get a complete and valid data line? */
		if(!pstate->line_done)
		{
			/*
			 * If eof is not yet reached, we skip att parsing and read more 
			 * data. But if eof _was_ reached it means that this data line is 
			 * defective and let the attribute parser find the error (we leave
			 * ret_mode intact).
			 */
			if (!pstate->fe_eof)
				ret_mode = NEED_MORE_DATA;
			
			/* found end marker "\." set ret_mode to skip attribute parsing */
			if (pstate->end_marker)
				ret_mode = END_MARKER;
		}

		if(ret_mode == LINE_OK)
		{
			if(pstate->csv_mode)
				CopyReadAttributesCSV(pstate, scan->nulls, pstate->attr_offsets,
						scan->num_phys_attrs, scan->attr);
			else
				CopyReadAttributesText(pstate, scan->nulls, pstate->attr_offsets,
						scan->num_phys_attrs, scan->attr);

			err_ctxt = pstate->rowcontext;
			MemoryContextSwitchTo(err_ctxt);

			foreach(cur, pstate->attnumlist)
			{
				int 	attnum = lfirst_int(cur);
				int 	m = attnum - 1;
				char   *string;
				bool	isnull;

				string = pstate->attribute_buf.data + pstate->attr_offsets[m];

				if(!scan->nulls[m])
					isnull = false;
				else
					isnull = true;

				/* check FORCE NOT NULL for this column */
				if (pstate->csv_mode && isnull && pstate->force_notnull_flags[m])
				{
					string = pstate->null_print;	/* set to NULL string */
					isnull = false;
				}

				if(!isnull)
				{
					pstate->cur_attname = NameStr(scan->attr[m]->attname);

					scan->values[m] = InputFunctionCall(&scan->in_functions[m],
														string,
														scan->typioparams[m],
														scan->attr[m]->atttypmod);
					scan->nulls[m] = false;
					pstate->cur_attname = NULL;
				}
			}
			EXT_RESET_LINEBUF;
		}
	} 
	PG_CATCH();
	{
		ret_mode = LINE_ERROR;
		MemoryContextSwitchTo(err_ctxt);
		FILEAM_HANDLE_ERROR;
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldctxt);

	if(ret_mode == LINE_ERROR)
	{
		FILEAM_IF_REJECT_LIMIT_REACHED_ABORT;
		EXT_RESET_LINEBUF;
	}

	return ret_mode; 
}

/* ----------------
*		filegettup  form another tuple from the data file.
*		This is the workhorse - make sure it's fast!
*
*		Initialize the scan if not already done.
*		Verify that we are scanning forward only.
*
* ----------------
*/
static HeapTuple
filegettup(FileScanDesc scan,
		   ScanDirection dir)
{
	HeapTuple	tuple = NULL;
	CopyState	pstate = scan->fs_pstate;

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
			
			/* on first time around just throw the header line away */
			if (pstate->header_line && pstate->bytesread > 0)
			{
				PG_TRY();
				{
					readHeaderLine(pstate);
				}
				PG_CATCH();
				{					
					/* 
					 * got here? encoding conversion error occured on the 
					 * header line (first row).
					 */
					if (pstate->errMode == ALL_OR_NOTHING)
					{
						PG_RE_THROW();											
					}
					else
					{
						/* SREH - release error state */
						if (!elog_dismiss(DEBUG5))
							PG_RE_THROW(); /* hope to never get here! */
						
						/* 
						 * note: we don't bother doing anything special here.
						 * we are never interested in logging a header line
						 * error. just continue the workflow.
						 */
					}			
				}
				PG_END_TRY();
				
				EXT_RESET_LINEBUF;
				pstate->header_line = false;
			}	
		}

		if (pstate->bytesread > 0 || !pstate->fe_eof)
		{
			/* while there is still data in our buffer */
			while (!pstate->raw_buf_done)
			{
				DataLineStatus ret_mode = parse_next_line(scan);

				if(ret_mode == LINE_OK)
				{
					/* convert to heap tuple */
					/* XXX This is bad code.  Planner should be able to
					 * decide whether we need heaptuple or memtuple upstream,
					 * so make the right decision here.
					 */
					tuple = heap_form_tuple(scan->fs_tupDesc, 
							scan->values, scan->nulls);

					pstate->processed++;
					MemoryContextReset(pstate->rowcontext);
					return tuple;
				}
				else if(ret_mode == LINE_ERROR)
				{
					/* error was handled in parse_next_line. move to the next */
					continue;					
				}
				else 
				{
					/* try to get more data if possible */
					Assert(ret_mode == NEED_MORE_DATA || ret_mode == END_MARKER);
					break;
				}
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
if (!actual)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. expected %s", desired)));\
}\
if (pg_strcasecmp(actual, desired) != 0)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. expected %s, got %s", desired, actual)));\
}

#define FILEAM_GET_CHAR(val, clause) \
if (!val)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error for %s clause value", clause)));\
}\
if (strlen(val) == 3)\
{\
	val[2] = '\0';\
	val++;\
}\
else if (strlen(val) > 3)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. %s value %s too long", clause, val)));\
}\
else if (strlen(val) == 2)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. %s value %s is not a single character", clause, val)));\
}\
else if (strlen(val) == 1)\
{\
	/* we must have a ' '. consume the second quote and set value to 'space' */\
	val = strtok(NULL, " ");\
	if (strcmp(val,"\'") != 0)\
		ereport(ERROR,\
				(errcode(ERRCODE_SYNTAX_ERROR),\
				 errmsg("internal parse error in %s value - expected single quote", clause)));\
	*val = ' ';\
}\

#define FILEAM_GET_STRING(val, clause) \
len = strlen(val);\
if (!val)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error for %s clause value", clause)));\
}\
if (len < 2)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error. invalid length for %s value", clause)));\
}\
else if (len == 2)\
{\
	val = "";\
}\
else\
{\
	val[len - 1] = '\0';\
	val++;\
}

#define FILEAM_GET_LIST(val, clause) \
if (!val)\
{\
	ereport(ERROR,\
			(errcode(ERRCODE_SYNTAX_ERROR),\
			 errmsg("internal parse error for %s clause value", clause)));\
}\



/*
 * Initialize the data parsing state.
 *
 * This includes format descriptions (delimiter, quote...), format type
 * (text, csv), encoding converstion information, etc...
 */
static void
initParseState(CopyState pstate, Relation relation, List *fmtOpts, bool isText, 
			   bool isMasterOnly, char *uri, int rejectlimit, 
			   bool islimitinrows, Oid fmterrtbl, int encoding)
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
	pstate->err_loc_type = ROWNUM_ORIGINAL;
	pstate->cur_attname = NULL;
	pstate->raw_buf_done = true;	/* true so we will read data in first run */
	pstate->line_done = true;
	pstate->no_more_data = false;
	pstate->bytesread = 0;
	pstate->csv_mode = !isText;
	pstate->header_line = false;
	pstate->fill_missing = false;
	pstate->line_buf_converted = false;
	pstate->raw_buf_index = 0;
	pstate->processed = 0;
	pstate->filename = uri;

	/*
	 * Error handling setup
	 */
	if (rejectlimit == -1) 
	{
		/* Default error handling - "all-or-nothing" */
		pstate->cdbsreh = NULL; /* no SREH */
		pstate->errMode = ALL_OR_NOTHING; 
	}
	else
	{
		RangeVar *errtbl_rv;
		bool	  curTxnCreatedErrtbl = false; /* errtbl created in current txn? */

		/* select the SREH mode */
		if (fmterrtbl == InvalidOid)
		{
			/* no error table */
			pstate->errMode = SREH_IGNORE; 
			errtbl_rv = NULL;
		}
		else
		{
			/* with error table */
			
			Relation rel;
			
			pstate->errMode = SREH_LOG; 
			
			/*
			 * we want to be sure that this error table is alive (wasn't dropped).
			 * we must do this check until we have proper dependency recorded.
			 */
			LockRelationOid(fmterrtbl, AccessShareLock);
			rel = RelationIdGetRelation(fmterrtbl);
			
			if (rel == NULL)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
								errmsg("The specified error table for this "
									   "external table doesn't appear to "
									   "exist in the database. It may have "
									   "been dropped."),
								errhint("Refresh your external table definition.")));
			
			RelationDecrementReferenceCount(rel); /* must do this after RelationIdGetRelation() */
			
			
			errtbl_rv = makeRangeVar(get_namespace_name(get_rel_namespace(fmterrtbl)),
									 get_rel_name(fmterrtbl));
			
			if (rel->rd_createSubid != InvalidSubTransactionId)
				curTxnCreatedErrtbl = true;
				
#ifdef nothing			
			/* bummer... a non writer gang can't write to an error table! */
			if (!Gp_is_writer && Gp_role != GP_ROLE_DISPATCH)
				ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_YET),
								errmsg("Error tables can not yet be used in queries "
									   "with more than one table in the FROM clause. "
									   "You can use the same query with single row "
									   "error handling by redefining the external " 
									   "table without an error table ")));
#endif
		}

		/* Single row error handling */
		pstate->cdbsreh = makeCdbSreh(true, /* don't DROP errtable at end of execution */
									  !curTxnCreatedErrtbl, /* really only relevant to COPY though */
									  rejectlimit, 
									  islimitinrows, 
									  errtbl_rv, 
									  pstate->filename,
									  (char *)pstate->cur_relname);

		/* if necessary warn the user of the risk of table getting dropped */
		if (Gp_role == GP_ROLE_DISPATCH && curTxnCreatedErrtbl)
			emitSameTxnWarning();

 		pstate->num_consec_csv_err = 0;
	}

	initStringInfo(&pstate->attribute_buf);
	initStringInfo(&pstate->line_buf);

	/* Set up data buffer to hold a chunk of data */
	MemSet(pstate->raw_buf, ' ', RAW_BUF_SIZE * sizeof(char));
	pstate->raw_buf[RAW_BUF_SIZE] = '\0';

	/*
	 * Set up encoding conversion info.  Even if the client and server
	 * encodings are the same, we must apply pg_client_to_server() to validate
	 * data in multibyte encodings.
	 *
	 * Each external table specifies the encoding of its external data. We will
	 * therefore set a client encoding and client-to-server conversion procedure
	 * in here and these will be used in the data conversion routines (in copy.c
	 * CopyReadLineXXX()).
	 */
	Insist(PG_VALID_ENCODING(encoding));
	pstate->client_encoding = encoding;
	setEncodingConversionProc(pstate, encoding);

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
	 * header
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
	}

	/*
	 * Possible remaining opts:
	 * - HEADER 
	 * - FILL MISSING FIELDS
	 * - FORCE NOT NULL <list>
	 * 
	 * note: this parsing logic is kinda ugly but must be done this way
	 * until we switch from strtok's to a more high level mechanism.
	 */
	while(true)
	{
		clause = strtok(NULL, " ");

		if (clause)
		{
			if (pg_strcasecmp(clause, "header") == 0)
			{
				pstate->header_line = true;
			}
			else if(pg_strcasecmp(clause, "fill") == 0)
			{
				pstate->fill_missing = true;

				/* consume the rest of the related keywords */
				clause = strtok(NULL, " ");
				FILEAM_VERIFY_CLAUSE(clause, "missing");
				clause = strtok(NULL, " ");
				FILEAM_VERIFY_CLAUSE(clause, "fields");
			}
			else if(pg_strcasecmp(clause, "force") == 0)
			{
				List *temp = NIL;
				ListCell *l;
				
				Assert(pstate->csv_mode);
				
				/* consume the rest of the related keywords */
				clause = strtok(NULL, " ");
				FILEAM_VERIFY_CLAUSE(clause, "not");
				clause = strtok(NULL, " ");
				FILEAM_VERIFY_CLAUSE(clause, "null");
				
				/* get the fnn attr list */
				value = strtok(NULL, " ");

				if(!SplitIdentifierString(value, ',', &temp))
				{
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid list syntax for \"force not null\"")));
				}
				
				/* wrap each string of temp inside a Value node and copy to 
				 * force_notnull so that later CopyGetAttnums could use it 
				 */ 
				foreach(l, temp)
				{
					Value *val = makeString((char *)lfirst(l));
					pstate->force_notnull = lappend(pstate->force_notnull, val);
				}
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("internal parse error at end of format string. got \"%s\"", clause)));
			}
		}
		else
		{
			break;
		}
	}

	 // elog(NOTICE, "delim %s null %s escape %s quote %s %s %s", pstate->delim,
	 // pstate->null_print, pstate->escape, pstate->quote, (pstate->header_line
	 // ? "header" : ""), (pstate->fill_missing ? "fill missing fields" : ""));

	if (pstate->csv_mode)
	{
		if (!pstate->quote)
			pstate->quote = "\"";
	}

	tupDesc = RelationGetDescr(relation);
	pstate->attr_offsets = (int *) palloc(tupDesc->natts * sizeof(int));

	/* Generate or convert list of attributes to process */
	pstate->attnumlist = CopyGetAttnums(tupDesc, relation, NIL);

	/* Convert FORCE NOT NULL name list to per-column flags, check validity */
	pstate->force_notnull_flags = (bool *) palloc0(tupDesc->natts * sizeof(bool));
	if (pstate->force_notnull)
	{
		List	   *attnums;
		ListCell   *cur;
		
		attnums = CopyGetAttnums(tupDesc, relation, pstate->force_notnull);
		
		foreach(cur, attnums)
		{
			int			attnum = lfirst_int(cur);			
			pstate->force_notnull_flags[attnum - 1] = true;
		}
	}

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
	extvar_t 	extvar;
	time_t 		now = time(0);
	struct 		tm* tm = localtime(&now);
	char        *result = (char *) palloc(7);        /* sign, 5 digits, '\0' */
	int 		response_code;
	const char	*response_string;
	
	/* set up extvar */

	memset(&extvar, 0, sizeof(extvar));
	sprintf(extvar.GP_CSVOPT,
			"m%dx%dq%dh%d",
			scan->fs_pstate->csv_mode ? 1 : 0,
			scan->fs_pstate->escape ? 255&*scan->fs_pstate->escape : 0,
			scan->fs_pstate->quote ? 255&*scan->fs_pstate->quote : 0,
			scan->fs_pstate->header_line ? 1 : 0);
	
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		pg_itoa(qdPostmasterPort, result);
		extvar.GP_MASTER_PORT = result;
		extvar.GP_MASTER_HOST = qdHostname;
	}
	else
	{
		CdbComponentDatabases *cdb_component_dbs = getCdbComponentDatabases();
		CdbComponentDatabaseInfo *qdinfo = &cdb_component_dbs->entry_db_info[0];
		
		pg_itoa(qdinfo->port, result);
		extvar.GP_MASTER_PORT = result;
		
		if (qdinfo->hostip != NULL)
			extvar.GP_MASTER_HOST = pstrdup(qdinfo->hostip);
		else
			extvar.GP_MASTER_HOST = pstrdup(qdinfo->hostname);		
	}
	
	if (MyProcPort)
		extvar.GP_USER = MyProcPort->user_name;
	else
		extvar.GP_USER = "";
	
	extvar.GP_DATABASE = get_database_name(MyDatabaseId);
	extvar.GP_SEG_PG_CONF = ConfigFileName;   /*location of the segments pg_conf file*/
	extvar.GP_SEG_DATADIR = data_directory;   /*location of the segments datadirectory*/
   	sprintf(extvar.GP_DATE, "%04d%02d%02d",
			1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday);
	sprintf(extvar.GP_TIME, "%02d%02d%02d", 
			tm->tm_hour, tm->tm_min, tm->tm_sec);
	if (! getDistributedTransactionIdentifier(extvar.GP_XID)) 
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("cannot get distributed transaction identifier while %s", 
						scan->fs_uri)));

	sprintf(extvar.GP_CID, "%x", QEDtxContextInfo.curcid);
	sprintf(extvar.GP_SN, "%x", scan->fs_scancounter);
	sprintf(extvar.GP_SEGMENT_ID, "%d", Gp_segment);
    sprintf(extvar.GP_SEG_PORT, "%d", PostPortNumber);
    sprintf(extvar.GP_SESSION_ID, "%d", gp_session_id);
 	sprintf(extvar.GP_SEGMENT_COUNT, "%d", GpIdentity.numsegments);
    
    /*
     * Open the url with url_open().
     */
    scan->fs_file = (FILE*) url_fopen(scan->fs_uri, PG_BINARY_R, &extvar,scan->fs_pstate,&response_code,&response_string);

	if (scan->fs_file == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open \"%s\" for reading: %d %s",
						scan->fs_uri,response_code,response_string)));
	}
	else 
	{
		/*
		 * Get a global reference to the file pointer, so we can free it from
		 * AbortTransaction in the case of an error or abort. We don't use it 
		 * for anything else.
		 */
		g_dataSource = scan->fs_file;
		g_dataSourceName = scan->fs_uri;
	}
}

/*
 * close the external file.
 */
static void
close_external_file(FILE *dataSource, char *dataSourceName)
{
	if (dataSource)
	{
		if (url_fclose((URL_FILE*) dataSource))
 		{
 			g_dataSource = dataSource = NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
 					 errmsg("error closing file or executing program \"%s\": %m",
							dataSourceName)));
		}
	}
	
	g_dataSource = dataSource = NULL;
}

/*
 * get a chunk of data from the external data file.
 */
static int
file_getdata(URL_FILE *extfile, CopyState pstate, int maxread)
{
	int			bytesread = 0;

 	/* CK: this code is very delicate. The caller expects this:
 	    - if url_fread returns something, and the EOF is reached, it
 		  this call must return with both the content and the fe_eof
 		  flag set.
 		- failing to do so will result in skipping the last line.
 	*/
 
	bytesread = url_fread((void *) pstate->raw_buf, 1, maxread, extfile,pstate);
 
 	if (url_feof(extfile)) 
 	{
  		pstate->fe_eof = true;
 	}
 
 	if (bytesread <= 0) 
 	{
 		if (url_ferror(extfile))
 			ereport(ERROR,
 					(errcode_for_file_access(),
 					 errmsg("could not read from external file: %m")));
 	}

	return bytesread;
}

/*
 * error context callback for external table scan
 */
static void
file_scan_error_callback(void *arg)
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
	char buffer[20];
				
	initStringInfo(&copy_of_line_buf);
	appendStringInfoString(&copy_of_line_buf, cstate->line_buf.data);
				
	if (cstate->cur_attname)
	{
		/* error is relevant to a particular column */
		limit_printout_length(&cstate->attribute_buf);

		errcontext("External table %s, line %s of %s, column %s",
				   cstate->cur_relname,linenumber_atoi(buffer,cstate->cur_lineno), cstate->filename,
				   cstate->cur_attname);
	}
	else
	{

		/* error is relevant to a particular line */
		if (cstate->line_buf_converted || !cstate->need_transcoding)
		{
			truncateEol(&copy_of_line_buf, cstate->eol_type);
			limit_printout_length(&copy_of_line_buf);

			errcontext("External table %s, line %s of %s: \"%s\"",
					   cstate->cur_relname, linenumber_atoi(buffer,cstate->cur_lineno),
					   cstate->filename, copy_of_line_buf.data);
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
			errcontext("External table %s, line %s of file %s",
					   cstate->cur_relname, linenumber_atoi(buffer,cstate->cur_lineno), cstate->filename);
		}
	}

	/* error context will possibly free it but just in case */
	pfree(copy_of_line_buf.data);
	
}

/*
 * Read the data file header line and ignore it. 
 *
 * This function should be called only once for each data file
 * and only if HEADER is specified in the SQL command. 
 */
void readHeaderLine(CopyState pstate)
{
	pstate->line_done = pstate->csv_mode ?
		CopyReadLineCSV(pstate, pstate->bytesread) :
		CopyReadLineText(pstate, pstate->bytesread);		
}

/*
 * Free external resources on Abort.
 */
void AtAbort_ExtTables(void)
{
	close_external_file(g_dataSource, g_dataSourceName);
	g_dataSource = NULL;
}
void
gfile_printf_then_putc_newline(const char*format,...)
{
	char*a;
	va_list va;
	int i;

	va_start(va,format);
	i = vsnprintf(0,0,format,va);
	va_end(va);

	if (i < 0)
		elog(NOTICE,"gfile_printf_then_putc_newline vsnprintf failed.");
	else if (!(a=palloc(i+1)))
		elog(NOTICE,"gfile_printf_then_putc_newline palloc failed.");
	else
	{
		va_start(va,format);
		vsnprintf(a,i+1,format,va);
		va_end(va);
		elog(NOTICE,"%s",a);
		pfree(a);
	}
}
void*
gfile_malloc(size_t size)
{
	return palloc(size);
}
void
gfile_free(void*a)
{
	pfree(a);
}
char*
linenumber_atoi(char buffer[20],int64_t linenumber)
{
	if (linenumber < 0)
		return "N/A";

	snprintf(buffer,20,INT64_FORMAT,linenumber);

	return buffer;
}
