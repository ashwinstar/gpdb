/*-------------------------------------------------------------------------
 *
 * cdbcat.h
 *	  routines for reading info from cdb schema tables
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBCAT_H
#define CDBCAT_H

#include "access/genam.h"
#include "access/heapam.h"

#define CdbNamespaceName			"cdb"

/*
 * Defines for cdb.pgdatabase table
 */
#define CdbSegmentRelationName			"pgdatabase"
#define CdbSegmentPrimaryKeyIndexName	"pgdatabase_pkey"
#define Natts_cdb_pgdatabase			5
#define Anum_cdb_pgdatabase_dbid		1
#define Anum_cdb_pgdatabase_isprimary	2
#define Anum_cdb_pgdatabase_dbname		3
#define Anum_cdb_pgdatabase_content		4
#define Anum_cdb_pgdatabase_listenerid	5

/*
 * Defines for cdb.listener table
 */
#define CdbListenerRelationName			"listener"
#define CdbListenerPrimaryKeyIndexName	"listener_pkey"
#define Natts_cdb_listener				6
#define Anum_cdb_listener_listenerid	1
#define Anum_cdb_listener_hostid		2
#define Anum_cdb_listener_port			3
#define Anum_cdb_listener_datadir		4
#define Anum_cdb_listener_cdbsuperuser	5
#define Anum_cdb_listener_cdbsuperpswd	6

/*
 * Defines for cdb.host table
 */
#define CdbHostRelationName			"host"
#define CdbHostPrimaryKeyIndexName	"host_pkey"
#define Natts_cdb_host				2
#define Anum_cdb_host_hostid		1
#define Anum_cdb_host_hostname		2

/*
 * Defines for cdb.cdb table
 */
#define CdbCdbRelationName			"cdb"
#define Natts_cdb_cdb				4
#define Anum_cdb_cdb_cdbname		1
#define Anum_cdb_cdb_numsegments	2
#define Anum_cdb_cdb_dbid			3
#define Anum_cdb_cdb_content		4

/*
 * Defines for cdb.policy
 */
#define CdbPolicyRelationName		"policy"
#define CdbPolicyLocalOidIndexName	"policy_localoid_key"
#define Natts_cdb_policy			2
#define Anum_cdb_policy_localoid	1
#define Anum_cdb_policy_attrnums	2

/*
 * CdbPolicyType represents a type of policy under which a relation's
 * tuples may be assigned to a component database.
 */
typedef enum CdbPolicyType
{
	POLICYTYPE_UNDEFINED,	
	POLICYTYPE_PARTITIONED,	/* Tuples partitioned onto segment database. */
	POLICYTYPE_ENTRY		/* Tuples stored on enty database. */
} CdbPolicyType;

/*
 * CdbPolicy represents a CDB data distribution policy. The ptype field
 * is always significant.  Other fields may be specific to a particular
 * type.
 *
 * A CdbPolicy is typically palloc'd with space for nattrs integer
 * attribute numbers (attrs) in addition to sizeof(CdbPolicy).
 */
typedef
struct CdbPolicy
{
	CdbPolicyType ptype;
	
	/* These fields apply to POLICYTYPE_PARTITIONED. */
	int nattrs;
	AttrNumber attrs[0]; /* the first of nattrs attribute numbers.  */
} CdbPolicy;

/*
 * get_table_policy: returns a palloc'd CdbPolicy for a table oid.
 */
extern CdbPolicy *get_table_policy(Oid tbloid);

/*
 * set_table_policy: sets the CdbPolicy for a table.
 */
extern void set_table_policy(Oid tbloid, CdbPolicy * policy);

/*
 * getCDBNamespaceOid: gets the oid of the cdb namespace.
 */
extern Oid getCDBNamespaceOid(void);

/*
 * HeapOpenCDBObject: opens a relation (either a table or an index) by name
 * that is part of the cdb namespace.
 */
extern Relation HeapOpenCDBObject( const char *pszName, Oid cdb_namespace_oid, bool bTable );

/*
 * cdbtable_beginscan --- set up for heap-or-index scan
 *
 * similar to systable_beginscan, but using an Oid for index, instead of name
 */
extern SysScanDesc cdbtable_beginscan(Relation heapRelation,
				   			   			   Oid indexOid,
										   bool indexOK,
										   Snapshot snapshot,
										   int nkeys, ScanKey key);

#endif	/* CDBCAT_H */
