/*-------------------------------------------------------------------------
 * tupchunk.h
 *     The data-structures and functions for dealing with tuple chunks.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef TUPCHUNK_H
#define TUPCHUNK_H


#include "access/htup.h"
#include "access/tupdesc.h"


/*--------------*/
/* Tuple Chunks */
/*--------------*/


typedef enum TupleChunkType
{
    TC_WHOLE,           /* Contains a whole tuple. */
    TC_PARTIAL_START,   /* Contains the starting portion of a tuple. */
    TC_PARTIAL_MID,     /* Contains a middle part of a tuple. */
    TC_PARTIAL_END,     /* Contains the final portion of a tuple. */
    TC_END_OF_STREAM,   /* Indicates "end of tuples" from this source. */
    
    TC_MAXVAL           /* For range checks on type values. */
} TupleChunkType;



/* This is the size of a tuple-chunk header, as it appears in the packet that
 * comes from the network.  Thus, some values are packed into 2 bytes or 1
 * byte in this header.  The break-down is as follows:
 *
 *    Offset      Description           Size
 *       0    Target Segment Index     2 bytes
 *       2    Group ID                 2 bytes
 *       4    Source Segment Index     2 bytes
 *       6    Motion Node ID           2 bytes
 *       8    Tuple Chunk Size         2 bytes
 *      10    Tuple Type               1 byte
 *   ------------------------------------------
 *                            TOTAL:  11 BYTES
 */
#define TUPLE_CHUNK_HEADER_SIZE 11


/* Simple accessors/mutators for parts of the tuple-chunk header.  These
 * functions store multibyte integers in network byte order, and convert
 * back to hardware byte order on retrieval.
 */

extern uint16  GetTargetSegIdx(uint8 *tc_data);
extern uint16  GetQueryGroupID(uint8 *tc_data);
extern uint16  GetSourceSegIdx(uint8 *tc_data);
extern uint16  GetMotionNodeID(uint8 *tc_data);
extern uint16 GetChunkDataSize(uint8 *tc_data);
extern uint8      GetChunkType(uint8 *tc_data);

extern void  SetTargetSegIdx(uint8 *tc_data, uint16 value);
extern void  SetQueryGroupID(uint8 *tc_data, uint16 value);
extern void  SetSourceSegIdx(uint8 *tc_data, uint16 value);
extern void  SetMotionNodeID(uint8 *tc_data, uint16 value);
extern void SetChunkDataSize(uint8 *tc_data, uint16 value);
extern void     SetChunkType(uint8 *tc_data, TupleChunkType value);


/* This is the maximum allowable tuple-chunk data-payload size.  It is based
 * on the maximum message size that we want to support by the underlying IPC
 * and network transports used by the Motion Layer and AMS.
 *
 * TODO:  Calculate max from max_msg_size - ams_pkt_hdr - tuple_chunk_hdr.
 */
#define MIN_TC_DATA_SIZE 64
#define MAX_TC_DATA_SIZE 32768
#define DEFAULT_TC_DATA_SIZE 1024

/*-------------------*/
/* Tuple Chunk Lists */
/*-------------------*/


/* An item in a tuple-chunk list.  Chunk data is just a sequence of bytes.
 * A "tuple-chunk" struct doesn't make sense for several reasons:
 *   - The struct would need to be packed, not word-aligned, to keep from
 *     sending too much stuff.  This can be tricky to achieve across various
 *     compilers.
 *   - Multi-byte integers need to be transmitted in network byte order, which
 *     may not match hardware byte order.
 * Therefore, it's just a chunk of bytes in the tuple-chunk list items.
 */
typedef struct TupleChunkListItemData
{
    /* Pointer to next item in the list. */
    struct TupleChunkListItemData *p_next;
    
    /* Length of chunk, including header.  We could calculate this value, but
     * why trouble ourselves???
     */
    uint32 chunk_length;
    
    /* Variable-length data portion of the struct. */
    uint8 chunk_data[];
    
} TupleChunkListItemData, *TupleChunkListItem;


/* A list of tuple-chunks, in order, for a single HeapTuple.
 *
 * NOTE:  This structure is intended to hold tuple-chunks for ONLY ONE tuple
 *        at a time!!!  Tuple chunks have no information that indicates which
 *        tuple they are a part of, because they are expected to be sent and
 *        received in order, and without loss.  So, BEWARE.
 */
typedef struct TupleChunkListData
{
    /* If we ever want to generate these lists on an incremental basis (would
     * be desirable for very large tuples, so that we use less memory during
     * the process - although other things in Postgres would probably break
     * too), the HeapTuple and the TupleDesc will probably need to be stored
     * in this structure.
     */
    
    /* Pointers to start and end of list. */
    TupleChunkListItem p_first;
    TupleChunkListItem p_last;
    
    /* Count of chunks in the list. */
    int num_chunks;
    
    /* Total bytes of serialized tuple data, not including chunk headers. */
    int serialized_data_length;
    
} TupleChunkListData, *TupleChunkList;


extern TupleChunkList CvtHeapTupToChunks(HeapTuple htup, TupleDesc tupdesc,
    int maxChunkDataSize, uint16 srcSegIdx, uint16 tgtSegIdx, uint16 mnid);


extern HeapTuple CvtChunksToHeapTup(TupleChunkList tclist, TupleDesc tupdesc);



/* Sends a tuple chunk from the Postgres process to the local AMS process via
 * IPC.  This function does not block; if the IPC channel cannot accept the
 * tuple chunk for some reason, then this is indicated by a return-code.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - tcItem:  The tuple-chunk data to send.
 *
 * RETURN:
 *   - false if the chunk could not be sent, or true if it could be sent.
 */
extern bool SendTupleChunkToAMS(TupleChunkListItem tcItem);


/* Receives a tuple chunk from the local AMS process via IPC.  This function
 * does not block; if there is no tuple chunk available, then NULL is
 * returned.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - none.
 *
 * RETURN:
 *   - A populated TupleChunkListItemData structure (allocated with palloc()),
 *     or NULL if no chunk is available at time of call.
 */
extern TupleChunkListItem RecvTupleChunkFromAMS(void);


extern void appendChunkToTCList(TupleChunkList tcList, TupleChunkListItem tcItem);


#endif /* TUPCHUNK_H */
