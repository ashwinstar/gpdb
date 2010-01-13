/*-------------------------------------------------------------------------
 *
 * cdbdirectopen.h
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDIRECTOPEN_H
#define CDBDIRECTOPEN_H

#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"

/*
 * The goal here is to open any relations without going through
 * pg_class or the pg_class indices.  
 *
 * This allows us to handle relation CREATEs and DROPs
 * before XLOG recovery is complete, allows an REINDEX INDEX pg_class_oid_index
 * command to complete successfully, and more.
 */

typedef struct DirectOpen
{
	bool isInit;

	int natts;

	int relId;

	int relType;

	bool hasOid;

	// The following fields initialize to 0, with minor exceptions.

	struct tupleConstr constrData;

	struct tupleDesc descData;

	PgStat_TableStatus pgStat;

	RelationData relationData;

} DirectOpen;

// HUGE MAGIC DEFINE parameterize by: 
//     Class tuple values, 
//     Number of attributes, 
//     Schema (i.e. pg_attribute) values and array,
//     Relation id, and 
//     Relation type id.
//     (NOTE: REMAINING FIELDS 0)

#define DirectOpenDefine(name,pgClassStatic,natts,attrArrayStatic,attrPtrArrayStatic,relId,relType,relHasOid) \
\
static FormData_pg_class *name##RelPgClass = &pgClassStatic; \
static FormData_pg_attribute *name##RelAttrArray = attrArrayStatic; \
static Form_pg_attribute *name##RelAttrPtrArray = attrPtrArrayStatic; \
\
static DirectOpen name##RelDirectOpen = \
	{ false, \
	  natts, \
	  relId, \
	  relType, \
	  relHasOid }; \
\
static inline Relation name##OpenShared(void) \
{ \
	return DirectOpen_Open( \
				&name##RelDirectOpen, \
				GLOBALTABLESPACE_OID, \
				0, \
				name##RelPgClass, \
				name##RelAttrArray, \
				name##RelAttrPtrArray); \
} \
\
static inline Relation name##Open(Oid tablespace, Oid database) \
{ \
	return DirectOpen_Open( \
				&name##RelDirectOpen, \
				tablespace, \
				database, \
				name##RelPgClass, \
				name##RelAttrArray, \
				name##RelAttrPtrArray); \
} \
\
static inline void name##Close( \
	Relation	rel) \
{ \
	DirectOpen_Close( \
				&name##RelDirectOpen, \
				rel); \
}

extern Relation DirectOpen_Open(
	DirectOpen *direct,

	Oid tablespace,

	Oid database,

	FormData_pg_class *pgClass,

	FormData_pg_attribute *attrArray,

	Form_pg_attribute *attrPtrArray);


extern void DirectOpen_Close(
	DirectOpen 	*direct,

	Relation	rel);

#endif   /* CDBDIRECTOPEN_H */
