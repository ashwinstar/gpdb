/*-------------------------------------------------------------------------
 * tupchunk.h
 *     The data-structures and functions for dealing with tuple chunks.
 *
 * Copyright (c) 2005, Greenplum
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
	TC_SHUTDOWN, 		/* Indicates normal shutdown to be performed. 
						   Should only be sent from pg to ams-svc.
						 */

    TC_MAXVAL           /* For range checks on type values. */
} TupleChunkType;



/* This is the size of a tuple-chunk header, as it appears in the packet that
 * comes from the network.  Thus, some values are packed into 2 bytes or 1
 * byte in this header.  The break-down is as follows:
 *
 *    Offset      Description           Size
 *      0    Tuple Chunk Size         2 bytes
 *      2    Tuple Type               1 byte
 *   ------------------------------------------
 *                            TOTAL:  3 BYTES
 */
#define TUPLE_CHUNK_HEADER_SIZE 3


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

/*#define BROADCAST_SEGIDX        0x7FFF*/
#define BROADCAST_SEGIDX        0xFF
/*#define ALL_SEGDBS_MASK         0x8000*/

#define ANY_ROUTE -100

#define GetSegIdx(x)            (x)
#define IsBroadcastSegIdx(x)    (GetSegIdx(x) == BROADCAST_SEGIDX)

/* Simple accessors/mutators for parts of the tuple-chunk header.  These
 * functions store multibyte integers in network byte order, and convert
 * back to hardware byte order on retrieval.
 */


extern uint16 GetChunkDataSize(uint8 *tc_data);
extern uint8  GetChunkType(uint8 *tc_data);

extern void  SetChunkDataSize(uint8 *tc_data, uint16 value);
extern void  SetChunkType(uint8 *tc_data, TupleChunkType value);

#endif /* TUPCHUNK_H */
