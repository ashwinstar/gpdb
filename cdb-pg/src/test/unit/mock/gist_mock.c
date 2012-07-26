/*
 * Auto-generated Mocking Source
 * 
 * Source: access/gist.h, access/gist_private.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/genam.h"
#include "access/gist_private.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "cdb/cdbfilerepprimary.h"

const XLogRecPtr XLogRecPtrForTemp;
Datum 
gistbuild(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
gistinsert(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

MemoryContext 
createTempGistContext(void)
{



    return (MemoryContext) mock();

}

void 
initGISTstate(GISTSTATE * giststate, Relation index)
{

    check_expected(giststate);
    check_expected(index);
    optional_assignment(giststate);
    mock();

}

void 
freeGISTstate(GISTSTATE * giststate)
{

    check_expected(giststate);
    optional_assignment(giststate);
    mock();

}

void 
gistmakedeal(GISTInsertState * state, GISTSTATE * giststate)
{

    check_expected(state);
    check_expected(giststate);
    optional_assignment(state);
    optional_assignment(giststate);
    mock();

}

void 
gistnewroot(Relation r, Buffer buffer, IndexTuple * itup, int len, ItemPointer key)
{

    check_expected(r);
    check_expected(buffer);
    check_expected(itup);
    check_expected(len);
    check_expected(key);
    optional_assignment(itup);
    mock();

}

SplitedPageLayout *
gistSplit(Relation r, Page page, IndexTuple * itup, int len, GISTSTATE * giststate)
{

    check_expected(r);
    check_expected(page);
    check_expected(itup);
    check_expected(len);
    check_expected(giststate);
    optional_assignment(itup);
    optional_assignment(giststate);
    return (SplitedPageLayout *) mock();

}

GISTInsertStack *
gistFindPath(Relation r, BlockNumber child)
{

    check_expected(r);
    check_expected(child);

    return (GISTInsertStack *) mock();

}

Datum 
gistgettuple(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
gistgetmulti(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
gistoptions(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

bool 
gistfitpage(IndexTuple * itvec, int len)
{

    check_expected(itvec);
    check_expected(len);
    optional_assignment(itvec);
    return (bool) mock();

}

bool 
gistnospace(Page page, IndexTuple * itvec, int len, OffsetNumber todelete, Size freespace)
{

    check_expected(page);
    check_expected(itvec);
    check_expected(len);
    check_expected(todelete);
    check_expected(freespace);
    optional_assignment(itvec);
    return (bool) mock();

}

void 
gistcheckpage(Relation rel, Buffer buf)
{

    check_expected(rel);
    check_expected(buf);

    mock();

}

Buffer 
gistNewBuffer(Relation r)
{

    check_expected(r);

    return (Buffer) mock();

}

OffsetNumber 
gistfillbuffer(Relation r, Page page, IndexTuple * itup, int len, OffsetNumber off)
{

    check_expected(r);
    check_expected(page);
    check_expected(itup);
    check_expected(len);
    check_expected(off);
    optional_assignment(itup);
    return (OffsetNumber) mock();

}

IndexTuple     *
gistextractpage(Page page, int *len)
{

    check_expected(page);
    check_expected(len);
    optional_assignment(len);
    return (IndexTuple *) mock();

}

IndexTuple     *
gistjoinvector(IndexTuple * itvec, int *len, IndexTuple * additvec, int addlen)
{

    check_expected(itvec);
    check_expected(len);
    check_expected(additvec);
    check_expected(addlen);
    optional_assignment(itvec);
    optional_assignment(len);
    optional_assignment(additvec);
    return (IndexTuple *) mock();

}

IndexTupleData *
gistfillitupvec(IndexTuple * vec, int veclen, int *memlen)
{

    check_expected(vec);
    check_expected(veclen);
    check_expected(memlen);
    optional_assignment(vec);
    optional_assignment(memlen);
    return (IndexTupleData *) mock();

}

IndexTuple 
gistunion(Relation r, IndexTuple * itvec, int len, GISTSTATE * giststate)
{

    check_expected(r);
    check_expected(itvec);
    check_expected(len);
    check_expected(giststate);
    optional_assignment(itvec);
    optional_assignment(giststate);
    return (IndexTuple) mock();

}

IndexTuple 
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE * giststate)
{

    check_expected(r);
    check_expected(oldtup);
    check_expected(addtup);
    check_expected(giststate);
    optional_assignment(giststate);
    return (IndexTuple) mock();

}

IndexTuple 
gistFormTuple(GISTSTATE * giststate, Relation r, Datum * attdata, bool * isnull, bool newValues)
{

    check_expected(giststate);
    check_expected(r);
    check_expected(attdata);
    check_expected(isnull);
    check_expected(newValues);
    optional_assignment(giststate);
    optional_assignment(attdata);
    optional_assignment(isnull);
    return (IndexTuple) mock();

}

OffsetNumber 
gistchoose(Relation r, Page p, IndexTuple it, GISTSTATE * giststate)
{

    check_expected(r);
    check_expected(p);
    check_expected(it);
    check_expected(giststate);
    optional_assignment(giststate);
    return (OffsetNumber) mock();

}

void 
gistcentryinit(GISTSTATE * giststate, int nkey, GISTENTRY * e, Datum k, Relation r, Page pg, OffsetNumber o, bool l, bool isNull)
{

    check_expected(giststate);
    check_expected(nkey);
    check_expected(e);
    check_expected(k);
    check_expected(r);
    check_expected(pg);
    check_expected(o);
    check_expected(l);
    check_expected(isNull);
    optional_assignment(giststate);
    optional_assignment(e);
    mock();

}

void 
GISTInitBuffer(Buffer b, uint32 f)
{

    check_expected(b);
    check_expected(f);

    mock();

}

void 
gistdentryinit(GISTSTATE * giststate, int nkey, GISTENTRY * e, Datum k, Relation r, Page pg, OffsetNumber o, bool l, bool isNull)
{

    check_expected(giststate);
    check_expected(nkey);
    check_expected(e);
    check_expected(k);
    check_expected(r);
    check_expected(pg);
    check_expected(o);
    check_expected(l);
    check_expected(isNull);
    optional_assignment(giststate);
    optional_assignment(e);
    mock();

}

float 
gistpenalty(GISTSTATE * giststate, int attno, GISTENTRY * key1, bool isNull1, GISTENTRY * key2, bool isNull2)
{

    check_expected(giststate);
    check_expected(attno);
    check_expected(key1);
    check_expected(isNull1);
    check_expected(key2);
    check_expected(isNull2);
    optional_assignment(giststate);
    optional_assignment(key1);
    optional_assignment(key2);
    return (float) mock();

}

bool 
gistMakeUnionItVec(GISTSTATE * giststate, IndexTuple * itvec, int len, int startkey, Datum * attr, bool * isnull)
{

    check_expected(giststate);
    check_expected(itvec);
    check_expected(len);
    check_expected(startkey);
    check_expected(attr);
    check_expected(isnull);
    optional_assignment(giststate);
    optional_assignment(itvec);
    optional_assignment(attr);
    optional_assignment(isnull);
    return (bool) mock();

}

bool 
gistKeyIsEQ(GISTSTATE * giststate, int attno, Datum a, Datum b)
{

    check_expected(giststate);
    check_expected(attno);
    check_expected(a);
    check_expected(b);
    optional_assignment(giststate);
    return (bool) mock();

}

void 
gistDeCompressAtt(GISTSTATE * giststate, Relation r, IndexTuple tuple, Page p, OffsetNumber o, GISTENTRY * attdata, bool * isnull)
{

    check_expected(giststate);
    check_expected(r);
    check_expected(tuple);
    check_expected(p);
    check_expected(o);
    check_expected(attdata);
    check_expected(isnull);
    optional_assignment(giststate);
    optional_assignment(attdata);
    optional_assignment(isnull);
    mock();

}

void 
gistMakeUnionKey(GISTSTATE * giststate, int attno, GISTENTRY * entry1, bool isnull1, GISTENTRY * entry2, bool isnull2, Datum * dst, bool * dstisnull)
{

    check_expected(giststate);
    check_expected(attno);
    check_expected(entry1);
    check_expected(isnull1);
    check_expected(entry2);
    check_expected(isnull2);
    check_expected(dst);
    check_expected(dstisnull);
    optional_assignment(giststate);
    optional_assignment(entry1);
    optional_assignment(entry2);
    optional_assignment(dst);
    optional_assignment(dstisnull);
    mock();

}

Datum 
gistbulkdelete(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
gistvacuumcleanup(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

void 
gistSplitByKey(Relation r, Page page, IndexTuple * itup, int len, GISTSTATE * giststate, GistSplitVector * v, GistEntryVector * entryvec, int attno)
{

    check_expected(r);
    check_expected(page);
    check_expected(itup);
    check_expected(len);
    check_expected(giststate);
    check_expected(v);
    check_expected(entryvec);
    check_expected(attno);
    optional_assignment(itup);
    optional_assignment(giststate);
    optional_assignment(v);
    optional_assignment(entryvec);
    mock();

}
