/*-------------------------------------------------------------------------
 *
 * cdbcat.c
 *	  Provides routines for reading info from cdb schema tables
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "catalog/pg_namespace.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "cdb/cdbcat.h"


/*
 * Returns the policy type of a table from the cdb.policy table
 */
CdbPolicyType get_table_policytype(Oid tbloid)
{
	CdbPolicyType	ptype = POLICYTYPE_UNDEFINED;
	Relation		cdb_policy_rel;
	Oid				cdb_policy_index_oid;
	ScanKeyData		policy_key[1];
	SysScanDesc		cdb_policy_scan;
	HeapTuple		cdb_policy_tuple = NULL;
	
	/*
	 * We need to read the cdb.policy table
	 */	
	
	/*
	 * first, get the oid of the cdb namespace
	 */
	Oid cdb_namespace_oid = getCDBNamespaceOid();
	if ( cdb_namespace_oid == InvalidOid )
		return ptype;
		
	/*
	 * Then, open the table and and find the oid of the index
	 */
	cdb_policy_rel = HeapOpenCDBObject( CdbPolicyRelationName, cdb_namespace_oid, true );

	/*
	 * We scan based on the unique index on localoid
	 */
	cdb_policy_index_oid = get_relname_relid(CdbPolicyLocalOidIndexName, 
												cdb_namespace_oid);

	/*
	 * Set up a ScanKey for the value of the localoid field
	 */
	ScanKeyInit(&policy_key[0], Anum_cdb_policy_localoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(tbloid) );
					
	/*
	 * Begin the scan
	 */
	cdb_policy_scan = cdbtable_beginscan(cdb_policy_rel, 
										 cdb_policy_index_oid, 
									     (cdb_policy_index_oid != InvalidOid), 
										  SnapshotNow, 
										  1, 
										  policy_key);
		
	/*
	 * Read first (and only ) tuple
	 */
	cdb_policy_tuple = systable_getnext(cdb_policy_scan);
	if ( HeapTupleIsValid(cdb_policy_tuple) )
	{
		bool	isNull;
		Datum	attr;
		char*	policy = NULL;
		
		/*
		 * Get the policytype attribute
		 */
		attr = heap_getattr(cdb_policy_tuple, Anum_cdb_policy_policytype, cdb_policy_rel->rd_att, &isNull);
		Assert( !isNull );
		policy = DatumGetCString(DirectFunctionCall1(textout, attr));
		
		/*
		 * Convert it to CdbPolicyType
		 */
		switch( policy[0] )
		{
			case 'p':
				ptype = POLICYTYPE_PARTITIONED;
				break;	
			case 'r':
				ptype = POLICYTYPE_REPLICATED;
				break;	
			case 's':
				ptype = POLICYTYPE_SPECIAL;
				break;	
		}
		
		if ( policy != NULL )
			pfree( policy );
	}
	
	/*
	 * Cleanup the scan and relation objects.
	 */
	systable_endscan(cdb_policy_scan);
	
	heap_close(cdb_policy_rel, AccessShareLock);
			
	return ptype;
}

/*
 * getCDBNamespaceOid: gets the oid of the cdb namespace.
 */
Oid getCDBNamespaceOid(void)
{
	Oid				cdbOid = InvalidOid;
	Relation		pg_namespace_rel;
	ScanKeyData		namespace_key[1];
	NameData		nname;
	HeapScanDesc	pg_namespace_scan;
	HeapTuple		pg_namespace_tuple;
	
	/*
	 * We need to read the pg_namespace table
	 */	
	namestrcpy(&nname, CdbNamespaceName);

	/*
	 * Set up a ScanKey based on the namespace name field
	 */	
	ScanKeyEntryInitialize(&namespace_key[0], 
							0x0,
							Anum_pg_namespace_nspname, 
							InvalidStrategy, InvalidOid,
							F_NAMEEQ,
  						    NameGetDatum(&nname) );
							   
	/*
	 * We do a heap scan
	 */	
	pg_namespace_rel = heap_openr("pg_namespace", AccessShareLock);							   
	pg_namespace_scan = heap_beginscan(pg_namespace_rel, 
										   SnapshotNow, 
										   1, 
										   namespace_key);
			
	/*
	 * Get the one (and only) tuple
	 */	
	pg_namespace_tuple = heap_getnext(pg_namespace_scan, ForwardScanDirection);
	if ( HeapTupleIsValid(pg_namespace_tuple) )
	{
		/*
		 * Get the oid of the tuple
		 */	
		cdbOid = HeapTupleGetOid(pg_namespace_tuple);
	}
	
	/*
	 * Clean up the scan and relation objects
	 */	
	heap_endscan(pg_namespace_scan);
	heap_close(pg_namespace_rel, AccessShareLock);
	
	return cdbOid;
}

/*
 * HeapOpenCDBObject: opens a relation (either a table or an index) by name
 * that is part of the cdb namespace.
 */
Relation HeapOpenCDBObject( const char *pszName, Oid cdb_namespace_oid, bool bTable )
{
	Relation rel;
	
	/*
	 * Get the oid of the table or index, using the cdb namespace oid and the table name
	 */	
	Oid cdb_oid = get_relname_relid(pszName, cdb_namespace_oid);
	
	/*
	 * If the table or index wasn't there, we consider this a fatal error.
	 */	
	if ( cdb_oid == InvalidOid )
       ereport(ERROR, 
		(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
	        errmsg("Cannot retrieve oid of %s.%s", CdbNamespaceName, pszName)));

	/*
	 * If its a table, use heap_open. If an index, use index_open
	 */	
	if ( bTable )
		rel = heap_open(cdb_oid, AccessShareLock);
	else
		rel = index_open(cdb_oid);
	
	return rel;
}

/*
 * cdbtable_beginscan --- set up for heap-or-index scan
 *
 * similar to systable_beginscan, but using an Oid for index, instead of name
 */
SysScanDesc cdbtable_beginscan(Relation heapRelation,
				   			   Oid indexOid,
							   bool indexOK,
							   Snapshot snapshot,
							   int nkeys, ScanKey key)
{
	SysScanDesc sysscan;
	Relation	irel;

	if (indexOK && !IsIgnoringSystemIndexes())
	{
		irel = index_open(indexOid);

		if (ReindexIsProcessingIndex(RelationGetRelid(irel)))
		{
			/* oops, can't use index that's being rebuilt */
			index_close(irel);
			irel = NULL;
		}
	}
	else
		irel = NULL;

	sysscan = (SysScanDesc) palloc(sizeof(SysScanDescData));

	sysscan->heap_rel = heapRelation;
	sysscan->irel = irel;

	if (irel)
	{
		int			i;

		/*
		 * Change attribute numbers to be index column numbers.
		 *
		 * This code could be generalized to search for the index key numbers
		 * to substitute, but for now there's no need.
		 */
		for (i = 0; i < nkeys; i++)
		{
			Assert(key[i].sk_attno == irel->rd_index->indkey[i]);
			key[i].sk_attno = i + 1;
		}

		sysscan->iscan = index_beginscan(heapRelation, irel, snapshot,
										 nkeys, key);
		sysscan->scan = NULL;
	}
	else
	{
		sysscan->scan = heap_beginscan(heapRelation, snapshot, nkeys, key);
		sysscan->iscan = NULL;
	}

	return sysscan;
}

