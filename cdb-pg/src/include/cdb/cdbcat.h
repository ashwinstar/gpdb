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
 * Defines for cdb.segdb table
 */
#define CdbSegDBRelationName		"segdb"
#define Natts_cdb_segdb				4
#define Anum_cdb_segdb_segindex		1
#define Anum_cdb_segdb_isprimary	2
#define Anum_cdb_segdb_listenerid	3
#define Anum_cdb_segdb_isvalid		4

/*
 * Defines for cdb.segment table
 */
#define CdbSegmentRelationName			"segment"
#define CdbSegmentPrimaryKeyIndexName	"segment_pkey"
#define Natts_cdb_segment				2
#define Anum_cdb_segment_segindex		1
#define Anum_cdb_segment_dbname			2

/*
 * Defines for cdb.listener table
 */
#define CdbListenerRelationName			"listener"
#define CdbListenerPrimaryKeyIndexName	"listener_pkey"
#define Natts_cdb_listener				4
#define Anum_cdb_listener_listenerid	1
#define Anum_cdb_listener_hostid		2
#define Anum_cdb_listener_port			3
#define Anum_cdb_listener_datadir		4

/*
 * Defines for cdb.host table
 */
#define CdbHostRelationName			"host"
#define CdbHostPrimaryKeyIndexName	"host_pkey"
#define Natts_cdb_host				3
#define Anum_cdb_host_hostid		1
#define Anum_cdb_host_hostname		2
#define Anum_cdb_host_amsport		3

/*
 * Defines for cdb.cdb table
 */
#define CdbCdbRelationName			"cdb"
#define Natts_cdb_cdb				7
#define Anum_cdb_cdb_cdbname		1
#define Anum_cdb_cdb_cdbsuperuser	2
#define Anum_cdb_cdb_cdbsuperpswd	3
#define Anum_cdb_cdb_numSegments	4
#define Anum_cdb_cdb_listenerId		5
#define Anum_cdb_cdb_segindex		6
#define Anum_cdb_cdb_isprimary		7

/*
 * Defines for cdb.policy
 */
#define CdbPolicyRelationName		"policy"
#define CdbPolicyLocalOidIndexName	"policy_localoid_key"
#define Natts_cdb_policy			4
#define Anum_cdb_policy_globalid	1
#define Anum_cdb_policy_localoid	2
#define Anum_cdb_policy_policytype	3
#define Anum_cdb_policy_attrnums	4

/*
 * enum representing the policy type of a table,
 * from cdb.policy.policytype.
 * The mapping is as follows:
 * 'p'  ----> POLICYTYPE_PARTITIONED
 * 'r'	----> POLICYTYPE_REPLICATED
 * 's'  ----> POLICYTYPE_SPECIAL
 */
typedef enum CdbPolicyType
{
	POLICYTYPE_UNDEFINED,	
	POLICYTYPE_PARTITIONED,	
	POLICYTYPE_REPLICATED,	
	POLICYTYPE_SPECIAL	
} CdbPolicyType;

/*
 * get_table_policytype: returns the CdbPolicyType for a table oid.
 */
extern CdbPolicyType get_table_policytype(Oid tbloid);

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
