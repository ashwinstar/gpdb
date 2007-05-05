/* 
 * planshare.h
 * 		Prototypes and data structures for plan sharing
 * Copyright (c) 2007, greenplum inc
 */

#ifndef _PLANSHARE_H_
#define _PLANSHARE_H_

/* functor for planning share subplan */
typedef void (*INIT_SHARE_CTXT) (void *);
typedef Plan* (*NEWPLAN_FOR_SUBPLAN) (
		void *ctxt,
		PlannerInfo *root, 
		Plan *oldplan);
typedef Plan* (*ADD_SORT_MOTION) (	
		void *ctxt,
		PlannerInfo *root, 
		Plan *oldplan,
		List *dist_exprs,
		List *sortclause,
		List *sort_pathkeys,
		CdbPathLocus inputlocus,
		List **pathkeys_ptr);

typedef int (*GET_PARTKEY_LEN) (void *ctxt, int nth);
typedef List * (*GET_SORT_CLAUSE) (void *ctxt, int nth);

typedef struct ISHAREPLAN
{
	/* Method pointers */
	/* function that adds a node on top of the old plan */
	NEWPLAN_FOR_SUBPLAN newplan_for_subplan;

	/* function that add a sort or motion (or both) to focus locus
	 * and enforcing sort order 
	 */
	ADD_SORT_MOTION add_sort_motion_for_single;

	/* function that add a sort or motion (or both) to redistribute
	 */
	ADD_SORT_MOTION add_sort_motion_for_dist;

	/* Intialized share context */
	INIT_SHARE_CTXT init_ctxt;
	/* Data pointer */
	void *ctxt;
} ISHAREPLAN;

extern ISHAREPLAN shareplan_simple; 
extern ISHAREPLAN shareplan_ms; 
extern ISHAREPLAN shareplan_motion; 

/* Add a shared node on top of the common subplan and return the sharing type
 * Params:
 * 	root:
 *	subplan: _INOUT_ param.  return modified tree, with the sharing node on top
 *  subplan_pathkeys: _INOUT_ param.  return new sort order
 *	subplan_locus:
 * 	ctxt: A context
 *  nsharer: number of sharer.
 *  fget_pklen and fget_sc: routines to retrieve info from context
 */
extern ShareNode_ShareType add_sharenode(
		PlannerInfo *root, 
		Plan **subplan,
		List **subplan_pathkeys,
		CdbPathLocus subplan_locus,
		void * ctxt,
		int nsharer,
		GET_PARTKEY_LEN fget_pklen,
		GET_SORT_CLAUSE fget_sc
		);

/* Condition the input plan by adding plan operators required to 
 * satisfy the given partitioning (redistribution) and ordering
 * requirements.
 *
 * Note pathkeys_ptr is required (points to the input pathkeys) and
 * will be reset to point to the resulting pathkeys on return.
 */
extern Plan *condition_input(
	 	ISHAREPLAN *iSharePlan,	
		PlannerInfo *root,
		Plan *input_plan,
		int partkey_len,
		AttrNumber *partkey_attrs,
		List *sortclause,								 
		CdbPathLocus input_locus,
		List **pathkeys_ptr);

#endif /* _PLANSHARE_H_ */
