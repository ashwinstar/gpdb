/*-------------------------------------------------------------------------
 * tupser.h
 *     Functions for serializing and deserializing heap tuples.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef TUPSER_H
#define TUPSER_H


#include "access/htup.h"
#include "access/tupdesc.h"
#include "cdb/cdbmotion.h"
#include "cdb/tupchunk.h"
#include "lib/stringinfo.h"


/* Serialize a HeapTuple's data into a StringInfo. */
extern int SerializeTuple(HeapTuple tuple, TupleDesc tupdesc, StringInfo dest);

/* Deserialize a HeapTuple's data from a byte-array. */
extern HeapTuple DeserializeTuple(TupleDesc tupdesc, StringInfo serialTup);


/* Convert a HeapTuple into a sequence of chunks containing the serialized
 * tuple data.
 */
extern TupleChunkList CvtHeapTupToChunks(HeapTuple htup, TupleDesc tupdesc,
    int maxChunkDataSize, MotionNodeInfo *mn_info, uint16 tgtSegIdx);

/* Convert a sequence of chunks containing serialized tuple data into a
 * HeapTuple.
 */
extern HeapTuple CvtChunksToHeapTup(TupleChunkList tclist, TupleDesc tupdesc);

/* Create an "end of stream" tuple chunk. */
extern TupleChunkListItem MakeEndOfStreamChunk(uint16 srcSegIdx, MotionNodeInfo *mn_info);


#endif /* TUPSER_H */

