/*-------------------------------------------------------------------------
 *
 * cdbcat.c
 *	  Provides routines for reading info from mpp schema tables
 *
 * Copyright (c) 2003-2005, Greenplum
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "catalog/pg_namespace.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "cdb/cdbcat.h"
#include "cdb/cdbvars.h"          /* Mpp_role */
#include "catalog/pg_type.h"
#include "utils/array.h"
static void  extract_INT2OID_array(Datum array_datum, int* lenp, int16** vecp);

/*
 * This flag controls the policy type returned from
 * get_table_policy for non-partitioned tables.
 * It's a kludge put in to allow us to do
 * distributed queries on catalog tables, like pg_class
 * and pg_statistic.
 * To use it, simply set it to true before running a catalog query, then set 
 * it back to false.
 */
extern bool bTreatLocalsAsPartitioned;

/*
 * Initially, and normally, false.
 * To use it, simply set it to true before running a catalog query, then set 
 * it back to false.
 */
bool bTreatLocalsAsPartitioned = false;

/*
 * MppPolicyCopy -- Return a copy of a MppPolicy object.
 *
 * The copy is palloc'ed in the caller's context.
 */
MppPolicy *
MppPolicyCopy(const MppPolicy *src)
{
    MppPolicy *tgt;
    size_t     nb;
    if (!src)
        return NULL;
    nb = sizeof(*src) + src->nattrs * sizeof(src->attrs[0]);
    tgt = (MppPolicy*)palloc(nb);
    memcpy(tgt, src, nb);
    return tgt;
}                                       /* MppPolicyCopy */
 
/*
 * Returns the policy of a table as determined from the mpp.policy table
 * in a MppPolicy structure allocated in the current memory context.
 *
 * The caller is responsible for passing in a valid relation oid.  This
 * function does not check and assigns a policy of type POLICYTYPE_ENTRY
 * for any oid not found in .policy.
 */
MppPolicy *get_table_policy(Oid tbloid)
{
	MppPolicy *		policy = NULL;  /* The result */
	Relation		mpp_policy_rel;
	Oid				mpp_policy_index_oid;
    Oid             mpp_namespace_oid;
	ScanKeyData		policy_key[1];
	SysScanDesc		mpp_policy_scan;
	HeapTuple		mpp_policy_tuple = NULL;
	
    /*
     * Skip if qExec or utility mode.
     */
    if ( Mpp_role != MPP_ROLE_DISPATCH )
        return NULL;

    /*
	 * We need to read the mpp.policy table
	 */	
	
	/*
	 * first, get the oid of the mpp namespace
	 */
	mpp_namespace_oid = getMPPNamespaceOid();
	if ( mpp_namespace_oid == InvalidOid )
		return NULL; 
		
	/*
	 * Then, open the table and and find the oid of the index
	 */
	mpp_policy_rel = HeapOpenMPPObject( MppPolicyRelationName, mpp_namespace_oid, true );

	/*
	 * We scan based on the unique index on localoid
	 */
	mpp_policy_index_oid = get_relname_relid(MppPolicyLocalOidIndexName, 
												mpp_namespace_oid);

	/*
	 * Set up a ScanKey for the value of the localoid field
	 */
	ScanKeyInit(&policy_key[0], Anum_mpp_policy_localoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(tbloid) );
					
	/*
	 * Begin the scan
	 */
	mpp_policy_scan = mpptable_beginscan(mpp_policy_rel, 
										 mpp_policy_index_oid, 
									     (mpp_policy_index_oid != InvalidOid), 
										  SnapshotNow, 
										  1, 
										  policy_key);
		
	/*
	 * Read first (and only ) tuple
	 */
	mpp_policy_tuple = systable_getnext(mpp_policy_scan);
	if ( HeapTupleIsValid(mpp_policy_tuple) )
	{
		bool	isNull;
		Datum	attr;
		int		i, nattrs = 0;
		int16*	attrnums = NULL;
		size_t  sz;
		
		/*
		 * Get the attributes on which to partition.
		 */
		attr = heap_getattr(mpp_policy_tuple, Anum_mpp_policy_attrnums, mpp_policy_rel->rd_att, &isNull);
		Assert( !isNull );
		extract_INT2OID_array(attr, &nattrs, &attrnums);
		Assert( nattrs >= 0 );
		
		sz = sizeof(MppPolicy) + nattrs*sizeof(policy->attrs[0]);
		policy = (MppPolicy*) palloc(sz);
		policy->ptype = POLICYTYPE_PARTITIONED;
		policy->nattrs = nattrs;
		for ( i = 0; i < nattrs; i++ )
		{
			policy->attrs[i] = attrnums[i];
		}
	}
	
	/*
	 * Cleanup the scan and relation objects.
	 */
	systable_endscan(mpp_policy_scan);
	heap_close(mpp_policy_rel, AccessShareLock);
	
	/* Interpret absence of a valid policy row as POLICYTYPE_ENTRY */
	if ( policy == NULL )
	{
		policy = palloc(sizeof(MppPolicy));
		policy->ptype = bTreatLocalsAsPartitioned ? POLICYTYPE_PARTITIONED : POLICYTYPE_ENTRY;
		policy->nattrs= 0;
	}
			
	return policy;
}

/*
 * getMPPNamespaceOid: gets the oid of the mpp namespace.
 */
Oid getMPPNamespaceOid(void)
{
	Oid				mppOid = InvalidOid;
	Relation		pg_namespace_rel;
	ScanKeyData		namespace_key[1];
	NameData		nname;
	HeapScanDesc	pg_namespace_scan;
	HeapTuple		pg_namespace_tuple;
	
	/*
	 * We need to read the pg_namespace table
	 */	
	namestrcpy(&nname, MppNamespaceName);

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
		mppOid = HeapTupleGetOid(pg_namespace_tuple);
	}
	
	/*
	 * Clean up the scan and relation objects
	 */	
	heap_endscan(pg_namespace_scan);
	heap_close(pg_namespace_rel, AccessShareLock);
	
	return mppOid;
}

/*
 * HeapOpenMPPObject: opens a relation (either a table or an index) by name
 * that is part of the mpp namespace.
 */
Relation HeapOpenMPPObject( const char *pszName, Oid mpp_namespace_oid, bool bTable )
{
	Relation rel;
	
	/*
	 * Get the oid of the table or index, using the mpp namespace oid and the table name
	 */	
	Oid mpp_oid = get_relname_relid(pszName, mpp_namespace_oid);
	
	/*
	 * If the table or index wasn't there, we consider this a fatal error.
	 */	
	if ( mpp_oid == InvalidOid )
       ereport(ERROR, 
		(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
	        errmsg("Cannot retrieve oid of %s.%s", MppNamespaceName, pszName)));

	/*
	 * If its a table, use heap_open. If an index, use index_open
	 */	
	if ( bTable )
		rel = heap_open(mpp_oid, AccessShareLock);
	else
		rel = index_open(mpp_oid);
	
	return rel;
}

/*
 * mpptable_beginscan --- set up for heap-or-index scan
 *
 * similar to systable_beginscan, but using an Oid for index, instead of name
 */
SysScanDesc mpptable_beginscan(Relation heapRelation,
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
 * Extract len and pointer to buffer from an int16[] (vector) Datum 
 * representing a PostgreSQL INT2OID type.
 */
 static void
 extract_INT2OID_array(Datum array_datum, int* lenp, int16** vecp)
 {
	ArrayType *array_type;
	
	Assert( lenp != NULL );
	Assert( vecp != NULL );
	
	array_type = DatumGetArrayTypeP(array_datum);
	Assert( ARR_NDIM(array_type) == 1 );
	Assert( ARR_ELEMTYPE(array_type) == INT2OID );
	Assert( ARR_LBOUND(array_type)[0] == 1 );
	*lenp = ARR_DIMS(array_type)[0];
	*vecp = (int16*)ARR_DATA_PTR(array_type);
	
	return;
 }	
 
/*
 * Sets the policy of a table into the mpp.policy table
 * from a MppPolicy structure.
 *
 */
void
set_table_policy(Oid tbloid, MppPolicy * policy)
{

	Relation		mpp_policy_rel;
	/*Oid				mpp_policy_index_oid;
	ScanKeyData		policy_key[1];
	SysScanDesc		mpp_policy_scan;*/
	HeapTuple		mpp_policy_tuple = NULL;

	ArrayType  *attrnums;

	char		nulls[2];
	Datum		values[2];
	
	
	
	/*
	 * first, get the oid of the mpp namespace
	 */
	Oid mpp_namespace_oid = getMPPNamespaceOid();
	if ( mpp_namespace_oid == InvalidOid )
	{
		ereport(ERROR, (	
			errcode(ERRCODE_CDB_INTERNAL_ERROR),
			errmsg("MPP schema not found.")
		));
		
		return; /* Won't happen */
	}
		

	Assert(policy->nattrs >= 0);
	Assert(policy->ptype == POLICYTYPE_PARTITIONED);

	/* open the policy table for writing */
	{
		
		/*
		 * Get the oid of the policy table, using the mpp namespace oid and the table name
		 */	
		Oid mpp_oid = get_relname_relid(MppPolicyRelationName, mpp_namespace_oid);
		
		/*
		 * If the policy table wasn't there, we consider this a fatal error.
		 */	
		if ( mpp_oid == InvalidOid )
		 ereport(ERROR, 
			(errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
				errmsg("Cannot retrieve oid of %s.%s", MppNamespaceName, MppPolicyRelationName)));

		mpp_policy_rel = heap_open(mpp_oid, RowExclusiveLock);
	}


	/*
	 * Convert C arrays into Postgres arrays.
	 */
	if (policy->nattrs > 0)
	{
		int i;
		Datum	   *akey;

		akey = (Datum *) palloc(policy->nattrs * sizeof(Datum));
		for (i = 0; i < policy->nattrs; i++)
			akey[i] = Int16GetDatum(policy->attrs[i]);
		attrnums = construct_array(akey, policy->nattrs,
									  INT2OID, 2, true, 's');
	}
	else
	{
		attrnums = NULL;
	}


	nulls[0] = ' ';
	nulls[1] = ' ';
	values[0] =  ObjectIdGetDatum(tbloid);

	if (attrnums)
		values[1] =  PointerGetDatum(attrnums);
	else
		nulls[1] = 'n';

	mpp_policy_tuple = heap_formtuple(RelationGetDescr(mpp_policy_rel), values, nulls);

	simple_heap_insert(mpp_policy_rel, mpp_policy_tuple);

	/* update catalog indexes */
	CatalogUpdateIndexes(mpp_policy_rel, mpp_policy_tuple);


	heap_close(mpp_policy_rel, RowExclusiveLock);

	
	

	

			
	
}
