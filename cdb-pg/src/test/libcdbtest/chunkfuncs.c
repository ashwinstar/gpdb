/*-------------------------------------------------------------------------
 *
 * chunkfuncs.c
 *     Test functions for tuple chunk API.  These are to be called from SQL
 *     for testing purposes.
 *
 * Copyright (c) 2005, Metapa Inc.  All rights reserved.
 *
 *
 * IDENTIFICATION
 *     $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"

#include "access/tupdesc.h"
#include "cdb/tupchunk.h"
#include "cdb/tupser.h"
#include "executor/spi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"

#include "libpq-fe.h"


Datum GetSerializedRows__text1(PG_FUNCTION_ARGS);
Datum GetRowsAsChunks__text1(PG_FUNCTION_ARGS);


#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))



typedef struct ChunkedRowState
{
    int maxChunkDataSize;
    uint16 srcSegIdx;
    uint16 tgtSegIdx;
    uint16 mnid;

    /* Index of row we are on. */
    int rowIdx;

    /* Count of which chunk in the current row we are on. */
    int chunkNum;

    /* List of tuple-chunks for the current row. */
    TupleChunkList rowChunks;
    TupleChunkListItem nextChunk;
} ChunkedRowState;

/* Static state for GetRowsAsChunks function. */
static ChunkedRowState *s_rac_state;



typedef struct SerializedRowState
{
    /* Index of row we are on. */
    int rowIdx;
} SerializedRowState;

/* Static state for GetSerializedRows function. */
static SerializedRowState *s_sr_state;



/* This function executes a SQL "SELECT" statement and returns the results as
 * serialized data.  The returned rows have the following format:
 *
 * +--------------+--------------------+
 * | reportedsize | serialized (BYTEA) |
 * +--------------+--------------------+
 *
 * This is defined as the "__getserializedrows" data-type.
 */
PG_FUNCTION_INFO_V1(GetSerializedRows__text1);
Datum GetSerializedRows__text1(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    char *pszSQL;
    int rc;

    TupleDesc tupdesc;
    HeapTuple htup;

    Datum resultValues[2];          /* Parts of the result set. */
    char *resultNulls = "  ";       /* No NULLs in result set. */
    Datum resultTuple;              /* The full result. */

    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL())
    {
        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /* switch to memory context appropriate for multiple function calls */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /*---------------------*
         * One-time operations
         */

        /* Get the SQL to execute, and run it. */
        pszSQL = GET_STR(PG_GETARG_TEXT_P(0));
        rc = SPI_connect();
        if (rc != SPI_OK_CONNECT)
            elog(ERROR, "SPI_connect() reported error.");

        /* Execute read-only SQL.  The 0 means "no limit on results." */
        rc = SPI_execute(pszSQL, true, 0);
        if (rc != SPI_OK_SELECT)
            elog(ERROR, "This function requires a SELECT statement.");

        /* Allocate cross-call state, so that we can keep track of where we're
         * at in the processing.
         */

        s_sr_state = (SerializedRowState *) palloc0(sizeof(SerializedRowState));
        s_sr_state->rowIdx = 0;

        /* Generate return-type information.  This is derived from the user
         * datatype "__getserializedrows".
         */
        
        tupdesc = RelationNameGetTupleDesc("__getserializedrows");
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
    }
    
    
    /*---------------------*
     * Per-call operations
     */
    
    funcctx = SRF_PERCALL_SETUP();
    
    /* The chunk list is empty, or we have traversed all chunks in the
     * current row.  Go to the next row, and generate a new list of chunks
     * for that row.
     */

    if (s_sr_state->rowIdx < SPI_processed)
    {
        /* There are more chunks.  Return the next chunk as a row. */

        StringInfoData serTupData;
        int reportedSize;
        bytea *chunkData;

        pq_begintypsend(&serTupData);   /* Does initStringInfo() call. */
        reportedSize = SerializeTuple(SPI_tuptable->vals[s_sr_state->rowIdx],
            SPI_tuptable->tupdesc, &serTupData);
        chunkData = pq_endtypsend(&serTupData);

        resultValues[0] = Int32GetDatum(reportedSize);
        resultValues[1] = PointerGetDatum(chunkData);

        /* Form the tuple. */
        htup = heap_formtuple(funcctx->tuple_desc, resultValues, resultNulls);
        resultTuple = HeapTupleGetDatum(htup);

        /* Move on to the next row in the list, for next iteration. */
        s_sr_state->rowIdx++;

        /* Return our result! */
        SRF_RETURN_NEXT(funcctx, resultTuple);
    }
    else
    {
        /* There are no more tuples.  We are done. */

        /* Do final cleanup. */
        SPI_finish();

        SRF_RETURN_DONE(funcctx);
    }
}



/* This function executes a SQL "SELECT" statement and returns the results as
 * a collection of tuple chunks.  The returned rows have the following format:
 *
 * +------------------+--------------------+-------------------+
 * | rownum (INTEGER) | chunknum (INTEGER) | chunkdata (BYTEA) |
 * +------------------+--------------------+-------------------+
 *
 * This is defined as the "__getrowsaschunks" data-type.
 *
 * Both rownum and chunknum start at 1.
 */
PG_FUNCTION_INFO_V1(GetRowsAsChunks__text1);
Datum GetRowsAsChunks__text1(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    char *pszSQL;
    int rc;

    TupleDesc tupdesc;
    HeapTuple htup;

    Datum resultValues[3];          /* Parts of the result set. */
    char *resultNulls = "   ";      /* No NULLs in result set. */
    Datum resultTuple;              /* The full result. */


    /* stuff done only on the first call of the function */
    if (SRF_IS_FIRSTCALL())
    {
        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /* switch to memory context appropriate for multiple function calls */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /*---------------------*
         * One-time operations
         */
        
        /* Get the SQL to execute, and run it. */
        pszSQL = GET_STR(PG_GETARG_TEXT_P(0));
        rc = SPI_connect();
        if (rc != SPI_OK_CONNECT)
            elog(ERROR, "SPI_connect() reported error.");
        
        /* Execute read-only SQL.  The 0 means "no limit on results." */
        rc = SPI_execute(pszSQL, true, 0);
        if (rc != SPI_OK_SELECT)
            elog(ERROR, "This function requires a SELECT statement.");
        
        /* Allocate cross-call state, so that we can keep track of where we're
         * at in the processing.
         */
        
        s_rac_state = (ChunkedRowState *) palloc0(sizeof(ChunkedRowState));
        s_rac_state->rowIdx = 0;
        s_rac_state->rowChunks = NULL;
        s_rac_state->chunkNum = 0;
        
        /* Generate return-type information.  This is derived from the user
         * datatype "__getrowsaschunks".
         */
        
        tupdesc = RelationNameGetTupleDesc("__getrowsaschunks");
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
    }
    
    
    /*---------------------*
     * Per-call operations
     */
    
    funcctx = SRF_PERCALL_SETUP();
    
    if (s_rac_state->rowChunks == NULL || s_rac_state->nextChunk == NULL)
    {
        /* The chunk list is empty, or we have traversed all chunks in the
         * current row.  Go to the next row, and generate a new list of chunks
         * for that row.
         */

        s_rac_state->rowIdx++;

        if (s_rac_state->rowIdx <= SPI_processed)
        {
            s_rac_state->rowChunks = CvtHeapTupToChunks(
                SPI_tuptable->vals[s_rac_state->rowIdx],
                SPI_tuptable->tupdesc,
                s_rac_state->maxChunkDataSize,
                s_rac_state->srcSegIdx,
                s_rac_state->tgtSegIdx,
                s_rac_state->mnid);
        }
        
        s_rac_state->nextChunk = s_rac_state->rowChunks->p_first;
        s_rac_state->chunkNum = 0;
    }
    
    if (s_rac_state->rowChunks == NULL ||
        s_rac_state->rowChunks->p_first == NULL)
    {
        /* There are no more tuples and no more chunks.  We are done. */
        SRF_RETURN_DONE(funcctx);
    }
    else
    {
        /* There are more chunks.  Return the next chunk as a row. */

        size_t escChunkLength;
        char *pszEscChunk;

        resultValues[0] = Int32GetDatum(s_rac_state->rowIdx + 1);
        resultValues[1] = Int32GetDatum(s_rac_state->chunkNum + 1);

        pszEscChunk = PQescapeBytea(s_rac_state->nextChunk->chunk_data,
            s_rac_state->nextChunk->chunk_length, &escChunkLength);

        resultValues[2] = CStringGetDatum(pszEscChunk);

        /* Form the tuple. */
        htup = heap_formtuple(funcctx->tuple_desc, resultValues, resultNulls);
        resultTuple = HeapTupleGetDatum(htup);

        /* Clean up memory. */
        PQfreemem(pszEscChunk);
        
        /* Go to the next chunk, for the next call to this function. */
        s_rac_state->nextChunk = s_rac_state->nextChunk->p_next;
        s_rac_state->chunkNum++;
        
        /* Return our result! */
        SRF_RETURN_NEXT(funcctx, resultTuple);
    }
}

