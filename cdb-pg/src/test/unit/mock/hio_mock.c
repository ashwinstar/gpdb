/*
 * Auto-generated Mocking Source
 * 
 * Source: access/hio.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "storage/freespace.h"
#include "cdb/cdbfilerepprimary.h"

void 
RelationPutHeapTuple(Relation relation, Buffer buffer, HeapTuple tuple)
{

    check_expected(relation);
    check_expected(buffer);
    check_expected(tuple);

    mock();

}

Buffer 
RelationGetBufferForTuple(Relation relation, Size len, Buffer otherBuffer, bool use_fsm)
{

    check_expected(relation);
    check_expected(len);
    check_expected(otherBuffer);
    check_expected(use_fsm);

    return (Buffer) mock();

}
