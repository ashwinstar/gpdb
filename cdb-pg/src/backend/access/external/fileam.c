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
#include "commands/dbcommands.h"
#include "catalog/namespace.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/uri.h"
#include "mb/pg_wchar.h"
#include "commands/copy.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "nodes/makefuncs.h"
#include "cdb/cdbvars.h"
#include "postmaster/postmaster.h"  /*postmaster port*/



typedef struct extvar_t extvar_t;
struct extvar_t
{
	char* GP_MASTER_HOST;
	char* GP_MASTER_PORT;
	char* GP_DATABASE;
	char* GP_SEG_PG_CONF;   /*location of the segments pg_conf file*/
	char* GP_SEG_DATADIR;   /*location of the segments datadirectory*/
	char GP_DATE[9];		/* YYYYMMDD */
	char GP_TIME[7];		/* HHMMSS */
	char GP_XID[TMGIDSIZE];		/* global transaction id */
	char GP_CID[10];		/* command id */
	char GP_SN[10];		/* scan number */
	char GP_SEGMENT_ID[6];  /*segments content id*/
	char GP_SEG_PORT[10];
};

#include "url.i"

static HeapTuple filegettup(FileScanDesc scan, ScanDirection dir);
static void initParseState(CopyState pstate, Relation relation, List *fmtOpts, 
						   bool isText, char *uri, int rejectlimit, 
						   bool islimitinrows, Oid fmterrtbl, int encoding);
static void open_external_file(FileScanDesc scan);
static int	file_getdata(URL_FILE *extfile, CopyState pstate, int maxread);
static void file_scan_error_callback(void *arg);
void readHeaderLine(CopyState pstate);


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
			   List *uriList, List *fmtOpts, bool isText, int rejLimit,
			   bool rejLimitInRows, Oid fmterrtbl, int encoding)
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
	scan->fs_scanrelid = scanrelid;
	scan->fs_scancounter = scancounter;
	scan->fs_noop = false;
	scan->fs_file = NULL;

	/*
	 * get the external URI assigned to us. 
	 */
	if(Gp_role == GP_ROLE_EXECUTE)
		uri = (char *) strVal(list_nth(uriList, GpIdentity.segindex));
	
	/*
	 * if a uri is assigned to us - get a reference to it. Some executors
	 * don't have a uri to scan (if # of uri's < # of primary segdbs). 
	 * in which case uri will be NULL. If that's the case for this 
	 * segdb set to no-op.
	 */
	if(uri)
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
			
	/* we only need to set this up once */
	scan->fs_ctup.t_tableOid = RelationGetRelid(relation);
	
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
	initParseState(scan->fs_pstate, relation, fmtOpts, isText, scan->fs_uri,
				   rejLimit, rejLimitInRows, fmterrtbl, encoding);

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
	 * if SREH was active:
	 * 1) QEs: send a libpq message to QD with num of rows rejected in this segment
	 * 2) Free SREH resources (includes closeing the error table if used).
	 */
	if(scan->fs_pstate->errMode != ALL_OR_NOTHING)
	{
		if(Gp_role == GP_ROLE_EXECUTE)
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
	if(!scan->fs_noop)
		close_external_file(scan->fs_file, scan->fs_uri);

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
if(pstate->errMode == ALL_OR_NOTHING) \
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
	cur_row_rejected = true; \
\
	/* save a copy of the error info */ \
	oldcontext = MemoryContextSwitchTo(pstate->cdbsreh->badrowcontext);\
	edata = CopyErrorData();\
 	MemoryContextSwitchTo(oldcontext);\
\
	if(!elog_dismiss(DEBUG5)) \
		PG_RE_THROW(); /* <-- hope to never get here! */ \
\
	truncateEol(&pstate->line_buf, pstate->eol_type); \
	pstate->cdbsreh->rawdata = pstate->line_buf.data; \
	pstate->cdbsreh->is_server_enc = pstate->line_buf_converted; \
	pstate->cdbsreh->linenumber = pstate->cur_lineno; \
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
	if(errmsg_is_a_copy && !IsRejectLimitReached(pstate->cdbsreh)) \
		pfree(pstate->cdbsreh->errmsg); \
}

/*
 * if in SREH mode and data error occured it was already handled in 
 * FILEAM_HANDLE_ERROR. Therefore, skip to the next row before attempting
 * to do any further processing on this one. 
 */
#define FILEAM_GOTO_NEXT_ROW \
EXT_RESET_LINEBUF; \
cur_row_rejected = false; /* reset for next run */ \
continue; /* move on to the next data line */

#define FILEAM_IF_REJECT_LIMIT_REACHED_ABORT \
if(IsRejectLimitReached(pstate->cdbsreh)) \
{ \
	char *rejectmsg = "Segment reject limit reached. Aborting operation. Last error was:";\
	char *finalmsg = (char *) palloc((strlen(pstate->cdbsreh->errmsg) + \
									  strlen(rejectmsg) + 12 + 1)\
									  * sizeof(char)); \
\
    sprintf(finalmsg, "%s %s", rejectmsg, pstate->cdbsreh->errmsg); \
\
	ereport(ERROR, \
			(errcode(ERRCODE_T_R_GP_REJECT_LIMIT_REACHED), \
			(errmsg(finalmsg) \
			))); \
}


/* ----------------
*		filegettup � form another tuple from the data file.
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
	bool		isnull;
	bool		cur_row_rejected = false;
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
			
			/* on first time around just throw the header line away */
			if(pstate->header_line && pstate->bytesread > 0)
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
					if(pstate->errMode == ALL_OR_NOTHING)
					{
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
				
				EXT_RESET_LINEBUF;
				pstate->header_line = false;
			}
				
		}

		if (pstate->bytesread > 0 || !pstate->fe_eof)
		{
			/* while there is still data in our buffer */
			while (!pstate->raw_buf_done)
			{
				
				/* Initialize all values for row to NULL */
				MemSet(scan->values, 0, scan->num_phys_attrs * sizeof(Datum));
				MemSet(scan->nulls, 'n', scan->num_phys_attrs * sizeof(char));

				MemSet(pstate->attr_offsets, 0, scan->num_phys_attrs * sizeof(int));

				PG_TRY();
				{
					/* get a data line */
					pstate->line_done = pstate->csv_mode ?
						CopyReadLineCSV(pstate, pstate->bytesread) :
						CopyReadLineText(pstate, pstate->bytesread);
				}
				PG_CATCH();
				{
					/* got here? encoding conversion/check error occured */
					FILEAM_HANDLE_ERROR;
				}
				PG_END_TRY();					

				if(cur_row_rejected)
				{
					FILEAM_IF_REJECT_LIMIT_REACHED_ABORT;
					FILEAM_GOTO_NEXT_ROW;
				}
				
				
				if(!pstate->line_done)
				{
					/*
					 * We did not finish reading a complete data line.
					 *
					 * If eof is not yet reached, we skip att parsing
					 * and read more data. But if eof _was_ reached it means
					 * that the original last data line is defective and
					 * we want to catch that error later on.
					 */
					if (!pstate->fe_eof || pstate->end_marker)
						break;
				}
				
				/* parse attributes */
				PG_TRY();
				{

					if (pstate->csv_mode)
						CopyReadAttributesCSV(pstate, scan->nulls, pstate->attr_offsets, scan->num_phys_attrs, scan->attr);
					else
						CopyReadAttributesText(pstate, scan->nulls, pstate->attr_offsets, scan->num_phys_attrs, scan->attr);
				}
				PG_CATCH();
				{
					/* Got here? missing or extra column error occured */
					FILEAM_HANDLE_ERROR;
				}
				PG_END_TRY();					
				
				if(cur_row_rejected)
				{
					FILEAM_IF_REJECT_LIMIT_REACHED_ABORT;
					FILEAM_GOTO_NEXT_ROW;
				}
				
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
						
						PG_TRY();
						{
							scan->values[m] = InputFunctionCall(&scan->in_functions[m],
																string,
															scan->typioparams[m],
													   scan->attr[m]->atttypmod);
						}
						PG_CATCH();
						{
							/* Got here? datum conversion error occured */
							FILEAM_HANDLE_ERROR;
						}
						PG_END_TRY();					
								
						if(cur_row_rejected)
							break; /* get out of loop, then go to next row */

						scan->nulls[m] = ' ';
						pstate->cur_attname = NULL;
					}
				}

                MemoryContextSwitchTo(oldcontext);

				if(cur_row_rejected)
				{
					FILEAM_IF_REJECT_LIMIT_REACHED_ABORT;
					FILEAM_GOTO_NEXT_ROW;
				}
				
				/* we can reset line buffer now. */
				EXT_RESET_LINEBUF;
				
				/* convert to heap tuple */
				tuple = heap_formtuple(scan->fs_tupDesc, scan->values, scan->nulls);

                MemoryContextReset(pstate->rowcontext);

				return tuple;
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
initParseState(CopyState pstate, Relation relation, List *fmtOpts, bool isText, 
			   char *uri, int rejectlimit, bool islimitinrows, Oid fmterrtbl,
			   int encoding)
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
	pstate->line_buf_converted = false;
	pstate->raw_buf_index = 0;
	pstate->processed = 0;
	pstate->filename = uri;

	/*
	 * Error handling setup
	 */
	if(rejectlimit == -1) 
	{
		/* Default error handling - "all-or-nothing" */
		pstate->cdbsreh = NULL; /* no SREH */
		pstate->errMode = ALL_OR_NOTHING; 
	}
	else
	{
		RangeVar *errtbl_rv;
		
		/* select the SREH mode */
		if(fmterrtbl == InvalidOid)
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
			
			if(rel == NULL)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
								errmsg("The specified error table for this "
									   "external table doesn't appear to "
									   "exist in the database. It may have "
									   "been dropped."),
								errhint("Refresh your external table definition.")
									   ));
			
			RelationDecrementReferenceCount(rel); /* must do this after RelationIdGetRelation() */
			
			
			errtbl_rv = makeRangeVar(get_namespace_name(get_rel_namespace(fmterrtbl)),
									 get_rel_name(fmterrtbl));
			
 #ifdef nothing			
			/* bummer... a non writer gang can't write to an error table! */
			if(!Gp_is_writer && Gp_role != GP_ROLE_DISPATCH)
				ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_YET),
								errmsg("Error tables can not yet be used in queries "
									   "with more than one table in the FROM clause. "
									   "You can use the same query with single row "
									   "error handling by redefining the external " 
									   "table without an error table "
									   )));
#endif
		}
		
		/* Single row error handling */
		pstate->cdbsreh = makeCdbSreh(true, /* don't DROP errtable at end of execution */
									  true, /* only relevant to COPY */
									  rejectlimit, 
									  islimitinrows, 
									  errtbl_rv, 
									  pstate->filename,
									  (char *)pstate->cur_relname);
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
	extvar_t extvar;
	time_t now = time(0);
	struct tm* tm = localtime(&now);
	char       *result = (char *) palloc(7);        /* sign, 5 digits, '\0' */
	
	/* set up extvar */

	memset(&extvar, 0, sizeof(extvar));
	extvar.GP_MASTER_HOST = qdHostname;
	
	pg_itoa(qdPostmasterPort, result);
	extvar.GP_MASTER_PORT = result;
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

	sprintf(extvar.GP_CID, "%x", QEDtxContextInfo.distributedSnapshotData.curcid);
	sprintf(extvar.GP_SN, "%x", scan->fs_scancounter);
	sprintf(extvar.GP_SEGMENT_ID, "%d", Gp_segment);
    sprintf(extvar.GP_SEG_PORT, "%d", PostPortNumber);
    
    /*
     * Open the url with url_open().
     */
    scan->fs_file = (FILE*) url_fopen(scan->fs_uri, PG_BINARY_R, &extvar);

	if (scan->fs_file == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open \"%s\" for reading: %m",
						scan->fs_uri)));
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
void
close_external_file(FILE *dataSource, char *dataSourceName)
{
	
	if(dataSource)
		if (url_fclose((URL_FILE*) dataSource))
			ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("could not close file \"%s\": %m",
							dataSourceName)));
	
	g_dataSource = dataSource = NULL;
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

		/* error is relevant to a particular line */
		if (cstate->line_buf_converted || !cstate->need_transcoding)
		{
			truncateEol(&copy_of_line_buf, cstate->eol_type);
			limit_printout_length(&copy_of_line_buf);

			errcontext("External table %s, line %d of %s: \"%s\"", cstate->cur_relname, cstate->cur_lineno, cstate->filename, copy_of_line_buf.data);
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
}
