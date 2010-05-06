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
	bool isOpen;

	struct tupleConstr constrData;

	struct tupleDesc descData;

	PgStat_TableStatus pgStat;

	RelationData relationData;

} DirectOpen;

// HUGE MAGIC DEFINE parameterize by: 
//     Class tuple values, 
//     Schema (i.e. pg_attribute) values and array,
//     (NOTE: REMAINING FIELDS 0)

#define DirectOpenDefine(name,pgClassStatic,attrArrayStatic,relHasOid) \
\
static FormData_pg_class *name##RelPgClass = &pgClassStatic; \
static FormData_pg_attribute *name##RelAttrArray = attrArrayStatic; \
static bool name##RelHasOid = relHasOid; \
\
static inline Relation name##OpenShared( \
	DirectOpen *directOpen) \
{ \
	return DirectOpen_Open( \
					directOpen, \
					-1, \
					GLOBALTABLESPACE_OID, \
					0, \
					-1, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
} \
\
static inline Relation name##Open( \
					DirectOpen *directOpen, \
					Oid tablespace, \
					Oid database) \
{ \
	return DirectOpen_Open( \
					directOpen, \
					-1, \
					tablespace, \
					database, \
					-1, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
} \
\
\
static inline Relation name##OpenDynamic( \
					DirectOpen *directOpen, \
					Oid relationId, \
					Oid tablespace, \
					Oid database, \
					Oid relfilenode) \
{ \
	return DirectOpen_Open( \
					directOpen, \
					relationId, \
					tablespace, \
					database, \
					relfilenode, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
}

extern Relation DirectOpen_Open(
	DirectOpen *direct,

	Oid relationId,

	Oid tablespace,

	Oid database,

	Oid relfilenode,

	FormData_pg_class *pgClass,

	FormData_pg_attribute *attrArray,

	bool relHasOid);


extern void DirectOpen_Close(
	DirectOpen 	*direct,

	Relation	rel);

#endif   /* CDBDIRECTOPEN_H */
