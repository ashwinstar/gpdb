#ifndef _MPP_POLICY_H_
#define _MPP_POLICY_H_


/*
 * Defines for mpp_policy
 */
#define MppPolicyRelationName		"mpp_distribution_policy"

#define MppPolicyRelationId  5002

CATALOG(mpp_distribution_policy,5002) BKI_WITHOUT_OIDS
{
	Oid			localoid;
	int2		attrnums[1];
} FormData_mpp_policy;

#define Natts_mpp_policy			2
#define Anum_mpp_policy_localoid	1
#define Anum_mpp_policy_attrnums	2

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
 * MppPolicy represents a MPP data distribution policy. The ptype field
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
	AttrNumber	attrs[0];		/* the first of nattrs attribute numbers.  */
} MppPolicy;

/*
 * MppPolicyCopy -- Return a copy of a MppPolicy object.
 *
 * The copy is palloc'ed in the caller's context.
 */
MppPolicy *
			MppPolicyCopy(const MppPolicy *src);

/*
 * get_table_policy: returns a palloc'd MppPolicy for a table oid.
 */
extern MppPolicy *get_table_policy(Oid tbloid);

/*
 * set_table_policy: sets the MppPolicy for a table.
 */
extern void set_table_policy(Oid tbloid, MppPolicy *policy);


#endif /*_MPP_POLICY_H_*/
