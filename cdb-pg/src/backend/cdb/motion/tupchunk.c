/*-------------------------------------------------------------------------
 * tupchunk.c
 *     The data-structures and functions for dealing with tuple chunks.
 *
 *     WARNING:  This code is used by both amsd and the motion nodes.  Please
 *               be sure that if you change this file you aren't pulling in
 *			     postgres guts that will break building amsd.
 *
 * Copyright (c) 2005, Metapa
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


/*-------------------------------*/
/* Get/Set target segment index. */
/*-------------------------------*/

uint16 GetTargetSegIdx(uint8 *tc_data)
{
    /* Target segment index = offset 0 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 0);
}

void SetTargetSegIdx(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Target segment index = offset 0 */
    netval = htons(value);
    memcpy(tc_data + 0, &netval, 2);
}

/*-------------------------*/
/* Get/Set query group ID. */
/*-------------------------*/

uint16 GetQueryGroupID(uint8 *tc_data)
{
    /* Query group ID = offset 2 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 2);
}

void SetQueryGroupID(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Query group ID = offset 2 */
    netval = htons(value);
    memcpy(tc_data + 2, &netval, 2);
}

/*-------------------------------*/
/* Get/Set source segment index. */
/*-------------------------------*/

uint16 GetSourceSegIdx(uint8 *tc_data)
{
    /* Source segment index = offset 4 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 4);
}

void SetSourceSegIdx(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Source segment index = offset 4 */
    netval = htons(value);
    memcpy(tc_data + 4, &netval, 2);
}

/*-------------------------*/
/* Get/Set motion node ID. */
/*-------------------------*/

uint16 GetMotionNodeID(uint8 *tc_data)
{
    /* Motion node ID = offset 6 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 6);
}

void SetMotionNodeID(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Motion node ID = offset 6 */
    netval = htons(value);
    memcpy(tc_data + 6, &netval, 2);
}

/*---------------------------*/
/* Get/Set tuple chunk size. */
/*---------------------------*/

uint16 GetChunkDataSize(uint8 *tc_data)
{
    /* Motion node ID = offset 8 */
    AssertArg(tc_data != NULL);
    return getUint16(tc_data + 8);
}

void SetChunkDataSize(uint8 *tc_data, uint16 value)
{
    uint16 netval;
    
    AssertArg(tc_data != NULL);
    
    /* Motion node ID = offset 8 */
    netval = htons(value);
    memcpy(tc_data + 8, &netval, 2);
}

/*---------------------------*/
/* Get/Set tuple chunk type. */
/*---------------------------*/

uint8 GetChunkType(uint8 *tc_data)
{
    return *(tc_data + 10);
}

void SetChunkType(uint8 *tc_data, TupleChunkType value)
{
    *(tc_data + 10) = (uint8) value;
}




