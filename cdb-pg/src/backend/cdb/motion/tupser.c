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

#include "access/heapam.h"
#include "libpq/pqformat.h"
#include "utils/lsyscache.h"

#include "cdb/tupser.h"



/* Attribute information for sending and receiving.  This is cached, since
 * there will almost certainly be many rows processed, all with the same type.
 *
 * All values are for the binary input and output functions.
 * ----------------
 */
typedef struct
{
    Oid      typsend;           /* Oid for the type's binary output fn */
    Oid      send_typio_param;  /* param to pass to the output fn */
    FmgrInfo send_finfo;        /* Precomputed call info for output fn */

    Oid      typrecv;           /* Oid for the type's binary input fn */
    Oid      recv_typio_param;  /* param to pass to the input fn */
    FmgrInfo recv_finfo;        /* Precomputed call info for output fn */

    bool    typisvarlena;       /* is type varlena (ie possibly toastable)? */
} SerAttrInfo;

typedef struct
{
    TupleDesc   tupdesc;    /* The attr info we are set up for */
    int         nattrs;
    SerAttrInfo *myinfo;    /* Cached info about each attr */

    /* preallocated space for deformtuple */
    Datum       *values;
    char        *nulls;
} SerTupInfo;

static SerTupInfo s_serTupInfo;

/*-----------------------*/
/* Function Declarations */
/*-----------------------*/

void initSerTupInfo(SerTupInfo *myState, TupleDesc tupdesc, int numAttrs);


/*----------------------*/
/* Function Definitions */
/*----------------------*/


/* Convert a HeapTuple into a byte-sequence, and store it into the StringInfo.
 *
 * This code is based on the printtup_internal_20() function in printtup.c.
 */
int SerializeTuple(HeapTuple tuple, TupleDesc tupdesc, StringInfo strInfo)
{
    SerTupInfo *myState;
    int natts;

    int i, j, k;
    int origLen;

    myState = &s_serTupInfo;
    natts = tupdesc->natts;
    origLen = strInfo->len;

    /* Set or update my derived attribute info, if needed */
    if (myState->tupdesc != tupdesc || myState->nattrs != natts)
        initSerTupInfo(myState, tupdesc, natts);

    /* deconstruct the tuple (faster than a heap_getattr loop) */
    heap_deformtuple(tuple, tupdesc, myState->values, myState->nulls);

    /* send a bitmap of which attributes are not null */
    j = 0;
    k = 1 << 7;
    for (i = 0; i < natts; ++i)
    {
        if (myState->nulls[i] != 'n')
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
        SerAttrInfo *thisState = myState->myinfo + i;
        Datum origattr = myState->values[i], attr;
        bytea *outputbytes;

        if (myState->nulls[i] == 'n')
            continue;

        /* If we have a toasted datum, forcibly detoast it here to avoid
         * memory leakage inside the type's output routine.
         */
        if (thisState->typisvarlena)
            attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
        else
            attr = origattr;

        outputbytes = DatumGetByteaP(FunctionCall2(&thisState->send_finfo,
            attr, ObjectIdGetDatum(thisState->send_typio_param)));

        /* We assume the result will not have been toasted */
        pq_sendint(strInfo, VARSIZE(outputbytes) - VARHDRSZ, 4);
        pq_sendbytes(strInfo, VARDATA(outputbytes),
            VARSIZE(outputbytes) - VARHDRSZ);
        pfree(outputbytes);

        /* Clean up detoasted copy, if any */
        if (attr != origattr)
            pfree(DatumGetPointer(attr));
    }
    
    /* Return the size of the serialized tuple in bytes. */
    return strInfo->len - origLen;
}


/* Deserialize a HeapTuple's data from a byte-array.
 */
HeapTuple DeserializeTuple(TupleDesc tupdesc, StringInfo serialTup)
{
    SerTupInfo *myState;
    int natts;

    int i, j, k;
    int origLen;
    StringInfoData attr_data;

    myState = &s_serTupInfo;
    natts = tupdesc->natts;
    origLen = serialTup->len;

    /* Set or update my derived attribute info, if needed */
    if (myState->tupdesc != tupdesc || myState->nattrs != natts)
        initSerTupInfo(myState, tupdesc, natts);

    /* Receive and process the "non-NULL values" bitmap. */
    j = pq_getmsgint(serialTup, 1);
    k = 1 << 7;
    for (i = 0; i < natts; ++i)
    {
        /* If bit cleared, set null; otherwise, set not null. */
        if ((j & k) != 0)
            myState->nulls[i] = ' ';
        else
            myState->nulls[i] = 'n';

        k >>= 1;
        if (k == 0)             /* end of byte? */
        {
            j = pq_getmsgint(serialTup, 1);
            k = 1 << 7;
        }
    }

    /* Deserialize the non-NULL attributes of this tuple */
    initStringInfo(&attr_data);
    for (i = 0; i < natts; ++i)
    {
        SerAttrInfo *thisState = myState->myinfo + i;
        uint32 attr_size;

        if (myState->nulls[i] == 'n')
            continue;

        attr_size = pq_getmsgint(serialTup, 4);

        /* reset attr_data to empty, and load raw data into it */
        
        attr_data.len = 0;
        attr_data.data[0] = '\0';
        attr_data.cursor = 0;

        appendBinaryStringInfo(&attr_data,
            pq_getmsgbytes(serialTup, attr_size), attr_size);

        /* Call the attribute type's binary input converter. */
        myState->values[i] =  FunctionCall2(&thisState->recv_finfo,
            PointerGetDatum(&attr_data),
            ObjectIdGetDatum(thisState->recv_typio_param));

        /* Trouble if it didn't eat the whole buffer */
        if (attr_data.cursor != attr_data.len)
        {
            ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                errmsg("incorrect binary data format")));
        }
    }
    
    /* Construct the tuple from the Datums and nulls values. */
    return heap_formtuple(tupdesc, myState->values, myState->nulls);
}


/* Get the lookup info that SerializeTuple() needs */
void initSerTupInfo(SerTupInfo *myState, TupleDesc tupdesc, int numAttrs)
{
    int i;
    MemoryContext oldctxt;

    /* Allocate this stuff in the cache memory context so that it won't
     * get freed when a statement ends.
     */
    oldctxt = MemoryContextSwitchTo(CacheMemoryContext);

    /* get rid of any old data */

    if (myState->myinfo)
        pfree(myState->myinfo);
    myState->myinfo = NULL;
    if (myState->values)
        pfree(myState->values);
    myState->values = NULL;
    if (myState->nulls)
        pfree(myState->nulls);
    myState->nulls = NULL;

    myState->tupdesc = tupdesc;
    myState->nattrs = numAttrs;
    if (numAttrs <= 0)
        return;

    myState->myinfo = (SerAttrInfo *) palloc0(numAttrs * sizeof(SerAttrInfo));

    myState->values = (Datum *) palloc(numAttrs * sizeof(Datum));
    myState->nulls = (char *) palloc(numAttrs * sizeof(char));

    for (i = 0; i < numAttrs; i++)
    {
        SerAttrInfo *thisState = myState->myinfo + i;

        /* Info for sending the type. */

        getTypeBinaryOutputInfo(tupdesc->attrs[i]->atttypid,
            &thisState->typsend, &thisState->send_typio_param,
            &thisState->typisvarlena);

        fmgr_info(thisState->typsend, &thisState->send_finfo);

        /* Info for receiving the type. */

        getTypeBinaryInputInfo(tupdesc->attrs[i]->atttypid,
            &thisState->typrecv, &thisState->recv_typio_param);

        fmgr_info(thisState->typrecv, &thisState->recv_finfo);
    }

    MemoryContextSwitchTo(oldctxt);
}




