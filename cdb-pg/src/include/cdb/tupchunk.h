/*-------------------------------------------------------------------------
 * tupchunk.h
 *     The data-structures and functions for dealing with tuple chunks.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef TUPCHUNK_H
#define TUPCHUNK_H

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
    TC_ABORT_STREAM,    /* Indicates abnormal termination of execution. */

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


/* The target segment index also encodes whether the data should go only to
 * the active segment-database for the segment-index, or to all segment-
 * databases for that segment-index, in the top bit.  (0 = active only;
 * 1 = all seg-dbs for the seg-index.)  In addition, the segment-index 0x7FFF
 * means "broadcast."
 */

/* TODO HACK: this no longer works now that segIdx is based on the "content" id.
 *            since it can be negative.  segIdX is no longer unsigned so we cant
 *            use the top bit anymore.
 * 
 * 	      Below you'll see we have essentially disabled the difference 
 *            between "active" segdb and backup
 */

//#define BROADCAST_SEGIDX        0x7FFF
#define BROADCAST_SEGIDX        0xFF
//#define ALL_SEGDBS_MASK         0x8000

//#define GetSegIdx(x)            ((x) & ~ALL_SEGDBS_MASK)
#define GetSegIdx(x)            (x)
#define IsBroadcastSegIdx(x)    (GetSegIdx(x) == BROADCAST_SEGIDX)
//#define IsActiveOnlySegIdx(x)   ( ((x) & ALL_SEGDBS_MASK) == 0 )
//#define SetAllSegdbsFlag(x)     ((x) | ALL_SEGDBS_MASK)

/* Simple accessors/mutators for parts of the tuple-chunk header.  These
 * functions store multibyte integers in network byte order, and convert
 * back to hardware byte order on retrieval.
 */

extern int16  GetTargetSegIdx(uint8 *tc_data);
extern uint16  GetQueryGroupID(uint8 *tc_data);
extern int16  GetSourceSegIdx(uint8 *tc_data);
extern uint16  GetMotionNodeID(uint8 *tc_data);
extern uint16  GetChunkDataSize(uint8 *tc_data);
extern uint8   GetChunkType(uint8 *tc_data);

extern void  SetTargetSegIdx(uint8 *tc_data, int16 value);
extern void  SetQueryGroupID(uint8 *tc_data, uint16 value);
extern void  SetSourceSegIdx(uint8 *tc_data, int16 value);
extern void  SetMotionNodeID(uint8 *tc_data, uint16 value);
extern void  SetChunkDataSize(uint8 *tc_data, uint16 value);
extern void  SetChunkType(uint8 *tc_data, TupleChunkType value);


/* This is the maximum allowable tuple-chunk data-payload size.  It is based
 * on the maximum message size that we want to support by the underlying IPC
 * and network transports used by the Motion Layer and AMS.
 *
 * TODO:  Calculate max from max_msg_size - ams_pkt_hdr - tuple_chunk_hdr.
 */
#define MIN_TC_DATA_SIZE 64
#define MAX_TC_DATA_SIZE 32768
#define DEFAULT_TC_DATA_SIZE 1024




#endif /* TUPCHUNK_H */
