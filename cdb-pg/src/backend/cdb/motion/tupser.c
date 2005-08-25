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



static void appendStringInfoInt32(StringInfo str, int32 i);
static int32 stringInfoGetInt32(StringInfo str);



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
		
#ifdef TUPSER_SCRATCH_SPACE
		/* If the field is a varlena, allocate some space to use for
		 * deserializing it.  If most of the values are smaller than this
		 * scratch-space then we save time on allocation and freeing.
		 */
		attrInfo->pv_varlen_scratch = palloc(VARLEN_SCRATCH_SIZE);
		attrInfo->varlen_scratch_size = VARLEN_SCRATCH_SIZE;
#endif
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

    int i;
    int origLen;
	bool fHandled;
	
	AssertArg(tuple != NULL);
	AssertArg(pSerInfo != NULL);
	AssertArg(strInfo != NULL);
	
	tupdesc = pSerInfo->tupdesc;
    natts = tupdesc->natts;
    origLen = strInfo->len;

	/* Flip to our tuple-serialization memory-context, to speed up memory
	 * reclamation operations.
	 */
	AssertState(s_tupSerMemCtxt != NULL);
	oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);

    /* deconstruct the tuple (faster than a heap_getattr loop) */
    heap_deformtuple(tuple, tupdesc, pSerInfo->values, pSerInfo->nulls);

	/* Send the nulls character-array. */
	appendBinaryStringInfo(strInfo, pSerInfo->nulls, natts);

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

		/* Assume that the data's output will be handled by the special IO
		 * code, and if not then we can handle it the slow way.
		 */
		fHandled = true;
		switch (attrInfo->atttypid)
		{
		case INT4OID:
			appendStringInfoInt32(strInfo, DatumGetInt32(attr));
			break;
			
		case CHAROID:
			appendStringInfoCharMacro(strInfo, DatumGetChar(attr));
			break;
			
		case BPCHAROID:
		case VARCHAROID:
		{
			text *pText = DatumGetTextP(attr);
			int32 textSize = VARSIZE(pText) - VARHDRSZ;
			appendStringInfoInt32(strInfo, textSize);
			appendBinaryStringInfo(strInfo, (char *) VARDATA(pText), textSize);
			break;
		}
		
		case DATEOID:
		{
			DateADT date = DatumGetDateADT(attr);
			appendBinaryStringInfo(strInfo, (char *) &date, sizeof(DateADT));
			break;
		}
		
		case NUMERICOID:
		{
			/* Treat the numeric as a varlena variable, and just push the
			 * whole shebang to the output-buffer.  We don't care about the
			 * guts of the numeric.
			 */
			Numeric num = DatumGetNumeric(attr);
			int32 numSize = VARSIZE(num) - VARHDRSZ;
			appendStringInfoInt32(strInfo, numSize);
			appendBinaryStringInfo(strInfo, (char *) VARDATA(num), numSize);
			break;
		}
		
		default:
			fHandled = false;
		}
		
		if (fHandled)
			continue;

        outputbytes = DatumGetByteaP(FunctionCall2(&attrInfo->send_finfo,
            attr, ObjectIdGetDatum(attrInfo->send_typio_param)));

        /* We assume the result will not have been toasted */
        appendStringInfoInt32(strInfo, VARSIZE(outputbytes) - VARHDRSZ);
        appendBinaryStringInfo(strInfo, VARDATA(outputbytes),
            VARSIZE(outputbytes) - VARHDRSZ);
    }

	MemoryContextSwitchTo(oldCtxt);
	
	MemoryContextReset(s_tupSerMemCtxt);

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

    int i;
    int origLen;
    StringInfoData attr_data;
	bool fHandled;

	AssertArg(pSerInfo != NULL);
	AssertArg(serialTup != NULL);
	
    tupdesc = pSerInfo->tupdesc;
    natts = tupdesc->natts;
    origLen = serialTup->len;

	/* Flip to our tuple-serialization memory-context, to speed up memory
	 * reclamation operations.
	 */
	AssertState(s_tupSerMemCtxt != NULL);
	oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);
	
	/* Receive nulls character-array. */
	pq_copymsgbytes(serialTup, pSerInfo->nulls, natts);

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

		/* Assume that the data's output will be handled by the special IO
		 * code, and if not then we can handle it the slow way.
		 */
		fHandled = true;
		switch (attrInfo->atttypid)
		{
		case INT4OID:
			pSerInfo->values[i] = Int32GetDatum(stringInfoGetInt32(serialTup));
			break;
		
		case CHAROID:
			pSerInfo->values[i] = CharGetDatum(pq_getmsgbyte(serialTup));
			break;
			
		case BPCHAROID:
		case VARCHAROID:
		{
			text *pText;
			int textSize;
			
			textSize = stringInfoGetInt32(serialTup);
			
#ifdef TUPSER_SCRATCH_SPACE
			if (textSize + VARHDRSZ <= attrInfo->varlen_scratch_size)
				pText = (text *) attrInfo->pv_varlen_scratch;
			else
				pText = (text *) palloc(textSize + VARHDRSZ);
#else
			pText = (text *) palloc(textSize + VARHDRSZ);
#endif
			
			VARATT_SIZEP(pText) = textSize + VARHDRSZ;
			pq_copymsgbytes(serialTup, VARDATA(pText), textSize);
			pSerInfo->values[i] = PointerGetDatum(pText);
			break;
		}
		
		case DATEOID:
		{
			/* TODO:  I would LIKE to do something more efficient, but DateADT
			 *        is not strictly limited to 4 bytes by its definition.
			 */
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
			
			numSize = stringInfoGetInt32(serialTup);
			
#ifdef TUPSER_SCRATCH_SPACE
			if (numSize + VARHDRSZ <= attrInfo->varlen_scratch_size)
				num = (Numeric) attrInfo->pv_varlen_scratch;
			else
				num = (Numeric) palloc(numSize + VARHDRSZ);
#else
			num = (Numeric) palloc(numSize + VARHDRSZ);
#endif
			
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
		
		attr_size = stringInfoGetInt32(serialTup);

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

    /* Construct the tuple from the Datums and nulls values.  NOTE:  Switch
	 * out of our temporary context before we form the tuple!
	 */
	
	MemoryContextSwitchTo(oldCtxt);

    htup = heap_formtuple(tupdesc, pSerInfo->values, pSerInfo->nulls);

	MemoryContextReset(s_tupSerMemCtxt);

	/* All done.  Return the result. */
	return htup;
}



TupleChunkList CvtHeapTupToChunks(HeapTuple htup, SerTupInfo *pSerInfo,
    int maxChunkDataSize)
{
    StringInfoData serialTup;
    TupleChunkList tcList;
    TupleChunkListItem tcItem;
    int i;
    uint16 chunkDataSize;
    TupleChunkType tcType;

    AssertArg(htup != NULL);
    AssertArg(pSerInfo != NULL);

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
            chunkDataSize);

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
                chunkDataSize );

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
            (const char *)(tcItem->chunk_data + TUPLE_CHUNK_HEADER_SIZE),
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


/*
 * appendStringInfoInt32
 *
 * Append a 4-byte integer to str.  The result is not necessarily in network
 * byte-ordering, and is architecture dependent.
 *
 * Hopefully this is faster than the stuff in stringinfo.c or pqformat.c!
 */
void appendStringInfoInt32(StringInfo str, int32 i)
{
	/* Make more room if needed */
	if (str->len + 4 >= str->maxlen)
		enlargeStringInfo(str, 4);

	/* Store the int32.  We cast the pointer to the next part of the data to
	 * an int32 pointer, and just store the integer into that location of the
	 * array.
	 */
	*((int32 *) (str->data + str->len)) = i;
	
	/* Update the count, and 0-terminate the string. */
	str->len += 4;
	str->data[str->len] = '\0';
}


/*
 * stringInfoGetInt32
 *
 * Pull a 4-byte integer out of str, at the current cursor location.  The
 * cursor is then incremented by 4.  If there is not 4 bytes left between the
 * cursor and the end of the string, an error is reported.
 *
 * The input is expected to be in the current architecture's byte-ordering.
 *
 * Hopefully this is faster than the stuff in stringinfo.c or pqformat.c!
 */
int32 stringInfoGetInt32(StringInfo str)
{
	int32 res;
	
	/* Make sure there are enough bytes left. */
	if (str->len - str->cursor < 4)
	{
		ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
			errmsg("insufficient data left in message")));
	}

	/* Pull out the int32.  We cast the pointer to the next part of the data
	 * to an int32 pointer, and just retrieve the integer from that location
	 * in the array.
	 */
	res = *((int32 *) (str->data + str->cursor));
	
	/* Update the cursor. */
	str->cursor += 4;
	
	return res;
}



