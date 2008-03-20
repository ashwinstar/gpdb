/*-------------------------------------------------------------------------
 *
 * cdbcat.c
 *	  Provides routines for reading info from mpp schema tables
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * $Id$
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
#include "cdb/cdbvars.h"		/* Gp_role */
#include "catalog/pg_type.h"
#include "utils/array.h"
static void extract_INT2OID_array(Datum array_datum, int *lenp, int16 **vecp);


/*
 * GpPolicyAlloc -- Allocate a GpPolicy object.
 *
 * To free the GpPolicy object, the caller should call GpPolicyFree(), or
 * let it remain allocated until the containing MemoryContext 'mcxt' is
 * destroyed or reset.  To minimize leakage when 'mcxt' is a long-lived
 * context such as CacheMemoryContext, GpPolicyFree() should be called.
 */
GpPolicy *
GpPolicyAlloc(MemoryContext mcxt, GpPolicyType ptype, int nattrs)
{
    GpPolicy   *policy = (GpPolicy *)MemoryContextAlloc(mcxt, sizeof(*policy));

    Assert(nattrs >= 0 && nattrs < 2000);
    Assert(ptype == POLICYTYPE_PARTITIONED ||
           ptype == POLICYTYPE_ENTRY);

    policy->ptype = ptype;
    policy->nattrs = nattrs;

    if (nattrs > 0)
        policy->attrs = (AttrNumber *)MemoryContextAllocZero(mcxt, nattrs * sizeof(policy->attrs[0]));

    return policy;
}	                            /* GpPolicyAlloc */


/*
 * GpPolicyFree -- Release the memory occupied by a GpPolicy object.
 */
void
GpPolicyFree(GpPolicy *policy)
{
    if (policy)
    {
        if (policy->attrs)
            pfree(policy->attrs);
        pfree(policy);
    }
}	                            /* GpPolicyFree */


/*
 * GpPolicyCopy -- Return a copy of a GpPolicy object.
 *
 * The copy is palloc'ed in the specified context.
 *
 * GpPolicyFree() should be called to free the returned policy if
 * appropriate; see GpPolicyAlloc() comments above.
 */
GpPolicy *
GpPolicyCopy(MemoryContext mcxt, const GpPolicy *src)
{
	GpPolicy  *tgt;

	if (!src)
		return NULL;

   	tgt = (GpPolicy *)MemoryContextAlloc(mcxt, sizeof(*tgt));
	memcpy(tgt, src, sizeof(*tgt));

    if (tgt->nattrs > 0)
    {
        Size    nb = src->nattrs * sizeof(src->attrs[0]);

        tgt->attrs = (AttrNumber *)MemoryContextAlloc(mcxt, nb);
        memcpy(tgt->attrs, src->attrs, nb);
    }
	return tgt;
}	                            /* GpPolicyCopy */

/*
 * GpPolicyFetch
 *
 * Looks up a given Oid in the gp_distribution_policy table.
 *
 * If found, returns an GpPolicy object (palloc'd in the specified
 * context) containing the info from the gp_distribution_policy row.
 * Otherwise returns a policy of type POLICYTYPE_ENTRY.  Never returns NULL.
 *
 * The caller is responsible for passing in a valid relation oid.  This
 * function does not check and assigns a policy of type POLICYTYPE_ENTRY
 * for any oid not found in gp_distribution_policy.
 *
 * GpPolicyFree() should be called to free the returned policy if
 * appropriate; see GpPolicyAlloc() comments above.
 */
GpPolicy *
GpPolicyFetch(MemoryContext mcxt, Oid tbloid)
{
	GpPolicy  *policy = NULL;	/* The result */
	Relation	gp_policy_rel;
	ScanKeyData policy_key[1];
	SysScanDesc gp_policy_scan;
	HeapTuple	gp_policy_tuple = NULL;

	/*
	 * If qExec or utility mode, caller should access the table locally.
	 */
	if (Gp_role != GP_ROLE_DISPATCH)
        return GpPolicyAlloc(mcxt, POLICYTYPE_ENTRY, 0);

	/*
	 * We need to read the gp_distribution_policy table
	 */
	gp_policy_rel = heap_open(GpPolicyRelationId, AccessShareLock);

	/*
	 * Set up a ScanKey for the value of the localoid field
	 */
	ScanKeyInit(&policy_key[0], Anum_gp_policy_localoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(tbloid));

	/*
	 * Begin the scan
	 */						
	gp_policy_scan = systable_beginscan(gp_policy_rel,
										 GpPolicyLocalOidIndexId,
										 true,
										 SnapshotNow,
										 1,
										 policy_key);

	/*
	 * Read first (and only ) tuple
	 */
	gp_policy_tuple = systable_getnext(gp_policy_scan);
	if (HeapTupleIsValid(gp_policy_tuple))
	{
		bool		isNull;
		Datum		attr;
		int			i,
					nattrs = 0;
		int16	   *attrnums = NULL;

		/*
		 * Get the attributes on which to partition.
		 */
		attr = heap_getattr(gp_policy_tuple, Anum_gp_policy_attrnums, gp_policy_rel->rd_att, &isNull);
		
		/*
		 * Get distribution keys only if this table has a policy.
		 */
		if(!isNull)
		{
			extract_INT2OID_array(attr, &nattrs, &attrnums);
			Assert(nattrs >= 0);
		}
			
		/* Create an GpPolicy object. */
        policy = GpPolicyAlloc(mcxt, POLICYTYPE_PARTITIONED, nattrs);
		for (i = 0; i < nattrs; i++)
			policy->attrs[i] = attrnums[i];
	}

	/*
	 * Cleanup the scan and relation objects.
	 */
	systable_endscan(gp_policy_scan);
	heap_close(gp_policy_rel, AccessShareLock);

	/* Interpret absence of a valid policy row as POLICYTYPE_ENTRY */
	if (policy == NULL)
        policy = GpPolicyAlloc(mcxt, POLICYTYPE_ENTRY, 0);

	return policy;
}                               /* GpPolicyFetch */


/*
 * Extract len and pointer to buffer from an int16[] (vector) Datum
 * representing a PostgreSQL INT2OID type.
 */
static void
extract_INT2OID_array(Datum array_datum, int *lenp, int16 **vecp)
{
	ArrayType  *array_type;

	Assert(lenp != NULL);
	Assert(vecp != NULL);

	array_type = DatumGetArrayTypeP(array_datum);
	Assert(ARR_NDIM(array_type) == 1);
	Assert(ARR_ELEMTYPE(array_type) == INT2OID);
	Assert(ARR_LBOUND(array_type)[0] == 1);
	*lenp = ARR_DIMS(array_type)[0];
	*vecp = (int16 *) ARR_DATA_PTR(array_type);

	return;
}

/*
 * Sets the policy of a table into the gp_distribution_policy table
 * from a GpPolicy structure.
 *
 */
void
GpPolicyStore(Oid tbloid, const GpPolicy *policy)
{
	Relation	gp_policy_rel;
	HeapTuple	gp_policy_tuple = NULL;

	ArrayType  *attrnums;

	char		nulls[2];
	Datum		values[2];

	Insist(policy->ptype == POLICYTYPE_PARTITIONED);

    /*
     * Open and lock the gp_distribution_policy catalog.
     */
	gp_policy_rel = heap_open(GpPolicyRelationId, RowExclusiveLock);

	/*
	 * Convert C arrays into Postgres arrays.
	 */
	if (policy->nattrs > 0)
	{
		int			i;
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
	values[0] = ObjectIdGetDatum(tbloid);

	if (attrnums)
		values[1] = PointerGetDatum(attrnums);
	else
		nulls[1] = 'n';

	gp_policy_tuple = heap_formtuple(RelationGetDescr(gp_policy_rel), values, nulls);

	simple_heap_insert(gp_policy_rel, gp_policy_tuple);

	/* update catalog indexes */
	CatalogUpdateIndexes(gp_policy_rel, gp_policy_tuple);

	/*
     * Close the gp_distribution_policy relcache entry without unlocking.
     * We have updated the catalog: consequently the lock must be held until
     * end of transaction.
     */
    heap_close(gp_policy_rel, NoLock);
}                               /* GpPolicyStore */

/*
 * Sets the policy of a table into the gp_distribution_policy table
 * from a GpPolicy structure.
 *
 */
void
GpPolicyReplace(Oid tbloid, const GpPolicy *policy)
{
	Relation	gp_policy_rel;
	HeapTuple	gp_policy_tuple = NULL;
	ScanKeyData policy_key[1];
	SysScanDesc gp_policy_scan;

	ArrayType  *attrnums;

	char		nulls[2];
	Datum		values[2];
	char		repl[2];

	Insist(policy->ptype == POLICYTYPE_PARTITIONED);

    /*
     * Open and lock the gp_distribution_policy catalog.
     */
	gp_policy_rel = heap_open(GpPolicyRelationId, RowExclusiveLock);

	/*
	 * Convert C arrays into Postgres arrays.
	 */
	if (policy->nattrs > 0)
	{
		int			i;
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
	values[0] = ObjectIdGetDatum(tbloid);

	if (attrnums)
		values[1] = PointerGetDatum(attrnums);
	else
		nulls[1] = 'n';
		
	repl[0] = ' ';
	repl[1] = 'r';

	

	/*
	 * Set up a ScanKey for the value of the localoid field
	 */
	ScanKeyInit(&policy_key[0], Anum_gp_policy_localoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(tbloid));

	/*
	 * Begin the scan
	 */						
	gp_policy_scan = systable_beginscan(gp_policy_rel,
										 GpPolicyLocalOidIndexId,
										 true,
										 SnapshotNow,
										 1,
										 policy_key);

	/*
	 * Read first (and only ) tuple
	 */
	gp_policy_tuple = systable_getnext(gp_policy_scan);
	if (HeapTupleIsValid(gp_policy_tuple))
	{
		
		HeapTuple newtuple = heap_modifytuple(gp_policy_tuple, RelationGetDescr(gp_policy_rel), values,
								nulls, repl);
								
		simple_heap_update(gp_policy_rel, &gp_policy_tuple->t_self, newtuple);
		/* update catalog indexes */
		CatalogUpdateIndexes(gp_policy_rel, newtuple);
		heap_freetuple(newtuple);
	}
	else
	{
		gp_policy_tuple = heap_formtuple(RelationGetDescr(gp_policy_rel), values, nulls);
		simple_heap_insert(gp_policy_rel, gp_policy_tuple);
		/* update catalog indexes */
		CatalogUpdateIndexes(gp_policy_rel, gp_policy_tuple);
	}
	
	

	/*
	 * Cleanup the scan and relation objects.
	 */
	systable_endscan(gp_policy_scan);
	
	

	/*
     * Close the gp_distribution_policy relcache entry without unlocking.
     * We have updated the catalog: consequently the lock must be held until
     * end of transaction.
     */
    heap_close(gp_policy_rel, NoLock);
}                               /* GpPolicyReplace */


/*
 * Removes the policy of a table from the gp_distribution_policy table
 *
 * Calers must check that there actually *is* a policy for the relation.
 */
void
GpPolicyRemove(Oid tbloid)
{
	Relation	gp_policy_rel;
	HeapTuple	gp_policy_tuple = NULL;
	ScanKeyData	key;
	SysScanDesc	scan;


    /*
     * Open and lock the gp_distribution_policy catalog.
     */
	gp_policy_rel = heap_open(GpPolicyRelationId, RowExclusiveLock);

	ScanKeyInit(&key,
				Anum_gp_policy_localoid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(tbloid));

	scan = systable_beginscan(gp_policy_rel, GpPolicyLocalOidIndexId, true,
							  SnapshotNow, 1, &key);

	gp_policy_tuple = systable_getnext(scan);
	if(!HeapTupleIsValid(gp_policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("distribution policy for relation \"%d\" does not exist",
						tbloid)));

	/* Delete the policy entry from the catalog. */
	simple_heap_delete(gp_policy_rel, &gp_policy_tuple->t_self);

	/* update catalog indexes */
	CatalogUpdateIndexes(gp_policy_rel, gp_policy_tuple);

	/* End the scan */
	systable_endscan(scan);

	/*
     * Close the gp_distribution_policy relcache entry without unlocking.
     * We have updated the catalog: consequently the lock must be held until
     * end of transaction.
     */
    heap_close(gp_policy_rel, NoLock);
}                               /* GpPolicyRemove */
