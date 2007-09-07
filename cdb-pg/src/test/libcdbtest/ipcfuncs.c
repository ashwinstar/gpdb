/*-------------------------------------------------------------------------
 *
 * chunkfuncs.c
 *     Test functions for Motion Layer IPC API.  These are to be called from SQL
 *     for testing purposes.
 *
 * Copyright (c) 2005, Greenplum Inc.  All rights reserved.
 *
 *
 * IDENTIFICATION
 *     $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include <unistd.h>

#include "funcapi.h"
#include "libpq-fe.h"
#include "access/tupdesc.h"
#include "cdb/cdbvars.h"
#include "cdb/tupchunk.h"
#include "cdb/tupchunklist.h"
#include "cdb/cdbmotion.h"
#include "cdb/cdblink.h"
#include "cdb/ml_ipc.h"
#include "executor/spi.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"

#include "cdbtest.h"

#if 0

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))



/* This function takes the message provided as an argument and 
 * forms a tuplechunk for the message and sends it to itself
 * via the Motion Layer IPC API.  
 *
 * The function then reads its own incoming queue through the 
 * Motion Layer IPC API and returns the payload data as a string
 * via this function.  
 *
 * In other words, this function should return in a single row
 * (single column) the message that was passed into this function.
 *
 * NOTE: this function MUST be called in utility mode.!
 * 
 */

PG_FUNCTION_INFO_V1(ml_ipc_getMsg__text1);
Datum ml_ipc_getMsg__text1(PG_FUNCTION_ARGS)
{
	char	*msg = GET_STR(PG_GETARG_TEXT_P(0));
    int		mySegIndex = 1;

	/* temporarily disable this routine. see TODO item below. */
	ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_YET),
					errmsg("Interconnect testing through libcdbtest is currently disabled")
					));
	
    /* Initialize the Motion Layer IPC subsystem. */			
	InitMotionLayerIPC();

    /* Form a tuplechunk with the message as a payload */
    TupleChunkListItem tcItem = (TupleChunkListItem) 
									palloc0(sizeof(TupleChunkListData) + 
								    TUPLE_CHUNK_HEADER_SIZE +
									strlen(msg));

    tcItem->chunk_length = strlen(msg) + TUPLE_CHUNK_HEADER_SIZE;
    
    SetTargetSegIdx(tcItem->chunk_data, mySegIndex);
    SetQueryGroupID(tcItem->chunk_data, 1);
    SetSourceSegIdx(tcItem->chunk_data, mySegIndex);
    SetMotionNodeID(tcItem->chunk_data, 1);
    SetChunkDataSize(tcItem->chunk_data, strlen(msg) );
    SetChunkType(tcItem->chunk_data, TC_WHOLE );

    memcpy( tcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE, msg, strlen(msg) );

    /* Send it out via the ml_ipc */
    /* TODO: we need to be able to enable this call again in order to support this test routine */
	/*if( ! SendTupleChunkToAMS(tcItem) )
	{
	    elog(FATAL, "failed during SendTupleChunkToAMS( tcItem )" );
        //DOH: nasty error handling.
    }*/

    /* wait for amsd to get to it */
    sleep(1);

    /* Get it back from the ml_ipc */
    TupleChunkListItem recvTcItem;

    recvTcItem = RecvTupleChunkFromAMS( );

    if( recvTcItem == NULL )
    {
        //DOH: nasty error handling.
       elog(FATAL, "failed during RecvTupleChunkFromAMS( tcItem )" );
    }

    
    /* now pull out the message and form a string and return it back. */
    uint8 *dataStart = recvTcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE;

    uint16 dataSize;

	GetChunkDataSize(recvTcItem->chunk_data, &dataSize);
    
    //form result
    int len = dataSize + VARHDRSZ;
    text* result = (text *)palloc(len);
    
    /* set size of result string... */
    SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, dataStart, dataSize);
	

    //be nice and cleanup
    pfree(tcItem);
	pfree(msg);
	pfree(recvTcItem);

       
    PG_RETURN_TEXT_P(result);
    //PG_RETURN_TEXT_P( PG_GETARG_TEXT_P(0) );
}

#endif
