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

#include "catalog/pg_type.h"
#include "utils/array.h"
static void  extract_int_array(Datum array_datum, int* lenp, int** vecp);
 
 
/*
 * Returns the policy of a table as determined from the cdb.policy table
 * in a CdbPolicy structure allocated in the current memory context.
 *
 * The caller is responsible for passing in a valid relation oid.  This
 * function does not check and assigns a policy of type POLICYTYPE_ENTRY
 * for any oid not found in cdb.policy.
 */
CdbPolicy *get_table_policy(Oid tbloid)
{
	CdbPolicy *		policy = NULL;  /* The result */
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
	{
		ereport(ERROR, (	
			errcode(ERRCODE_CDB_INTERNAL_ERROR),
			errmsg("CDB schema not found.")
		));
		
		return NULL; /* Won't happen */
	}
		
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
		char*	policy_str = NULL;
		int		i, nattrs = 0;
		int*	attrnums = NULL;
		size_t  sz;
		
		/*
		 * Get the policytype attribute
		 */
		attr = heap_getattr(cdb_policy_tuple, Anum_cdb_policy_policytype, cdb_policy_rel->rd_att, &isNull);
		Assert( !isNull );
		policy_str = DatumGetCString(DirectFunctionCall1(textout, attr));
		
		/*
		 * Outcome depends on CdbPolicyType. This is a bit of a mess due
		 * to the need to work against the old CDB schema, which doesn't
		 * really fit.
		 *
		 * TODO Clean up when new schema design is in place.
		 */
		switch( policy_str[0] )
		{
			case 'p':  /* POLICYTYPE_PARTITIONED */
				/* Get the attributes on which to partition */
				attr = heap_getattr(cdb_policy_tuple, Anum_cdb_policy_attrnums, cdb_policy_rel->rd_att, &isNull);
				Assert( !isNull );
				extract_int_array(attr, &nattrs, &attrnums);
				Assert( nattrs >= 0 );
				
				sz = sizeof(CdbPolicy) + nattrs*sizeof(policy->attrs[0]);
				policy = (CdbPolicy*) palloc(sz);
				policy->ptype = POLICYTYPE_PARTITIONED;
				policy->nattrs = nattrs;
				for ( i = 0; i < nattrs; i++ )
				{
					policy->attrs[i] = attrnums[i];
				}
				break;	
			default:
				ereport(WARNING, (	
					errcode(ERRCODE_WARNING_DEPRECATED_FEATURE),
					errmsg("Ignoring cdb.policy row with policytype=\"%s\".", policy_str),
					errhint("Update your CDB database and CDB schema contents!")
				));
				break;	
		}
		
		if ( policy_str != NULL )
			pfree( policy_str );
	}
	
	/*
	 * Cleanup the scan and relation objects.
	 */
	systable_endscan(cdb_policy_scan);
	heap_close(cdb_policy_rel, AccessShareLock);
	
	/* Interpret absence of a valid policy row as POLICYTYPE_ENTRY */
	if ( policy == NULL )
	{
		policy = palloc(sizeof(CdbPolicy));
		policy->ptype = POLICYTYPE_ENTRY;
		policy->nattrs= 0;
	}
			
	return policy;
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


/*
 * Extract len and pointer to buffer from an int32[] (vector) Datum.
 */
 static void
 extract_int_array(Datum array_datum, int* lenp, int** vecp)
 {
	ArrayType *array_type;
	
	Assert( lenp != NULL );
	Assert( vecp != NULL );
	
	array_type = DatumGetArrayTypeP(array_datum);
	Assert( ARR_NDIM(array_type) == 1 );
	Assert( ARR_ELEMTYPE(array_type) == INT4OID );
	Assert( ARR_LBOUND(array_type)[0] == 1 );
	*lenp = ARR_DIMS(array_type)[0];
	*vecp = (int*)ARR_DATA_PTR(array_type);
	
	return;
 }	
 
