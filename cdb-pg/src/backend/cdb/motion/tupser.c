/*-------------------------------------------------------------------------
 * tupser.c
 *     Functions for serializing and deserializing a heap tuple.
 *
 * Copyright (c) 2005, Metapa
 *
 * IDENTIFICATION:
 *     $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "cdb/tupser.h"
#include "libpq/pqformat.h"
#include "utils/date.h"
#include "utils/numeric.h"


/* A MemoryContext used within the tuple serialize code, so that freeing of
 * space is SUPAFAST.  It is initialized in the first call to InitSerTupInfo()
 * since that must be called before any tuple serialization or deserialization
 * work can be done.
 */
static MemoryContext s_tupSerMemCtxt = NULL;


/* Look up all of the information that SerializeTuple() and DeserializeTuple()
 * need to perform their jobs quickly.  Also, scratchpad space is allocated
 * for serialization and desrialization of datum values, and for formation/
 * deformation of tuples themselves.
 *
 * NOTE:  This function allocates various data-structures, but it assumes that
 *        the current memory-context is acceptable.  So the caller should set
 *        the desired memory-context before calling this function.
 */
void InitSerTupInfo(TupleDesc tupdesc, SerTupInfo *pSerInfo)
{
    int i, numAttrs;

	AssertArg(tupdesc != NULL);
	AssertArg(pSerInfo != NULL);
	
	if (s_tupSerMemCtxt == NULL)
	{
		/* Create tuple-serialization memory context. */
		
		s_tupSerMemCtxt = AllocSetContextCreate(TopMemoryContext,
			"TupSerMemCtxt", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE,
			ALLOCSET_SMALL_MAXSIZE);
	}
	
    elog(DEBUG2, "Initializing cached tuple serialization information.");

	/* Set contents to all 0, just to make things clean and easy. */
	bzero(pSerInfo, sizeof(SerTupInfo));
	
	/* Store the tuple-descriptor so we can use it later. */
    pSerInfo->tupdesc = tupdesc;
	
	/* If we have some attributes, go ahead and prepare the information for
	 * each attribute in the descriptor.  Otherwise, we can return right away.
	 */
	
	numAttrs = tupdesc->natts;
    if (numAttrs <= 0)
        return;
	
    pSerInfo->myinfo = (SerAttrInfo *) palloc0(numAttrs * sizeof(SerAttrInfo));

    pSerInfo->values = (Datum *) palloc(numAttrs * sizeof(Datum));
    pSerInfo->nulls  = (char  *) palloc(numAttrs * sizeof(char ));

    for (i = 0; i < numAttrs; i++)
    {
        SerAttrInfo *attrInfo = pSerInfo->myinfo + i;

		/* Get attribute's data-type Oid.  This lets us shortcut the comm
		 * operations for some attribute-types.
		 */
		attrInfo->atttypid = tupdesc->attrs[i]->atttypid;
		
        /* Info for sending the type. */

        getTypeBinaryOutputInfo(attrInfo->atttypid, &attrInfo->typsend,
			&attrInfo->send_typio_param, &attrInfo->typisvarlena);

        fmgr_info(attrInfo->typsend, &attrInfo->send_finfo);

        /* Info for receiving the type. */

        getTypeBinaryInputInfo(attrInfo->atttypid, &attrInfo->typrecv,
			&attrInfo->recv_typio_param);

        fmgr_info(attrInfo->typrecv, &attrInfo->recv_finfo);
    }
}


/* Free up storage in a previously initialized SerTupInfo struct. */
void CleanupSerTupInfo(SerTupInfo *pSerInfo)
{
	AssertArg(pSerInfo != NULL);
	
    /*
	 * Free any old data.
	 *
	 * NOTE:  This works because data-structure was bzero()ed in init call.
	 */
	
    if (pSerInfo->myinfo != NULL)
        pfree(pSerInfo->myinfo);
    pSerInfo->myinfo = NULL;
	
    if (pSerInfo->values != NULL)
        pfree(pSerInfo->values);
    pSerInfo->values = NULL;
    
	if (pSerInfo->nulls != NULL)
        pfree(pSerInfo->nulls);
    pSerInfo->nulls = NULL;
	
	pSerInfo->tupdesc = NULL;
}


/* Convert a HeapTuple into a byte-sequence, and store it into the StringInfo.
 *
 * This code is based on the printtup_internal_20() function in printtup.c.
 */
int SerializeTuple(HeapTuple tuple, SerTupInfo *pSerInfo, StringInfo strInfo)
{
	MemoryContext oldCtxt;
	TupleDesc tupdesc;
    int natts;

    int i, j, k;
    int origLen;

	AssertArg(tuple != NULL);
	AssertArg(pSerInfo != NULL);
	AssertArg(strInfo != NULL);
	
	tupdesc = pSerInfo->tupdesc;
    natts = tupdesc->natts;
    origLen = strInfo->len;

#ifdef USE_TUPSER_MEM_CTXT
	/* Flip to our tuple-serialization memory-context, to speed up memory
	 * reclamation operations.
	 */
	AssertState(s_tupSerMemCtxt != NULL);
	oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);
#endif

    /* deconstruct the tuple (faster than a heap_getattr loop) */
    heap_deformtuple(tuple, tupdesc, pSerInfo->values, pSerInfo->nulls);

    /* send a bitmap of which attributes are not null */
    j = 0;
    k = 1 << 7;
    for (i = 0; i < natts; ++i)
    {
        if (pSerInfo->nulls[i] != 'n')
            j |= k;             /* set bit if not null */
        k >>= 1;
        if (k == 0)             /* end of byte? */
        {
            pq_sendint(strInfo, j, 1);
            j = 0;
            k = 1 << 7;
        }
    }
    if (k != (1 << 7))          /* flush last partial byte */
        pq_sendint(strInfo, j, 1);

    /* send the attributes of this tuple */
    for (i = 0; i < natts; ++i)
    {
        SerAttrInfo *attrInfo = pSerInfo->myinfo + i;
        Datum origattr = pSerInfo->values[i], attr;
        bytea *outputbytes;

        if (pSerInfo->nulls[i] == 'n')
            continue;

        /* If we have a toasted datum, forcibly detoast it here to avoid
         * memory leakage inside the type's output routine.
         */
        if (attrInfo->typisvarlena)
            attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
        else
            attr = origattr;

#ifdef SPECIAL_TUPSER_IO
		bool fHandled = true;
		switch (attrInfo->atttypid)
		{
		case INT4OID:
		{
			int32 tmpI32 = DatumGetInt32(attr);
			pq_sendbytes(strInfo, (char *) &tmpI32, 4);
			break;
		}
			
		case CHAROID:
		{
			char tmpCh = DatumGetChar(attr);
			pq_sendbyte(strInfo, tmpCh);
			break;
		}
			
		case BPCHAROID:
		case VARCHAROID:
		{
			text *pText = DatumGetTextP(attr);
			int textSize = VARSIZE(pText) - VARHDRSZ;
			pq_sendbytes(strInfo, (char *) &textSize, 4);
			pq_sendbytes(strInfo, (char *) VARDATA(pText), textSize);
			break;
		}
		
		case DATEOID:
		{
			DateADT date = DatumGetDateADT(attr);
			pq_sendbytes(strInfo, (char *) &date, sizeof(DateADT));
			break;
		}
		
		case NUMERICOID:
		{
			/* Treat the numeric as a varlena variable, and just push the
			 * whole shebang to the output-buffer.  We don't care about the
			 * guts of the numeric.
			 */
			Numeric num = DatumGetNumeric(attr);
			int numSize = VARSIZE(num) - VARHDRSZ;
			pq_sendbytes(strInfo, (char *) &numSize, 4);
			pq_sendbytes(strInfo, (char *) VARDATA(num), numSize);
			break;
		}
		
		default:
			fHandled = false;
		}
		
		if (fHandled)
			continue;
#endif

        outputbytes = DatumGetByteaP(FunctionCall2(&attrInfo->send_finfo,
            attr, ObjectIdGetDatum(attrInfo->send_typio_param)));

        /* We assume the result will not have been toasted */
        pq_sendint(strInfo, VARSIZE(outputbytes) - VARHDRSZ, 4);
        pq_sendbytes(strInfo, VARDATA(outputbytes),
            VARSIZE(outputbytes) - VARHDRSZ);

#ifndef USE_TUPSER_MEM_CTXT
        pfree(outputbytes);

        /* Clean up detoasted copy, if any */
        if (attr != origattr)
            pfree(DatumGetPointer(attr));
#endif
    }

#ifdef USE_TUPSER_MEM_CTXT
	MemoryContextSwitchTo(oldCtxt);
	
	MemoryContextReset(s_tupSerMemCtxt);
#endif

    /* Return the size of the serialized tuple in bytes. */
    return strInfo->len - origLen;
}


/* Deserialize a HeapTuple's data from a byte-array.
 *
 * This code is based on the binary input handling functions in copy.c.
 */
HeapTuple DeserializeTuple(SerTupInfo *pSerInfo, StringInfo serialTup)
{
	MemoryContext oldCtxt;
	TupleDesc tupdesc;
	HeapTuple htup;
    int natts;
	SerAttrInfo *attrInfo;
	uint32 attr_size;

    int i, j, k;
    int origLen;
    StringInfoData attr_data;

	AssertArg(pSerInfo != NULL);
	AssertArg(serialTup != NULL);
	
    tupdesc = pSerInfo->tupdesc;
    natts = tupdesc->natts;
    origLen = serialTup->len;

#ifdef USE_TUPSER_MEM_CTXT
	/* Flip to our tuple-serialization memory-context, to speed up memory
	 * reclamation operations.
	 */
	AssertState(s_tupSerMemCtxt != NULL);
	oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);
#endif
	
    /* Receive and process the "non-NULL values" bitmap. */
    for (i = 0, k = 0; i < natts; ++i)
    {
        if (k == 0)
        {
            /* Get the next byte in the bitmap. */
            j = pq_getmsgint(serialTup, 1);
            k = 1 << 7;
        }
        
        /* If bit cleared, set null; otherwise, set not null. */
        if ((j & k) != 0)
            pSerInfo->nulls[i] = ' ';
        else
            pSerInfo->nulls[i] = 'n';
        
        k >>= 1;
    }

    /* Deserialize the non-NULL attributes of this tuple */
    initStringInfo(&attr_data);
    for (i = 0; i < natts; ++i)
    {
        attrInfo = pSerInfo->myinfo + i;

        if (pSerInfo->nulls[i] == 'n')       /* NULL field. */
        {
            pSerInfo->values[i] = (Datum) 0;
			continue;
        }

#ifdef SPECIAL_TUPSER_IO
		bool fHandled = true;
		switch (attrInfo->atttypid)
		{
		case INT4OID:
		{
			int32 tmpI32;
			pq_copymsgbytes(serialTup, (char *) &tmpI32, 4);
			pSerInfo->values[i] = Int32GetDatum(tmpI32);
			break;
		}
		
		case CHAROID:
			pSerInfo->values[i] = CharGetDatum(pq_getmsgbyte(serialTup));
			break;
			
		case BPCHAROID:
		case VARCHAROID:
		{
			text *pText;
			int textSize;
			pq_copymsgbytes(serialTup, (char *) &textSize, 4);
			pText = (text *) palloc(textSize + VARHDRSZ);
			VARATT_SIZEP(pText) = textSize + VARHDRSZ;
			pq_copymsgbytes(serialTup, VARDATA(pText), textSize);
			pSerInfo->values[i] = PointerGetDatum(pText);
			break;
		}
		
		case DATEOID:
		{
			DateADT date;
			pq_copymsgbytes(serialTup, (char *) &date, sizeof(DateADT));
			pSerInfo->values[i] = DateADTGetDatum(date);
			break;
		}
		
		case NUMERICOID:
		{
			/* Treat the numeric as a varlena variable, and just push the
			 * whole shebang to the output-buffer.  We don't care about the
			 * guts of the numeric.
			 */
			Numeric num;
			int numSize;
			pq_copymsgbytes(serialTup, (char *) &numSize, 4);
			num = (Numeric) palloc(numSize + VARHDRSZ);
			VARATT_SIZEP(num) = numSize + VARHDRSZ;
			pq_copymsgbytes(serialTup, VARDATA(num), numSize);
			pSerInfo->values[i] = NumericGetDatum(num);
			break;
		}
		
		default:
			fHandled = false;
		}
		
		if (fHandled)
			continue;
#endif
		
		attr_size = pq_getmsgint(serialTup, 4);

		/* reset attr_data to empty, and load raw data into it */

		attr_data.len = 0;
		attr_data.data[0] = '\0';
		attr_data.cursor = 0;

		appendBinaryStringInfo(&attr_data,
			pq_getmsgbytes(serialTup, attr_size), attr_size);

		/* Call the attribute type's binary input converter. */
		pSerInfo->values[i] =  FunctionCall2(&attrInfo->recv_finfo,
			PointerGetDatum(&attr_data),
			ObjectIdGetDatum(attrInfo->recv_typio_param));

		/* Trouble if it didn't eat the whole buffer */
		if (attr_data.cursor != attr_data.len)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				errmsg("incorrect binary data format")));
		}
    }

#ifndef USE_TUPSER_MEM_CTXT
    /* Free up memory we used. */
    pfree(attr_data.data);
#endif

    /* Construct the tuple from the Datums and nulls values.  NOTE:  Switch
	 * out of our temporary context before we form the tuple!
	 */
	
#ifdef USE_TUPSER_MEM_CTXT
	MemoryContextSwitchTo(oldCtxt);
#endif

    htup = heap_formtuple(tupdesc, pSerInfo->values, pSerInfo->nulls);

#ifndef USE_TUPSER_MEM_CTXT
	/* Clean up any variable-length attribute values that were allocated! */
	
	for (i = 0; i < natts; ++i)
	{
        attrInfo = pSerInfo->myinfo + i;
		
		if (attrInfo->typisvarlena && pSerInfo->values[i] != NULL)
		{
			pfree(DatumGetPointer(pSerInfo->values[i]));
			pSerInfo->values[i] = (Datum) NULL;
		}
	}
#else
	MemoryContextReset(s_tupSerMemCtxt);
#endif

	/* All done.  Return the result. */
	return htup;
}



TupleChunkList CvtHeapTupToChunks(HeapTuple htup, SerTupInfo *pSerInfo,
    int maxChunkDataSize, uint16 motNodeID, MotionNodeInfo *mn_info,
    int16 tgtSegIdx)
{
    StringInfoData serialTup;
    TupleChunkList tcList;
    TupleChunkListItem tcItem;
    int i;
    uint16 chunkDataSize;
    TupleChunkType tcType;

    AssertArg(htup != NULL);
    AssertArg(pSerInfo != NULL);
    AssertArg(maxChunkDataSize >= MIN_TC_DATA_SIZE);
    AssertArg(maxChunkDataSize <= MAX_TC_DATA_SIZE);
    AssertArg(mn_info != NULL);

    /* Generate serialized version of heap tuple into the string info. */

    initStringInfo(&serialTup);
    SerializeTuple(htup, pSerInfo, &serialTup);

    /* Create the tuple-chunk list from the serialized data. */

    tcList = (TupleChunkList) palloc0(sizeof(TupleChunkListData));
    tcList->serialized_data_length = serialTup.len;

    if (serialTup.len <= maxChunkDataSize)   /* Tuple fits in single chunk. */
    {
        /* Create the new tuple chunk. */
        chunkDataSize = serialTup.len;
        tcItem = makeTupleChunkListItem(TC_WHOLE, serialTup.data,
            chunkDataSize, mn_info->segment_id, tgtSegIdx, motNodeID);

        /* Update the chunk list. */
        appendChunkToTCList(tcList, tcItem);
        Assert(tcList->num_chunks == 1);
    }
    else
    {
        /* Tuple requires at least two chunks. */
        
        for (i = 0; i < serialTup.len; /*nothing*/ )
        {
            /* Calculate the size of the next chunk of data, and create the
             * new tuple chunk.
             */

            chunkDataSize = serialTup.len - i;
            if (chunkDataSize > maxChunkDataSize)
            {
                chunkDataSize = maxChunkDataSize;
                tcType = (i == 0 ? TC_PARTIAL_START : TC_PARTIAL_MID);
            }
            else
            {
                /* Sanity check:  If we get here, it's because the tuple data
                 * won't fit in a single chunk.  So we shouldn't get in here
                 * on the first iteration of the loop.
                 */
                Assert(i > 0);
                
                /* Since we finally got to the point where the whole rest of
                 * the tuple fits in a chunk, this is the end chunk for the
                 * tuple.
                 */
                tcType = TC_PARTIAL_END;
            }

            tcItem = makeTupleChunkListItem(tcType, serialTup.data + i,
                chunkDataSize, mn_info->segment_id, tgtSegIdx, motNodeID);

            /* Update the chunk list. */
            appendChunkToTCList(tcList, tcItem);

            /* Finally, increment i! */
            i += chunkDataSize;
        }
    }
    
    /* Free up memory we used. */
    pfree(serialTup.data);
    
    return tcList;
}


HeapTuple CvtChunksToHeapTup(TupleChunkList tcList, SerTupInfo *pSerInfo)
{
    StringInfoData serData;
    TupleChunkListItem tcItem;
    int i;
    HeapTuple htup;
    
    AssertArg(tcList != NULL);
    AssertArg(tcList->p_first != NULL);
    AssertArg(pSerInfo != NULL);
    
    /* Dump all of the data in the tuple chunk list into a single StringInfo,
     * so that we can convert it into a HeapTuple.  Check chunk types based on
     * whether there is only one chunk, or multiple chunks.
     */

    initStringInfo(&serData);
    tcItem = tcList->p_first;
    i = 0;
    do
    {
        /* Make sure that the type of this tuple chunk is correct! */

        if (i == 0)
        {
            if (tcItem->p_next == NULL)
            {
                if (GetChunkType(tcItem->chunk_data) != TC_WHOLE)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Single chunk's type must be TC_WHOLE.")));
                }
            }
            else    /* tcItem->p_next != NULL */
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_START)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("First chunk of collection must have type"
                               " TC_PARTIAL_START.")));
                }
            }
        }
        else    /* i > 0 */
        {
            if (tcItem->p_next == NULL)
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_END)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Last chunk of collection must have type"
                               " TC_PARTIAL_END.")));
                }
            }
            else    /* tcItem->p_next != NULL */
            {
                if (GetChunkType(tcItem->chunk_data) != TC_PARTIAL_MID)
                {
                    ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("Last chunk of collection must have type"
                               " TC_PARTIAL_MID.")));
                }
            }
        }

        /* Copy this chunk into the tuple data.  Don't include the header! */
        appendBinaryStringInfo(&serData,
            tcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE,
            tcItem->chunk_length - TUPLE_CHUNK_HEADER_SIZE);

        /* Go to the next chunk. */
        tcItem = tcItem->p_next;
        i++;
    }
    while (tcItem != NULL);
    
    /* Create the HeapTuple from the concatenated chunks. */
    htup = DeserializeTuple(pSerInfo, &serData);

    /* Free up memory we used. */
    pfree(serData.data);
    
    return htup;
}

