/*-------------------------------------------------------------------------
 * tupser.h
 *     Functions for serializing and deserializing heap tuples.
 *
 * Copyright (c) 2005, Metapa
 *-------------------------------------------------------------------------
 */
#ifndef TUPSER_H
#define TUPSER_H


#include "access/heapam.h"
#include "cdb/tupchunklist.h"
#include "lib/stringinfo.h"
#include "utils/lsyscache.h"


/* Define this to pack the NULLs-mask into the minimum number of bytes
 * possible.  If undefined, the NULLs-sequence is sent as one character per
 * attribute.
 */
#undef TUPSER_BITPACK_NULLMASK

/* Define this to allocate scratch-space for varlena attribute-values, so that
 * tuple-deserialization doesn't have to allocate space if the varlena's value
 * is smaller than the scratch size.
 */
#undef TUPSER_SCRATCH_SPACE
#define VARLEN_SCRATCH_SIZE 500


/* Used to encapsulate a motion node's core identity.
 *
 * FIELDS:
 *
 *	query_group_id:  When a user connects to the CDB, a postgres process
 *			   will be started for every segment-database in the CDB.
 *			   All of these postgres processes are part of the same 
 *			   group of processes serving that user's connection and
 *			   so all will have the same globally unique 
 *			   "process_group_id" that identifies this group from
 *			   other groups within the CDB.  
 *
 *			   TODO:  This id is assigned by the AMS?
 *
 *	segment_id: The segment id for the local segment database.
 *
 *	host_id: The host id for the local segment database.
 *
 */
typedef struct MotionNodeInfo
{
	int query_group_id;
	int segment_id;
	int host_id;
    
} MotionNodeInfo;


/*
 * The next two structures are for cached tuple serialization and
 * deserialization information.  This information is cached since there will
 * typically be a LOT of tuples being moved around, and we don't want to look
 * up these details all the time.
 *
 * The cached information itself is typically kept within the Motion Layer's
 * per-motion-node storage, where it is going to be used.
 */

/* Attribute information for sending and receiving.
 *
 * All values are for the binary input and output functions.
 */
typedef struct SerAttrInfo
{
    Oid     atttypid;           /* Oid of the attribute's data-type. */
    bool    typisvarlena;       /* is type varlena (ie possibly toastable)? */
    
    Oid      typsend;           /* Oid for the type's binary output fn */
    Oid      send_typio_param;  /* param to pass to the output fn */
    FmgrInfo send_finfo;        /* Precomputed call info for output fn */

    Oid      typrecv;           /* Oid for the type's binary input fn */
    Oid      recv_typio_param;  /* param to pass to the input fn */
    FmgrInfo recv_finfo;        /* Precomputed call info for output fn */

#ifdef TUPSER_SCRATCH_SPACE
    void *pv_varlen_scratch;    /* For deserializing varlena attributes. */
    int varlen_scratch_size;    /* Size of varlena scratch space. */
#endif
} SerAttrInfo;


/* The information for sending and receiving tuples that match a particular
 * description.
 */
typedef struct SerTupInfo
{
    TupleDesc   tupdesc;    /* The attr info we are set up for */
    
    SerAttrInfo *myinfo;    /* Cached info about each attr */

    /* Preallocated space for deformtuple and formtuple. */
    Datum       *values;
    char        *nulls;
} SerTupInfo;



/* Populate a SerTupInfo struct with information looked up from the specified
 * tuple-descriptor.
 */
extern void InitSerTupInfo(TupleDesc tupdesc, SerTupInfo *pSerInfo);

/* Free up storage in a previously initialized SerTupInfo struct. */
extern void CleanupSerTupInfo(SerTupInfo *pSerInfo);



/* Serialize a HeapTuple's data into a StringInfo. */
extern int SerializeTuple(HeapTuple tuple, SerTupInfo *pSerInfo, StringInfo dest);

/* Deserialize a HeapTuple's data from a byte-array. */
extern HeapTuple DeserializeTuple(SerTupInfo *pSerInfo, StringInfo serialTup);


/* Convert a HeapTuple into a sequence of chunks containing the serialized
 * tuple data.
 */
extern TupleChunkList CvtHeapTupToChunks(HeapTuple htup, SerTupInfo *pSerInfo,
    int maxChunkDataSize, uint16 motNodeID, MotionNodeInfo *mn_info,
    int16 tgtSegIdx);

/* Convert a sequence of chunks containing serialized tuple data into a
 * HeapTuple.
 */
extern HeapTuple CvtChunksToHeapTup(TupleChunkList tclist, SerTupInfo *pSerInfo);

/* Create an "end of stream" tuple chunk. */
extern TupleChunkListItem MakeEndOfStreamChunk(int16 srcSegIdx,
    MotionNodeInfo *mn_info);


#endif /* TUPSER_H */

