/*-------------------------------------------------------------------------
 *
 * cdbcat.h
 *	  routines for reading info from MPP schema tables
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBCAT_H
#define CDBCAT_H

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/mpp_configuration.h"
#include "catalog/mpp_mpp.h"
#include "catalog/mpp_policy.h"
#include "catalog/mpp_version.h"



/*
 * HeapOpenMPPObject: opens a relation (either a table or an index) by name
 * that is part of the MPP namespace.
 */
//extern Relation HeapOpenMPPObject(const char *pszName, Oid mpp_namespace_oid, bool bTable);

/*
 * mpptable_beginscan --- set up for heap-or-index scan
 *
 * similar to systable_beginscan, but using an Oid for index, instead of name
 */
//extern SysScanDesc mpptable_beginscan(Relation heapRelation,
//				   Oid indexOid,
//				   bool indexOK,
//				   Snapshot snapshot,
//				   int nkeys, ScanKey key);

#endif   /* CDBCAT_H */
