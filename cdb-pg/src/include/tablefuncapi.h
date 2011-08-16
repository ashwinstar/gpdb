/*-------------------------------------------------------------------------
 *
 * tablefuncapi.h
 *	  Declarations for Table Function API
 *
 * Copyright (c) 2011, EMC corporation
 *-------------------------------------------------------------------------
 */
#ifndef TABLEFUNCAPI_H
#define TABLEFUNCAPI_H

#include "fmgr.h"
#include "access/tupdesc.h"
#include "access/htup.h"

typedef struct AnyTableData *AnyTable;

#define DatumGetAnyTable(d)   ((AnyTable) DatumGetPointer(d))
#define AnyTableGetDatum(x)   PointerGetDatum(x)

#define PG_GETARG_ANYTABLE(n) (DatumGetAnyTable(PG_GETARG_DATUM(n)))

extern TupleDesc AnyTable_GetTupleDesc(AnyTable);
extern HeapTuple AnyTable_GetNextTuple(AnyTable);

#endif
