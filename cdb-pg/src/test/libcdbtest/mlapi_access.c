/*-------------------------------------------------------------------------
 *
 * mlapi_access.c
 *     Test functions for accessing the Motion Layer API.
 *
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION
 *     $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "funcapi.h"

#include "access/heapam.h"
#include "cdb/cdblink.h"
#include "cdb/cdbmotion.h"
#include "executor/spi.h"
#include "utils/memutils.h"

#include "cdbtest.h"


#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/*
 * For now, since Interconnect doesn't support inter-machine operations, we
 * use hard-coded values for certain fields.
 */

#define TEST_PROC_GROUP_ID  0x55AA
#define TEST_HOST_ID        0xAA55


/* Some static variables that are necessary across calls to the send/recv
 * test functions.
 */

static MotionNodeInfo s_motNodeInfo;

static bool s_sendInProgress = false;
static TupleSendContext s_tupSendCtxt;

static ReceiveReturnCode s_last_recvRC = -1;


/* Initialize a motion layer node. */
PG_FUNCTION_INFO_V1(MLAPI_InitMLNode__int1_bool1_regtype1);
Datum MLAPI_InitMLNode__int1_bool1_regtype1(PG_FUNCTION_ARGS)
{
    int mnid;
    Oid typeOid;
    TupleDesc tupDesc;
    bool preserveOrder;

    /* Motion node ID */
    mnid = PG_GETARG_INT32(0);

    /* Order-preserving flag */
    preserveOrder = PG_GETARG_BOOL(1);

    /* Tuple-descriptor describing type of all tuples sent and received by
     * this motion node.
     */
    typeOid = PG_GETARG_OID(2);
    tupDesc = TypeGetTupleDesc(typeOid, NULL);

    InitMotionLayerNode(mnid, /*include-backup-segs*/ false, preserveOrder,
        tupDesc);

    PG_RETURN_INT32(mnid);
}


/* Clean up a motion layer node. */
PG_FUNCTION_INFO_V1(MLAPI_EndMLNode__int1);
Datum MLAPI_EndMLNode__int1(PG_FUNCTION_ARGS)
{
    int mnid;

    /* Motion node ID */
    mnid = PG_GETARG_INT32(0);

    EndMotionLayerNode(mnid, /* flush-comm-layer */ false);

    PG_RETURN_INT32(mnid);
}


PG_FUNCTION_INFO_V1(MLAPI_SendTuple__int1_text1);
Datum MLAPI_SendTuple__int1_text1(PG_FUNCTION_ARGS)
{
    int mnid, mySegIdx, rc;
    char *pszSQL;
    SendReturnCode sendRC;
    MemoryContext oldCtxt;

    if (s_sendInProgress)
        elog(ERROR, "An incomplete send-tuple is in progress.");

    /*---------------------*
     * Arguments
     */

    /* Motion node ID */
    mnid = PG_GETARG_INT32(0);

    /* Get the SQL that will generate the tuple to send. */
    pszSQL = GET_STR(PG_GETARG_TEXT_P(1));

    /*---------------------------------*
     * Run the SQL and get a row back.
     */
    
    rc = SPI_connect();
    if (rc != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect() reported error.");

    /* Execute read-only SQL, and get only the first tuple. */
    rc = SPI_execute(pszSQL, true, 1);
    if (rc != SPI_OK_SELECT)
        elog(ERROR, "This function requires a SELECT statement.");

    /*-----------------------------*
     * Set up motion node info.
     */

    oldCtxt = MemoryContextSwitchTo(CacheMemoryContext);
    
    bzero(&s_motNodeInfo, sizeof(s_motNodeInfo));

    getCdbIdentity(&mySegIdx, NULL);

    s_motNodeInfo.process_group_id = TEST_PROC_GROUP_ID;
    s_motNodeInfo.motion_node_id = mnid;
    s_motNodeInfo.segment_id = mySegIdx;
    s_motNodeInfo.host_id = TEST_HOST_ID;

    /*-----------------------------*
     * Set up tuple send context.
     */

    bzero(&s_tupSendCtxt, sizeof(s_tupSendCtxt));

    s_tupSendCtxt.heap_tuple = heap_copytuple(SPI_tuptable->vals[0]);
    s_tupSendCtxt.target_segment = mySegIdx;
    s_tupSendCtxt.flags = 0;

    MemoryContextSwitchTo(oldCtxt);

    /*-----------------------------*
     * Do the send!
     */
    
    sendRC = SendTuple(&s_motNodeInfo, &s_tupSendCtxt);
    s_sendInProgress = (sendRC == SEND_INCOMPLETE);

    if (sendRC == SEND_COMPLETE)
    {
        /* Time to clean up our intermediate state. */

        bzero(&s_motNodeInfo, sizeof(s_motNodeInfo));

        heap_freetuple(s_tupSendCtxt.heap_tuple);
        clearTCList(s_tupSendCtxt.tuple_chunk_list);
        bzero(&s_tupSendCtxt, sizeof(s_tupSendCtxt));
    }

    SPI_finish();
    PG_RETURN_INT32(sendRC);
}


PG_FUNCTION_INFO_V1(MLAPI_FinishSendTuple);
Datum MLAPI_FinishSendTuple(PG_FUNCTION_ARGS)
{
    SendReturnCode sendRC;

    if (!s_sendInProgress)
        elog(ERROR, "No incomplete send-tuple in progress.");

    sendRC = SendTuple(&s_motNodeInfo, &s_tupSendCtxt);
    s_sendInProgress = (sendRC == SEND_INCOMPLETE);

    if (sendRC == SEND_COMPLETE)
    {
        /* Time to clean up our intermediate state. */

        bzero(&s_motNodeInfo, sizeof(s_motNodeInfo));

        heap_freetuple(s_tupSendCtxt.heap_tuple);
        clearTCList(s_tupSendCtxt.tuple_chunk_list);
        bzero(&s_tupSendCtxt, sizeof(s_tupSendCtxt));
    }

    PG_RETURN_INT32(sendRC);
}


PG_FUNCTION_INFO_V1(MLAPI_RecvTuple__int1);
Datum MLAPI_RecvTuple__int1(PG_FUNCTION_ARGS)
{
    int mnid, mySegIdx;
    HeapTuple htup;

    /*---------------------*
     * Arguments
     */

    /* Motion node ID */
    mnid = PG_GETARG_INT32(0);

    /*---------------------*
     * Perform the receive.
     */

    /* Make sure that the motion node info is set up correctly. */

    bzero(&s_motNodeInfo, sizeof(s_motNodeInfo));

    getCdbIdentity(&mySegIdx, NULL);

    s_motNodeInfo.process_group_id = TEST_PROC_GROUP_ID;
    s_motNodeInfo.motion_node_id = mnid;
    s_motNodeInfo.segment_id = mySegIdx;
    s_motNodeInfo.host_id = TEST_HOST_ID;

    /* Try to receive a tuple. */

    htup = NULL;
    s_last_recvRC = RecvTuple(&s_motNodeInfo, &htup);

    if (s_last_recvRC == GOT_TUPLE)
    {
        /* Return the tuple we got. */
        return HeapTupleGetDatum(htup);
    }
    else
    {
        /* Return NULL. */
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(MLAPI_GetLastRecvCode);
Datum MLAPI_GetLastRecvCode(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT32(s_last_recvRC);
}

