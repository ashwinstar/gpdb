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



/* ----------------
 *   Private state for a printtup destination object
 *
 * NOTE: finfo is the lookup info for either typoutput or typsend, whichever
 * we are using for this column.
 * ----------------
 */
typedef struct
{
    Oid     typoutput;      /* Oid for the type's text output fn */
    Oid     typsend;        /* Oid for the type's binary output fn */
    Oid     typioparam;     /* param to pass to the output fn */
    bool    typisvarlena;   /* is it varlena (ie possibly toastable)? */
    int16   format;         /* format code for this column */
    FmgrInfo finfo;         /* Precomputed call info for output fn */
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

static SerTupInfo *s_serAttrInfo;


void InitSerTupInfo(SerTupInfo *myState, TupleDesc tupdesc, int numAttrs);




/* Get the lookup info that SerializeTuple() needs */
void InitSerTupInfo(SerTupInfo *myState, TupleDesc tupdesc, int numAttrs)
{
    int i;

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

        getTypeBinaryOutputInfo(tupdesc->attrs[i]->atttypid,
            &thisState->typsend, &thisState->typioparam,
            &thisState->typisvarlena);

        fmgr_info(thisState->typsend, &thisState->finfo);
    }
}





/* Convert a HeapTuple into a byte-sequence, and store it into the StringInfo.
 *
 * This code was copied from printtup_internal_20 in printtup.c.
 */
int SerializeTuple(HeapTuple tuple, TupleDesc tupdesc, StringInfo strInfo)
{
    SerTupInfo *myState;
    int natts;

    int i, j, k;
    int origLen;

    myState = s_serAttrInfo;
    natts = tupdesc->natts;
    origLen = strInfo->len;

    /* Set or update my derived attribute info, if needed */
    if (myState->tupdesc != tupdesc || myState->nattrs != natts)
        InitSerTupInfo(myState, tupdesc, natts);

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

        outputbytes = DatumGetByteaP(FunctionCall2(&thisState->finfo, attr,
            ObjectIdGetDatum(thisState->typioparam)));

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
