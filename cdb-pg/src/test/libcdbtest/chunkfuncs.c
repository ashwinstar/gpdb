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
#include "executor/spi.h"
#include "utils/builtins.h"

#include "libpq-fe.h"


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


static ChunkedRowState *s_func_state;



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
PG_FUNCTION_INFO_V1(GetRowsAsChunks);
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
        
        s_func_state = (ChunkedRowState *) palloc0(sizeof(ChunkedRowState));
        s_func_state->rowIdx = 0;
        s_func_state->rowChunks = NULL;
        s_func_state->chunkNum = 0;
        
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
    
    if (s_func_state->rowChunks == NULL || s_func_state->nextChunk == NULL)
    {
        /* The chunk list is empty, or we have traversed all chunks in the
         * current row.  Go to the next row, and generate a new list of chunks
         * for that row.
         */

        s_func_state->rowIdx++;

        if (s_func_state->rowIdx <= SPI_processed)
        {
            s_func_state->rowChunks = CvtHeapTupToChunks(
                SPI_tuptable->vals[s_func_state->rowIdx],
                SPI_tuptable->tupdesc,
                s_func_state->maxChunkDataSize,
                s_func_state->srcSegIdx,
                s_func_state->tgtSegIdx,
                s_func_state->mnid);
        }
        
        s_func_state->nextChunk = s_func_state->rowChunks->p_first;
        s_func_state->chunkNum = 0;
    }
    
    if (s_func_state->rowChunks == NULL ||
        s_func_state->rowChunks->p_first == NULL)
    {
        /* There are no more tuples and no more chunks.  We are done. */
        SRF_RETURN_DONE(funcctx);
    }
    else
    {
        /* There are more chunks.  Return the next chunk as a row. */

        size_t escChunkLength;
        char *pszEscChunk;

        resultValues[0] = Int32GetDatum(s_func_state->rowIdx + 1);
        resultValues[1] = Int32GetDatum(s_func_state->chunkNum + 1);

        pszEscChunk = PQescapeBytea(s_func_state->nextChunk->chunk_data,
            s_func_state->nextChunk->chunk_length, &escChunkLength);

        resultValues[2] = CStringGetDatum(pszEscChunk);

        /* Form the tuple. */
        htup = heap_formtuple(funcctx->tuple_desc, resultValues, resultNulls);
        resultTuple = HeapTupleGetDatum(htup);

        /* Clean up memory. */
        PQfreemem(pszEscChunk);
        
        /* Go to the next chunk, for the next call to this function. */
        s_func_state->nextChunk = s_func_state->nextChunk->p_next;
        s_func_state->chunkNum++;
        
        /* Return our result! */
        SRF_RETURN_NEXT(funcctx, resultTuple);
    }
}

