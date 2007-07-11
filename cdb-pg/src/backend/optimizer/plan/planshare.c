/* 
 * planshare.c
 * 		Plan shared plan
 * Copyright (c) 2007, greenplum inc
 */

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/subselect.h"
#include "optimizer/planshare.h"

#include "cdb/cdbgroup.h"					/* cdbpathlocus_collocates() */
#include "cdb/cdbpath.h"
#include "cdb/cdbsetop.h"					/* make_motion... routines */
#include "cdb/cdbmutate.h" 					/* add_slice_to_motion */
#include "cdb/cdbvars.h"

/*
 * Plan sharing:
 *
 * OLAP components that need to share a common subplan will need to firsetermine
 * that HOW to share the subplan, by calling add_sharenode implemented in this file.
 *
 * Once we decided how to share a subplan, then for each consumer of the subplan,
 * it need to condition_input the common subplan.  condition_input will put necessary
 * reordering (sort) and scanner (ShareInputScan) on top of the common subplan so it
 * can be used.  Condition input need to do things differently depends on whether we
 * are not sharing at all, share a material/sort, or share a motion.  The polymorphism
 * is achieved through ISHAREPLAN interface.  Very much like an OO interface/implementation,
 * the interface is implemented in three different ways, shareplan_simple (not shared),
 * shareplan_ms (material or sort) and shareplan_motion.
 */

Plan *condition_input(
		ISHAREPLAN *iSharePlan,
		PlannerInfo *root,
		Plan *input_plan,
		int partkey_len,
		AttrNumber *partkey_attrs,
		List *sortclause,								 
		CdbPathLocus input_locus,
		List **pathkeys_ptr)
{
	Plan *new_plan;
	List *pathkeys;
	List *sort_pathkeys = NULL;	
	
	Assert( input_plan);
	Assert( !CdbPathLocus_IsNull(input_locus));
	Assert( pathkeys_ptr && (*pathkeys_ptr == NIL || IsA(*pathkeys_ptr, List)) );
	Assert( iSharePlan);
	
	if(iSharePlan->newplan_for_subplan)
		new_plan = (*(iSharePlan->newplan_for_subplan))(iSharePlan->ctxt, root, input_plan);
	else
		new_plan = (Plan *) input_plan;

	pathkeys = *pathkeys_ptr;
	
	if ( sortclause != NIL )
	{
		sort_pathkeys = make_pathkeys_for_sortclauses(sortclause, new_plan->targetlist);
		sort_pathkeys = canonicalize_pathkeys(root, sort_pathkeys);
		Assert(sort_pathkeys != NULL);
	}
	
	if ( partkey_len == 0 ) /* Plan for single process locus. */
	{
		return (*(iSharePlan->add_sort_motion_for_single)) (iSharePlan->ctxt,
															root, 
															new_plan, 
															NULL,
															sortclause, 
															sort_pathkeys,
															input_locus, 
															pathkeys_ptr
				);
	}
	else  /* Plan for hash distributed locus. */
	{

		List *dist_keys = NIL;
		List *dist_pathkeys = NIL;
		List *dist_exprs = NIL;
		ListCell *lc;
		int n = partkey_len;
		
		foreach (lc, sortclause)
		{
			if ( 0 >= n-- ) break;
			dist_keys = lappend(dist_keys, lfirst(lc));
		}
		dist_pathkeys = make_pathkeys_for_sortclauses(dist_keys, new_plan->targetlist);
		dist_pathkeys = canonicalize_pathkeys(root, dist_pathkeys);
		
		if ( ! cdbpathlocus_collocates(input_locus, dist_pathkeys ) )
		{
			/* Preexisting input_locus won't do.  Redistribute. */
			foreach (lc, dist_keys)
			{
				SortClause *sc = (SortClause*)lfirst(lc);
				TargetEntry *tle =  get_sortgroupclause_tle(sc, new_plan->targetlist);
				dist_exprs = lappend(dist_exprs, tle->expr);
			}
		}

		return (*(iSharePlan->add_sort_motion_for_dist)) (
														iSharePlan->ctxt,
														root,
														new_plan,
														dist_exprs,
														sortclause,
														sort_pathkeys,
														input_locus,
														pathkeys_ptr
				);
	}
}


/* Routine for simple (duplicate subplan) case */
static Plan * add_sort_motion_single_simple(
		void *ctxt,
		PlannerInfo *root, 
		Plan *inputplan,
		List *dist_exprs,
		List *sortclause,
		List *sort_pathkeys,
		CdbPathLocus inputlocus,
		List **pathkeys_ptr
		)
{
	Plan *new_plan = inputplan;
	
	double motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
					gp_motion_cost_per_row :
					2.0 * cpu_tuple_cost;

	Assert(dist_exprs == NULL);

	/* Assure sort order first */
	if(sort_pathkeys != NULL)
	{
		if(!pathkeys_contained_in(sort_pathkeys, *pathkeys_ptr))
		{
			new_plan = (Plan *) make_sort_from_sortclauses(root, sortclause, new_plan);
			*pathkeys_ptr = sort_pathkeys;
			mark_sort_locus(new_plan);
		}
	}

	/* bring to single locus */
	if( CdbPathLocus_IsPartitioned(inputlocus))
	{
		new_plan = (Plan *) make_motion_gather_to_QE(new_plan, (*pathkeys_ptr != NULL));
		new_plan->total_cost += motion_cost_per_row * new_plan->plan_rows;
	}

	Assert(new_plan->flow); 
	return new_plan;
}

static Plan * add_sort_motion_dist_simple(
		void *ctxt,
		PlannerInfo *root, 
		Plan *inputplan,
		List *dist_exprs,
		List *sortclause,
		List *sort_pathkeys,
		CdbPathLocus inputlocus,
		List **pathkeys_ptr
		)
{
	Plan *new_plan = inputplan;
	double motion_cost_per_row = (gp_motion_cost_per_row > 0.0) ?
					gp_motion_cost_per_row :
					2.0 * cpu_tuple_cost;

	/* simple case does not need context */
	Assert(!ctxt);

	if(dist_exprs)
	{
		new_plan = (Plan *) make_motion_hash(root, new_plan, dist_exprs);
		new_plan->total_cost += motion_cost_per_row * new_plan->plan_rows;
		*pathkeys_ptr = NIL; /* no longer sorted */
		Assert(new_plan->flow);
	}

	if(sortclause != NIL)
	{
		if(! pathkeys_contained_in(sort_pathkeys, *pathkeys_ptr))
		{
			new_plan = (Plan *) make_sort_from_sortclauses(root, sortclause, new_plan);
			*pathkeys_ptr = sort_pathkeys;
			mark_sort_locus(new_plan);
		}
	}

	return new_plan;
}
	
ISHAREPLAN shareplan_simple =  
{
	NULL, /* No need to add new node on top for simple case */
	add_sort_motion_single_simple,
	add_sort_motion_dist_simple,
	NULL, NULL /* No need for context */
};

static ShareInputScan *make_shareinputscan(Plan *inputplan) 
{
	ShareInputScan *sisc = NULL;
	Plan *oldplan = NULL;

	sisc = makeNode(ShareInputScan);

	oldplan = (Plan *) inputplan;
	Assert(IsA(inputplan, Material) || IsA(inputplan, Sort));

	sisc->plan.targetlist = copyObject(oldplan->targetlist);
	sisc->plan.lefttree = oldplan;
	sisc->plan.flow = NULL;

	if(IsA(inputplan, Material))
	{
		Material *ma = (Material *) inputplan;
		Assert(ma->share_type == SHARE_MATERIAL && ma->share_id >= 0);
		sisc->share_type = SHARE_MATERIAL;
		sisc->share_id = ma->share_id;
	}
	else 
	{
		Sort *s = (Sort *) inputplan;
		Assert(s->share_type == SHARE_SORT && s->share_id >= 0);
		sisc->share_type = SHARE_SORT;
		sisc->share_id = s->share_id;
	}

	sisc->plan.qual = NIL;
	sisc->plan.righttree = NULL;

	sisc->plan.startup_cost = oldplan->startup_cost;
	sisc->plan.total_cost = oldplan->total_cost;
	sisc->plan.plan_rows = oldplan->plan_rows;
	sisc->plan.plan_width = oldplan->plan_width;
	
	return sisc;
}

/* Routine for share Material or Sort */

/* Create new plan for subplan.  For share Material or Sort, we add a ShareInputScan node on top.
 * The ScanInputScan get its flow from underlying material or sort.
 */
static Plan * newplan_for_subplan_share_ms(void *ctxt, PlannerInfo *root, Plan *inputplan)
{
	ShareInputScan *sisc = make_shareinputscan(inputplan); 
	
	/* no need for context */
	Assert(!ctxt);

	/* Copy the flow.  ShareInputScan will not change the flow of its subplan */
	sisc->plan.flow = copyObject(inputplan->flow); 
	return (Plan *) sisc;
}

ISHAREPLAN shareplan_ms = 
{
	newplan_for_subplan_share_ms,
	/* Add sort motion for share material/sort is the same as simple */
	add_sort_motion_single_simple,
	add_sort_motion_dist_simple,
	NULL, NULL /* no need for context */
};

#if 0
/* Routine and context for share Motion */
typedef struct ShareMotionContextSlice
{
	MotionSliceEntry *slice;
	int sliceIdx;
	List *sort_pathkeys;
	List *dist_exprs;
	bool isFocus;
} ShareMotionContextSlice ;

typedef struct ShareMotionContext
{
	List *siblings;
} ShareMotionContext;

static ShareMotionContext share_motion_context;
static void init_share_motion_context(void *ctxt)
{
	ShareMotionContext *smctxt = (ShareMotionContext *) ctxt;
	smctxt->siblings = NULL;
}

/* Helper function.
 * find the focusing motion slice entry, if there is one.
 */
static ShareMotionContextSlice *find_smc_focus_slice(ShareMotionContext *ctxt)
{
	ListCell *lc;
	foreach(lc, ctxt->siblings)
	{
		ShareMotionContextSlice *slice = lfirst(lc);
		if(slice->isFocus)
			return slice;
	}
	return NULL;
}

/* Helper function.
 * find the distribute motion slice entry, if there is one.
 */
static ShareMotionContextSlice *find_smc_dist_slice(ShareMotionContext *ctxt, List *dist_exprs)
{
	ListCell *lc;
	foreach(lc, ctxt->siblings)
	{
		ShareMotionContextSlice *slice = lfirst(lc);
		if(!slice->isFocus && equal(slice->dist_exprs, dist_exprs))
			return slice;
	}
	return NULL;
}

/* Create new plan for subplan. For share motion, we add a ShareInputScan on top.
 * At this point, we do not know anything about the flow yet.   
 */
static Plan * newplan_for_subplan_share_motion(void *ctxt, PlannerInfo *root, Node *inputplan)
{
	ShareInputScan *sisc = make_shareinputscan(inputplan, false);

	Assert(ctxt);
	return (Plan *) sisc; 
}

/* 
 * Plan share for focus flow.
 *
 * 1. we need to check if we already has a focus motion slice.  We install one if
 *    necessary.
 * 2. now, we know the flow info.  Update the flow in shareinputscan node.
 * 3. we check if we need to sort.  
 */
static Plan * addsm_for_single_share_motion(
			void *ctxt,
			PlannerInfo *root, 
			Plan *oldplan,
			List *dist_exprs,
			List *sortclause,
			List *sort_pathkeys,
			CdbPathLocus inputlocus,
			List **pathkeys_ptr
		)
{
	ShareMotionContext *smctxt = (ShareMotionContext *) ctxt;
	ShareInputScan *sisc = NULL;
	ShareMotionContextSlice *slice = NULL;
	Material *sharedMaterial = NULL;
	Motion *sharedMotion = NULL;

	Assert(smctxt);
	Assert(IsA(oldplan, ShareInputScan));
	Assert(!dist_exprs);

	sisc = (ShareInputScan *) oldplan;

	sharedMaterial = (Material *) oldplan->lefttree;
	Assert(sharedMaterial && IsA(sharedMaterial, Material));

	sharedMotion = (Motion *) sharedMaterial->plan.lefttree;
	Assert(sharedMotion && IsA(sharedMotion, Motion));

	slice = find_smc_focus_slice(smctxt);
	
	/* Set up the focus slice in the motion if we have not done that */
	if(!slice)
	{
		int *outSegIdx = (int *) palloc(sizeof(int));
		slice = palloc(sizeof(ShareMotionContextSlice));

		outSegIdx[0] = gp_singleton_segindex; 
		add_slice_to_motion(sharedMotion, MOTIONTYPE_FIXED, NULL, 1, outSegIdx);

		slice->sliceIdx = list_length(sharedMotion->sliceEntries) -1;
		slice->slice = list_nth(sharedMotion->sliceEntries, slice->sliceIdx);
		slice->sort_pathkeys = NULL;
		slice->dist_exprs = NULL;
		slice->isFocus = true;

		smctxt->siblings = lappend(smctxt->siblings, slice);
	}

	/* Update flow.
	 * For focus, shareinputscan does not need to filter on hash functions.
	 */
	sisc->plan.flow = copyObject(get_motion_flow(sharedMotion, slice->sliceIdx));
	
	/* Next, plan sort if necessary. */
	if(sort_pathkeys!=NULL)
	{
		Plan *sortplan = (Plan *) make_sort_from_sortclauses(root, sortclause, (Plan *)sisc);
		*pathkeys_ptr = sort_pathkeys;
		mark_sort_locus(sortplan);
		return sortplan;
	}

	return (Plan *) sisc;
}

/* 
 * Plan share for distribute flow.
 *
 * 1. we need to check if we already has a good motion slice.  We install one if
 *    necessary.
 * 2. we check if we need to sort.  
 * 3. we need to fix the hash function in shareinputscan, because there may be tuples
 *    that is not intented for this shareinputscan.  And by now we know the flow, so update
 *    the flow of shareinputscan.
 */
static Plan * addsm_for_dist_share_motion(
			void *ctxt,
			PlannerInfo *root, 
			Plan *oldplan,
			List *dist_exprs,
			List *sortclause,
			List *sort_pathkeys,
			CdbPathLocus inputlocus,
			List **pathkeys_ptr
		)
{
	ShareMotionContext *smctxt = (ShareMotionContext *) ctxt;
	ShareMotionContextSlice *slice = find_smc_dist_slice(smctxt, dist_exprs);

	ShareInputScan *sisc = NULL;
	Material *sharedMaterial = NULL;
	Motion *sharedMotion = NULL;

	Assert(smctxt);
	Assert(IsA(oldplan, ShareInputScan));
	sisc = (ShareInputScan *) oldplan;

	Assert(oldplan->lefttree && IsA(oldplan->lefttree, Material));
	sharedMaterial = (Material *) oldplan->lefttree;

	Assert(IsA(sharedMaterial->plan.lefttree, Motion));
	sharedMotion = (Motion *) sharedMaterial->plan.lefttree;

	if(!slice)
	{
		slice = palloc(sizeof(ShareMotionContextSlice));
		Assert(dist_exprs);

		add_slice_to_motion(sharedMotion, MOTIONTYPE_HASH, dist_exprs, 0, NULL);

		slice->sliceIdx = list_length(sharedMotion->sliceEntries) - 1;
		slice->slice = list_nth(sharedMotion->sliceEntries, slice->sliceIdx);
		slice->sort_pathkeys = NULL;
		slice->dist_exprs = dist_exprs;
		slice->isFocus = false;

		smctxt->siblings = lappend(smctxt->siblings, slice);
	}

	/* Now update sisc flow */
	Assert(get_motion_flow(sharedMotion, slice->sliceIdx));
	sisc->plan.flow = copyObject(get_motion_flow(sharedMotion, slice->sliceIdx));
	
	/* need to set up distexpr in sisc, so we can later filter on hash */
	sisc->hashExpr = copyObject(slice->slice->hashExpr);
	sisc->hashDataTypes = copyObject(slice->slice->hashDataTypes);

	/* Now let's add the sort order if necessary */
	if(sort_pathkeys!=NULL)
	{
		Plan *sortplan = (Plan *) make_sort_from_sortclauses(root, sortclause, (Plan *)sisc);
		*pathkeys_ptr = sort_pathkeys;
		mark_sort_locus(sortplan);
		return sortplan;
	}

	return (Plan *) sisc;
}

/* 
 * XXX Obviously using a static share_motion_context is NOT thread safe (or even
 * not safe if another share motion is run before we finish planning the current one).
 * but postgres is not threaded anyway and we finish one share planning before starts
 * another.  In case needed, this can be fixed alloc a context for each share planning.
 */
ISHAREPLAN shareplan_motion = 
{
	newplan_for_subplan_share_motion,
	addsm_for_single_share_motion,
	addsm_for_dist_share_motion,
	init_share_motion_context, &share_motion_context,
};

#endif

/* XXX Just add a material.  Let's just do share material for now.  Once
 * I fixed the deadlock issue, we will revisist share sort
 */
ShareType add_sharenode(
		PlannerInfo *root, 
		Plan **subplan,
		List **subplan_pathkeys,
		CdbPathLocus subplan_locus,
		void * ctxt,
		int nsharer,
		GET_PARTKEY_LEN fget_pklen,
		GET_SORT_CLAUSE fget_sc
		)
{
	Query *qry = root->parse;

	Assert(subplan && *subplan);
	Assert(ctxt);
	Assert(qry); 

	if(!IsA(*subplan, Material))
	{
		Plan *oldplan = *subplan; 
		Path matpath;
		Material *ma = make_material(oldplan);
		*subplan = (Plan *) ma;

		cost_material(&matpath, oldplan->total_cost, oldplan->plan_rows, oldplan->plan_width);
		(*subplan)->startup_cost = matpath.startup_cost;
		(*subplan)->total_cost = matpath.total_cost;
		(*subplan)->plan_rows = oldplan->plan_rows;
		(*subplan)->plan_width = oldplan->plan_width;
		(*subplan)->dispatch = oldplan->dispatch;
		(*subplan)->flow = copyObject(oldplan->flow); 

		ma->share_type = SHARE_MATERIAL;
		ma->share_id = list_length(qry->sharedNodes); 
		qry->sharedNodes = lappend(qry->sharedNodes, *subplan);

		/* XXX Do we need to do parameter kluge ? */
		/*
		(*subplan)->extParam = bms_copy(oldplan->extParam);
		(*subplan)->allParam = bms_cppy(oldplan->allParam);
		*/
		return SHARE_MATERIAL;
	}
	else
	{
		Material *ma = (Material *) *subplan;
		Assert(ma->share_type == SHARE_NOTSHARED);

		ma->share_type = SHARE_MATERIAL;

		ma->share_id = list_length(qry->sharedNodes); 
		qry->sharedNodes = lappend(qry->sharedNodes, *subplan);

		return SHARE_MATERIAL;
	}

	return SHARE_NOTSHARED;
}
