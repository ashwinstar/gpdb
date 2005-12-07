/*-------------------------------------------------------------------------
 *
 * cdbcat.c
 *	  Provides routines for reading info from mpp schema tables
 *
 * Copyright (c) 2005, greenplum inc
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
#include "cdb/cdbvars.h"		/* Mpp_role */
#include "catalog/pg_type.h"
#include "utils/array.h"
static void extract_INT2OID_array(Datum array_datum, int *lenp, int16 **vecp);


/*
 * MppPolicyCopy -- Return a copy of a MppPolicy object.
 *
 * The copy is palloc'ed in the caller's context.
 */
MppPolicy *
MppPolicyCopy(const MppPolicy *src)
{
	MppPolicy  *tgt;
	size_t		nb;

	if (!src)
		return NULL;
	nb = sizeof(*src) + src->nattrs * sizeof(src->attrs[0]);
	tgt = (MppPolicy *) palloc(nb);
	memcpy(tgt, src, nb);
	return tgt;
}	/* MppPolicyCopy */

/*
 * Returns the policy of a table as determined from the mpp.policy table
 * in a MppPolicy structure allocated in the current memory context.
 *
 * The caller is responsible for passing in a valid relation oid.  This
 * function does not check and assigns a policy of type POLICYTYPE_ENTRY
 * for any oid not found in .policy.
 */
MppPolicy *
get_table_policy(Oid tbloid)
{
	MppPolicy  *policy = NULL;	/* The result */
	Relation	mpp_policy_rel;
	ScanKeyData policy_key[1];
	SysScanDesc mpp_policy_scan;
	HeapTuple	mpp_policy_tuple = NULL;

	/*
	 * Skip if qExec or utility mode.
	 */
	if (Mpp_role != MPP_ROLE_DISPATCH)
		return NULL;

	/*
	 * We need to read the mpp.policy table
	 */

	
	/*
	 * Then, open the table and and find the oid of the index
	 */
	 
	mpp_policy_rel = heap_open(MppPolicyRelationId, AccessShareLock);

	/*
	 * Set up a ScanKey for the value of the localoid field
	 */
	ScanKeyInit(&policy_key[0], Anum_mpp_policy_localoid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(tbloid));

	/*
	 * Begin the scan
	 */						 
	mpp_policy_scan = systable_beginscan(mpp_policy_rel, 
										 MppPolicyLocalOidIndexId, 
										 true,
										 SnapshotNow, 
										 1, 
										 policy_key);

	/*
	 * Read first (and only ) tuple
	 */
	mpp_policy_tuple = systable_getnext(mpp_policy_scan);
	if (HeapTupleIsValid(mpp_policy_tuple))
	{
		bool		isNull;
		Datum		attr;
		int			i,
					nattrs = 0;
		int16	   *attrnums = NULL;
		size_t		sz;

		/*
		 * Get the attributes on which to partition.
		 */
		attr = heap_getattr(mpp_policy_tuple, Anum_mpp_policy_attrnums, mpp_policy_rel->rd_att, &isNull);
		Assert(!isNull);
		extract_INT2OID_array(attr, &nattrs, &attrnums);
		Assert(nattrs >= 0);

		sz = sizeof(MppPolicy) + nattrs * sizeof(policy->attrs[0]);
		policy = (MppPolicy *) palloc(sz);
		policy->ptype = POLICYTYPE_PARTITIONED;
		policy->nattrs = nattrs;
		for (i = 0; i < nattrs; i++)
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
	if (policy == NULL)
	{
		policy = palloc(sizeof(MppPolicy));
		policy->ptype = POLICYTYPE_ENTRY;
		policy->nattrs = 0;
	}

	return policy;
}




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
 * Sets the policy of a table into the mpp.policy table
 * from a MppPolicy structure.
 *
 */
void
set_table_policy(Oid tbloid, MppPolicy *policy)
{

	Relation	mpp_policy_rel;

	/*
	 * Oid				 mpp_policy_index_oid; ScanKeyData	   policy_key[1];
	 * SysScanDesc	   mpp_policy_scan;
	 */
	HeapTuple	mpp_policy_tuple = NULL;

	ArrayType  *attrnums;

	char		nulls[2];
	Datum		values[2];



	Assert(policy->nattrs >= 0);
	Assert(policy->ptype == POLICYTYPE_PARTITIONED);


	mpp_policy_rel = heap_open(MppPolicyRelationId, RowExclusiveLock);



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

	mpp_policy_tuple = heap_formtuple(RelationGetDescr(mpp_policy_rel), values, nulls);

	simple_heap_insert(mpp_policy_rel, mpp_policy_tuple);

	/* update catalog indexes */
	CatalogUpdateIndexes(mpp_policy_rel, mpp_policy_tuple);


	heap_close(mpp_policy_rel, RowExclusiveLock);








}
