/*
 * Auto-generated Mocking Source
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/gist_private.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "cdb/cdbfilerepprimary.h"

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

bool 
gistfitpage(IndexTuple * itvec, int len)
{

    check_expected(itvec);
    check_expected(len);
    optional_assignment(itvec);
    return (bool) mock();

}

IndexTuple     *
gistextractpage(Page page, int *len)
{

    check_expected(page);
    check_expected(len);
    optional_assignment(len);
    return (IndexTuple *) (unsigned) mock();

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
    return (IndexTuple *) (unsigned) mock();

}

IndexTupleData *
gistfillitupvec(IndexTuple * vec, int veclen, int *memlen)
{

    check_expected(vec);
    check_expected(veclen);
    check_expected(memlen);
    optional_assignment(vec);
    optional_assignment(memlen);
    return (IndexTupleData *) (unsigned) mock();

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

IndexTuple 
gistFormTuple(GISTSTATE * giststate, Relation r, Datum attdata[], bool isnull[], bool newValues)
{

    check_expected(giststate);
    check_expected(r);
    check_expected(attdata);
    check_expected(isnull);
    check_expected(newValues);
    optional_assignment(giststate);
    return (IndexTuple) mock();

}

float 
gistpenalty(GISTSTATE * giststate, int attno, GISTENTRY * orig, bool isNullOrig, GISTENTRY * add, bool isNullAdd)
{

    check_expected(giststate);
    check_expected(attno);
    check_expected(orig);
    check_expected(isNullOrig);
    check_expected(add);
    check_expected(isNullAdd);
    optional_assignment(giststate);
    optional_assignment(orig);
    optional_assignment(add);
    return (float) mock();

}

void 
GISTInitBuffer(Buffer b, uint32 f)
{

    check_expected(b);
    check_expected(f);

    mock();

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

Datum 
gistoptions(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}
