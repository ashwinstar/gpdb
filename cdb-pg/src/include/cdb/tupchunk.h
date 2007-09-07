/*-------------------------------------------------------------------------
 * tupchunk.h
 *	   The data-structures and functions for dealing with tuple chunks.
 *
 * Copyright (c) 2005-2006, Greenplum
 *-------------------------------------------------------------------------
 */
#ifndef TUPCHUNK_H
#define TUPCHUNK_H

/*--------------*/
/* Tuple Chunks */
/*--------------*/


typedef enum TupleChunkType
{
	TC_WHOLE,					/* Contains a whole tuple. */
	TC_PARTIAL_START,			/* Contains the starting portion of a tuple. */
	TC_PARTIAL_MID,				/* Contains a middle part of a tuple. */
	TC_PARTIAL_END,				/* Contains the final portion of a tuple. */
	TC_END_OF_STREAM,			/* Indicates "end of tuples" from this source. */
	TC_EMPTY,					/* Empty tuple */
	TC_MAXVAL					/* For range checks on type values. */
} TupleChunkType;



/* This is the size of a tuple-chunk header, as it appears in the packet that
 * comes from the network.	Thus, some values are packed into 2 bytes or 1
 * byte in this header.  The break-down is as follows:
 *
 *	  Offset	  Description			Size
 *		0	 Tuple Chunk Size		  2 bytes
 *		2	 Tuple Type				  2 byte
 *	 ------------------------------------------
 *							  TOTAL:  4 BYTES
 *
 * Yes, we could make this smaller. But we're doing lots of memcpy()s
 * of data immediately following these headers. Let's align the data on
 * 32-bit boundaries!
 */
#define TUPLE_CHUNK_HEADER_SIZE 4


/* The target segment index also encodes whether the data should go only to
 * the active segment-database for the segment-index, or to all segment-
 * databases for that segment-index, in the top bit.  (0 = active only;
 * 1 = all seg-dbs for the seg-index.)	In addition, the segment-index 0x7FFF
 * means "broadcast."
 */

/* TODO HACK: this no longer works now that segIdx is based on the "content" id.
 *			  since it can be negative.  segIdX is no longer unsigned so we cant
 *			  use the top bit anymore.
 *
 *		  Below you'll see we have essentially disabled the difference
 *			  between "active" segdb and backup
 */

/*#define BROADCAST_SEGIDX		  0x7FFF*/
#define BROADCAST_SEGIDX		0xFF
/*#define ALL_SEGDBS_MASK		  0x8000*/

#define ANY_ROUTE -100

#define GetSegIdx(x)			(x)
#define IsBroadcastSegIdx(x)	(GetSegIdx(x) == BROADCAST_SEGIDX)

/* Simple macros for accessing tuple-chunk headers; NOTE: we no longer
 * use network byte order */

/* add support for "inplace" chunk items */
#define GetChunkDataPtr(tcItem) \
	(((tcItem)->inplace != NULL) ? ((char *)((tcItem)->inplace)) : ((char *)((tcItem)->chunk_data)))

#define GetChunkDataSize(tcItem, sizep)	\
	do { uint16 sizeid; memcpy(&sizeid, (GetChunkDataPtr(tcItem)), sizeof(uint16)); *typep = typeid; } while (0)

#define GetChunkType(/* uint 8 * */tcItem, /* TupleChunkType * */typep) \
	do { uint16 typeid; memcpy(&typeid, (GetChunkDataPtr(tcItem) + 2), sizeof(uint16)); *typep = typeid; } while (0)

#define SetChunkDataSize(/* uint8 * */tc_data, /* uint16 */value) \
	do { uint16 val = (value); memcpy(tc_data, &val, sizeof(uint16)); } while (0)

#define SetChunkType(/* uint8 * */tc_data, /* TupleChunkType */value) \
	do { uint16 val = (value); memcpy((tc_data+2), &val, sizeof(uint16)); } while (0)

#endif   /* TUPCHUNK_H */
