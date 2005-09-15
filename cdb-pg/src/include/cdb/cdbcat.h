/*-------------------------------------------------------------------------
 *
 * cdbcat.h
 *	  routines for reading info from cdb schema tables
 *
 * Copyright (c) 2003-2005, Metapa
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBCAT_H
#define CDBCAT_H

#include "access/genam.h"
#include "access/heapam.h"

#define MppNamespaceName			"mpp"

/*
 * Defines for mpp.pgdatabase table
 */
#define MppSegmentRelationName			"pgdatabase"
#define MppSegmentPrimaryKeyIndexName	"pgdatabase_pkey"
#define Natts_mpp_pgdatabase			6
#define Anum_mpp_pgdatabase_dbid		1
#define Anum_mpp_pgdatabase_isprimary	2
#define Anum_mpp_pgdatabase_content		3
#define Anum_mpp_pgdatabase_listenerid 	4
#define Anum_mpp_pgdatabase_valid         5
#define Anum_mpp_pgdatabase_definedprimary  6


/*
 * Defines for mpp.listener table
 */
#define MppListenerRelationName			"listener"
#define MppListenerPrimaryKeyIndexName	"listener_pkey"
#define Natts_mpp_listener				6
#define Anum_mpp_listener_listenerid	1
#define Anum_mpp_listener_hostid		2
#define Anum_mpp_listener_port			3
#define Anum_mpp_listener_datadir		4
#define Anum_mpp_listener_mppsuperuser	5
#define Anum_mpp_listener_mppsuperpswd	6

/*
 * Defines for mpp.host table
 */
#define MppHostRelationName			"host"
#define MppHostPrimaryKeyIndexName	"host_pkey"
#define Natts_mpp_host				2
#define Anum_mpp_host_hostid		1
#define Anum_mpp_host_hostname		2

/*
 * Defines for mpp.mpp table
 */
#define MppMppRelationName			"mpp"
#define Natts_mpp_mpp				4
#define Anum_mpp_mpp_mppname		1
#define Anum_mpp_mpp_numsegments	2
#define Anum_mpp_mpp_dbid			3
#define Anum_mpp_mpp_content		4

/*
 * Defines for mpp.policy
 */
#define MppPolicyRelationName		"policy"
#define MppPolicyLocalOidIndexName	"policy_localoid_key"
#define Natts_mpp_policy			2
#define Anum_mpp_policy_localoid	1
#define Anum_mpp_policy_attrnums	2

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
 * CdbPolicyCopy -- Return a copy of a CdbPolicy object.
 *
 * The copy is palloc'ed in the caller's context.
 */
CdbPolicy *
CdbPolicyCopy(const CdbPolicy *src);

/*
 * get_table_policy: returns a palloc'd CdbPolicy for a table oid.
 */
extern CdbPolicy *get_table_policy(Oid tbloid);

/*
 * set_table_policy: sets the CdbPolicy for a table.
 */
extern void set_table_policy(Oid tbloid, CdbPolicy * policy);

/*
 * getMPPNamespaceOid: gets the oid of the cdb namespace.
 */
extern Oid getMPPNamespaceOid(void);

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
