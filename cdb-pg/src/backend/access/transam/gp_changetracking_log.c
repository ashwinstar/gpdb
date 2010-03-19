/*-------------------------------------------------------------------------
 *
 * gp_changetracking_log.c
 *		Set-returning function to view gp_distributed_log table.
 *
 * IDENTIFICATION
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>

#include "funcapi.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "cdb/cdbvars.h"                /* Gp_segment */

Datum		gp_changetracking_log(PG_FUNCTION_ARGS);

typedef struct CTContext
{
	File	file;			/* changetracking File */
	char	pageBuffer[CHANGETRACKING_BLCKSZ];	/* current page */
	char*	readRecordBuf; 	/* ReadRecord result area */
	char*	readHeaderBuf; 	/* Header result area */
	int		recs_read;
	int		recs_in_page;
	int		block_number;
	int		logRecOff;		/* offset of next record in page */
	int		logPageOff;		/* offset of current page in file */

} CTContext;


/* Read another page, if possible */
static bool
readChangeTrackingPage(CTContext *context)
{
	size_t nread = FileRead(context->file, context->pageBuffer, CHANGETRACKING_BLCKSZ);

	if (nread == CHANGETRACKING_BLCKSZ)
	{
		context->logPageOff += CHANGETRACKING_BLCKSZ;
		return true;
	}
	
	if (nread != 0)
		elog(ERROR, "Unexpected partial page read of %d bytes", (int) nread);

	return false;
}

/*
 * Attempt to read a ChangeTracking record into readRecordBuf.
 */
static bool
ReadRecord(CTContext* context)
{
	ChangeTrackingRecord 		*record;
	ChangeTrackingPageHeader	*header;
	char	   *buffer;
	uint32		header_len = sizeof(ChangeTrackingPageHeader);
	uint32		record_len = sizeof(ChangeTrackingRecord);

	if (context->logRecOff <= 0 || context->recs_read == context->recs_in_page)
	{
		/* Need to advance to new page */
		if (!readChangeTrackingPage(context))
			return false;
		
		context->logRecOff = 0;
		context->recs_read = 0;
		
		/* read page header */
		memcpy(context->readHeaderBuf, context->pageBuffer, header_len);
		header = (ChangeTrackingPageHeader *)context->readHeaderBuf;
		
		if(header->blockversion != 1)
			elog(ERROR, "gp_changetracking_log currently supports storage version 1 only");
		
		context->recs_in_page = header->numrecords;		
		context->logRecOff += sizeof(ChangeTrackingPageHeader);
	}
	
	record = (ChangeTrackingRecord *) (context->pageBuffer + context->logRecOff);
	buffer = context->readRecordBuf;
	
	memcpy(buffer, record, record_len);
	record = (ChangeTrackingRecord *) buffer;
	context->logRecOff += record_len;
	context->recs_read++;
	
	return true;
}

/*
 * produce a view of the changtracking file which includes
 * all the buffer pool information needed for resynchronization.
 */
Datum
gp_changetracking_log(PG_FUNCTION_ARGS)
{
	
	FuncCallContext *funcctx;
	CTContext *context;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		char	path[MAXPGPATH];

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match gp_distributed_xacts view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(9, false);
		
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segment_id", INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "dbid", INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "space", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "db", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "rel", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "xlogloc", XLOGLOCOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "blocknum", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "persistent_tid", TIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "persistent_sn", INT8OID, -1, 0); /* "fake" INT8, like in gp_persistent.c/h */
		
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the locking information that we will format and send
		 * out as a result set.
		 */
		context = (CTContext *) palloc(sizeof(CTContext));
		
		/* set up correct change tracking file path */
		ChangeTrackingLogFilePath(path);
		
		/* open it. error out if doesn't exist */
		context->file = PathNameOpenFile(path, 
										 O_RDONLY | PG_BINARY, 
										 S_IRUSR | S_IWUSR);

		if(context->file < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("gp_changetracking_log couldn't open file '%s' on segment %d dbid %d", 
							 path, Gp_segment, GpIdentity.dbid)));

		context->block_number = 0;
		context->logPageOff = -CHANGETRACKING_BLCKSZ; /* so 1st increment will give 0 */
		context->logRecOff = 0;
		context->recs_in_page = 0;
		context->recs_read = 0;
		context->readHeaderBuf = (char *) palloc(sizeof(ChangeTrackingPageHeader));
		context->readRecordBuf = (char *) palloc(sizeof(ChangeTrackingRecord));

		funcctx->user_fctx = (void *) context;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	context = (CTContext *) funcctx->user_fctx;

	/*
	 * Get the next record. No more records? we're done.
	 */
	if (ReadRecord(context))
	{
		Datum		values[9];
		bool		nulls[9];
		HeapTuple	tuple;
		Datum		result;
		
		ChangeTrackingRecord *record = (ChangeTrackingRecord *) context->readRecordBuf;
		
		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = Int16GetDatum((int16)GpIdentity.segindex);
		values[1] = Int16GetDatum((int16)GpIdentity.dbid);
		values[2] = ObjectIdGetDatum(record->relFileNode.spcNode);
		values[3] = ObjectIdGetDatum(record->relFileNode.dbNode);
		values[4] = ObjectIdGetDatum(record->relFileNode.relNode);
		values[5] = PointerGetDatum(&record->xlogLocation);
		values[6] = UInt32GetDatum(record->bufferPoolBlockNum);		
		values[7] = PointerGetDatum(&record->persistentTid);
		values[8] = Int64GetDatum(record->persistentSerialNum);
		       
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	pfree(context->readHeaderBuf);
	pfree(context->readRecordBuf);
	FileClose(context->file);
	
	funcctx->user_fctx = NULL;
	
	SRF_RETURN_DONE(funcctx);
}

