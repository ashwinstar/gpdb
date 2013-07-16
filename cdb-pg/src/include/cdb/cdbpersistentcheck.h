/*-------------------------------------------------------------------------
 *
 * cdbpersistentcheck.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBPERSISTENTCHECK_H
#define CDBPERSISTENTCHECK_H

#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "storage/fd.h"

extern void PTCheck_BeforeAddingEntry( PersistentStoreData *storeData, Datum *values);

extern ScanKey Persistent_RelationScanKeyInit(Datum *values, int *nKeys);
extern bool Persistent_RelationAllowDuplicateEntry(Datum *exist_values, Datum *new_values);
extern ScanKey Persistent_DatabaseScanKeyInit(Datum *values, int *nKeys);
extern bool Persistent_DatabaseAllowDuplicateEntry(Datum *exist_values, Datum *new_values);
extern ScanKey Persistent_TablespaceScanKeyInit(Datum *values, int *nKeys);
extern bool Persistent_TablespaceAllowDuplicateEntry(Datum *exist_values, Datum *new_values);
extern ScanKey Persistent_FilespaceScanKeyInit(Datum *values, int *nKeys);
extern bool Persistent_FilespaceAllowDuplicateEntry(Datum *exist_values, Datum *new_values);


#endif
