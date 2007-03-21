/*-------------------------------------------------------------------------
 *
 * gp_policy.h
 *	  definitions for the gp_distribution_policy catalog table
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef _GP_POLICY_H_
#define _GP_POLICY_H_


/*
 * Defines for gp_policy
 */
#define GpPolicyRelationName		"gp_distribution_policy"

#define GpPolicyRelationId  5002

CATALOG(gp_distribution_policy,5002) BKI_WITHOUT_OIDS
{
	Oid			localoid;
	int2		attrnums[1];
} FormData_gp_policy;

#define Natts_gp_policy			2
#define Anum_gp_policy_localoid	1
#define Anum_gp_policy_attrnums	2

/*
 * MppPolicyType represents a type of policy under which a relation's
 * tuples may be assigned to a component database.
 */
typedef enum MppPolicyType
{
	POLICYTYPE_UNDEFINED,
	POLICYTYPE_PARTITIONED,		/* Tuples partitioned onto segment database. */
	POLICYTYPE_ENTRY			/* Tuples stored on enty database. */
} MppPolicyType;

/*
 * MppPolicy represents a Greenplum DB data distribution policy. The ptype field
 * is always significant.  Other fields may be specific to a particular
 * type.
 *
 * A MppPolicy is typically palloc'd with space for nattrs integer
 * attribute numbers (attrs) in addition to sizeof(MppPolicy).
 */
typedef
struct MppPolicy
{
	MppPolicyType ptype;

	/* These fields apply to POLICYTYPE_PARTITIONED. */
	int			nattrs;
	AttrNumber	attrs[1];		/* the first of nattrs attribute numbers.  */
} MppPolicy;

/*
 * MppPolicyCopy -- Return a copy of a MppPolicy object.
 *
 * The copy is palloc'ed in the specified context.
 */
MppPolicy *
MppPolicyCopy(MemoryContext mcxt, const MppPolicy *src);

/*
 * MppPolicyFetch
 *
 * Looks up a given Oid in the gp_distribution_policy table.
 * If found, returns an MppPolicy object (palloc'd in the specified
 * context) containing the info from the gp_distribution_policy row.
 * Else returns NULL.
 *
 * The caller is responsible for passing in a valid relation oid.  This
 * function does not check and assigns a policy of type POLICYTYPE_ENTRY
 * for any oid not found in gp_distribution_policy.
 */
MppPolicy *
MppPolicyFetch(MemoryContext mcxt, Oid tbloid);

/*
 * MppPolicyStore: sets the MppPolicy for a table.
 */
void
MppPolicyStore(Oid tbloid, const MppPolicy *policy);

void
MppPolicyReplace(Oid tbloid, const MppPolicy *policy);


#endif /*_GP_POLICY_H_*/
