/*-------------------------------------------------------------------------
 * tupser.h
 *     Functions for serializing and deserializing a heap tuple.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef TUPSER_H
#define TUPSER_H


#include "access/htup.h"
#include "access/tupdesc.h"
#include "lib/stringinfo.h"


/* Serialize a HeapTuple's data into a StringInfo. */
extern int SerializeTuple(HeapTuple tuple, TupleDesc typeinfo, StringInfo dest);

/* Deserialize a HeapTuple's data from a byte-array. */
extern HeapTuple DeserializeTuple(TupleDesc typeinfo, uint8 *src, uint32 srcLen);


#endif /* TUPSER_H */

