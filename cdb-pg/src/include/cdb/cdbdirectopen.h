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
#include "catalog/pg_class.h"
#include "catalog/gp_persistent.h"
#include "catalog/gp_global_sequence.h"
#include "catalog/pg_appendonly.h"
#include "access/transam.h"
#include "access/aosegfiles.h"
#include "access/aocssegfiles.h"

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

	struct tupleConstr constrData;

	struct tupleDesc descData;

	PgStat_TableStatus pgStat;

	RelationData relationData;

} DirectOpen;

// HUGE MAGIC DEFINE creates static variables with values for DirectOpen_Open. 
//     Class tuple values, 
//     Schema (i.e. pg_attribute) values and array,
//
//     There can only be ONE instance of a kind of open per process (backend).
//
//     IMPORTANT: Arrange to share the ONE instance globally.
//     IMPORTANT: Use the *Static define in the .c file and the *Extern define in the .h.
//
#define DirectOpenDefineStatic(name,pgClassStatic,attrArrayStatic,relHasOid) \
\
static FormData_pg_class *name##RelPgClass = &pgClassStatic; \
static FormData_pg_attribute *name##RelAttrArray = attrArrayStatic; \
static bool name##RelHasOid = relHasOid; \
\
static DirectOpen name##RelDirectOpen; \
\
Relation name##OpenShared(void) \
{ \
	return DirectOpen_Open( \
					&name##RelDirectOpen, \
					-1, \
					GLOBALTABLESPACE_OID, \
					0, \
					-1, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
} \
\
Relation name##Open( \
					Oid tablespace, \
					Oid database) \
{ \
	return DirectOpen_Open( \
					&name##RelDirectOpen, \
					-1, \
					tablespace, \
					database, \
					-1, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
} \
\
Relation name##OpenDynamic( \
					Oid relationId, \
					Oid tablespace, \
					Oid database, \
					Oid relfilenode) \
{ \
	return DirectOpen_Open( \
					&name##RelDirectOpen, \
					relationId, \
					tablespace, \
					database, \
					relfilenode, \
					name##RelPgClass, \
					name##RelAttrArray, \
					name##RelHasOid); \
} \
\
void name##Close(Relation relation) \
{ \
	return DirectOpen_Close( \
					&name##RelDirectOpen, \
					relation); \
} \
\


#define DirectOpenDefineExtern(name) \
\
extern Relation name##OpenShared(void); \
\
extern Relation name##Open( \
					Oid tablespace, \
					Oid database); \
\
extern Relation name##OpenDynamic( \
					Oid relationId, \
					Oid tablespace, \
					Oid database, \
					Oid relfilenode); \
\
extern void name##Close(Relation relation);

DirectOpenDefineExtern(DirectOpen_PgClass);

DirectOpenDefineExtern(DirectOpen_GpRelationNode);

DirectOpenDefineExtern(DirectOpen_PgAppendOnly);

DirectOpenDefineExtern(DirectOpen_PgAoSeg);

DirectOpenDefineExtern(DirectOpen_PgAoCsSeg);

DirectOpenDefineExtern(DirectOpen_GpGlobalSequence);

DirectOpenDefineExtern(DirectOpen_GpPersistentRelationNode);

DirectOpenDefineExtern(DirectOpen_GpPersistentDatabaseNode);

DirectOpenDefineExtern(DirectOpen_GpPersistentTableSpaceNode);

DirectOpenDefineExtern(DirectOpen_GpPersistentFileSpaceNode);

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
