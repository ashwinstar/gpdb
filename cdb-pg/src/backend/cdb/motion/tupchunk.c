/*-------------------------------------------------------------------------
 * tupchunk.c
 *     The data-structures and functions for dealing with tuple chunks.
 *
 *     WARNING:  This code is used by both amsd and the motion nodes.  Please
 *               be sure that if you change this file you aren't pulling in
 *			     postgres guts that will break building amsd.
 *
 * Copyright (c) 2005, Greenplum
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#ifdef FRONTEND

#include "postgres_fe.h"
#include <assert.h>
#define AssertArg(expr)  \
		assert( expr )

#else
#include "postgres.h"
#endif


#include <arpa/inet.h>

#include "cdb/tupchunk.h"
#include "lib/stringinfo.h"


/*=========================================================================
 * Functions to get/set values on tuple chunks.  These respect network byte
 * ordering differences with hardware byte ordering (they use htons() etc.).
 */


#define getUint16(pNetData)    ntohs( *( (uint16 *) (pNetData) ) )
#define getInt16(pNetData)    ntohs( *( (int16 *) (pNetData) ) )


/*---------------------------*/
/* Get/Set tuple chunk size. */
/*---------------------------*/

uint16 GetChunkDataSize(uint8 *tc_data)
{
    /* Motion node ID = offset 0 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 0);
}

void SetChunkDataSize(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Motion node ID = offset 0 */
    netval = htons(value);
    memcpy(tc_data + 0, &netval, 2);
}

/*---------------------------*/
/* Get/Set tuple chunk type. */
/*---------------------------*/

uint8 GetChunkType(uint8 *tc_data)
{
    return *(tc_data + 2);
}

void SetChunkType(uint8 *tc_data, TupleChunkType value)
{
    *(tc_data + 2) = (uint8) value;
}




