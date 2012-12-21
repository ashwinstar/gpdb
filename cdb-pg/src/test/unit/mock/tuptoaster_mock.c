/*
 * Auto-generated Mocking Source
 * 
 * Source: access/tuptoaster.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <unistd.h>
#include <fcntl.h>
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "catalog/catalog.h"
#include "utils/fmgroids.h"
#include "utils/pg_lzcompress.h"
#include "utils/rel.h"
#include "utils/typcache.h"

HeapTuple 
toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup, MemTupleBinding * pbind, int toast_tuple_target, bool isFrozen)
{

    check_expected(rel);
    check_expected(newtup);
    check_expected(oldtup);
    check_expected(pbind);
    check_expected(toast_tuple_target);
    check_expected(isFrozen);
    optional_assignment(pbind);
    return (HeapTuple) mock();

}

void 
toast_delete(Relation rel, HeapTuple oldtup)
{

    check_expected(rel);
    check_expected(oldtup);

    mock();

}

struct varlena *
heap_tuple_fetch_attr(struct varlena * attr)
{

    check_expected(attr);
    optional_assignment(attr);
    mock();

}

void 
varattrib_untoast_ptr_len(Datum d, char **datastart, int *len, void **tofree)
{

    check_expected(d);
    check_expected(datastart);
    check_expected(len);
    check_expected(tofree);
    optional_assignment(datastart);
    optional_assignment(len);
    optional_assignment(tofree);
    mock();

}

int 
varattrib_untoast_len(Datum d)
{

    check_expected(d);

    return (int) mock();

}

struct varlena *
heap_tuple_untoast_attr(struct varlena * attr)
{

    check_expected(attr);
    optional_assignment(attr);
    mock();

}

struct varlena *
heap_tuple_untoast_attr_slice(struct varlena * attr, int32 sliceoffset, int32 slicelength)
{

    check_expected(attr);
    check_expected(sliceoffset);
    check_expected(slicelength);
    optional_assignment(attr);
    mock();

}

Datum 
toast_flatten_tuple_attribute(Datum value, Oid typeId, int32 typeMod)
{

    check_expected(value);
    check_expected(typeId);
    check_expected(typeMod);

    return (Datum) mock();

}

Datum 
toast_compress_datum(Datum value)
{

    check_expected(value);

    return (Datum) mock();

}

Size 
toast_raw_datum_size(Datum value)
{

    check_expected(value);

    return (Size) mock();

}

Size 
toast_datum_size(Datum value)
{

    check_expected(value);

    return (Size) mock();

}
