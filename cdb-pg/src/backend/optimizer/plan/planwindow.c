/*-------------------------------------------------------------------------
 *
 * planwindow.c
 *	  Planning for window queries.
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $ID$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_window.h"
#include "nodes/makefuncs.h"
#include "nodes/print.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/planshare.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/debugbreak.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbgroup.h"					/* cdbpathlocus_collocates() */
#include "cdb/cdbllize.h"                   /* pull_up_Flow() */
#include "cdb/cdbpath.h"
#include "cdb/cdbsetop.h"					/* make_motion... routines */
#include "cdb/cdbvars.h"

typedef enum CoplanType
{
	COPLAN_WINDOW,
	COPLAN_AGG
} CoplanType;

typedef struct Coplan
{
	CoplanType	type;
	Index		varno;
	AttrNumber	last_resno;
	int			num_aggs;
	List	   *targetnames;
	List	   *targetlist;
	int			rowkey_len;
	AttrNumber *rowkey_attrs;
	int			partkey_len;
	AttrNumber *partkey_attrs;
	int			trans_space;
} Coplan;

/* WindowInfo represents a set of coplans in the final plan for a window 
 * query.  The set contains at least one of a coplan for a Window node 
 * and a coplan for an Agg node.
 *
 * Each WindowInfo controls an array of SpecInfos all of which have the same
 * partitioning set and compatible ordering keys.  The SpecInfos are stored 
 * in WindowContext's array (nspecinfos, specinfos).  A WindowSpec controls
 * numspecindex of them beginning at firstspecindex.
 *
 * Stored in the WindowContext's array (nwindowinfos, windowinfos).
 */
typedef struct WindowInfo
{
	Index	firstspecindex;
	Index	numspecindex;
	List   *sortclause;
	int		partkey_len;
	int		rowkey_len;
	int		orderkeys_offset;
	bool	needpartkey;
	bool	needauxcount;
	
	AttrNumber *partkey_attrs;
	List	   *key_list;
	
	/* coplan assembly */
	
	struct Coplan  *window_coplan;
	struct Coplan  *agg_coplan;
} WindowInfo;


/* RefInfo holds a WindowRef and describes it in the context of the overall
 * query.  During planning, the contained WindowRef refers back to the
 * RefInfo via winindex - the position (counting from 0) of the RefInfo in
 * the WindowContext's list, refinfos.
 */
typedef struct RefInfo
{
	WindowRef  *ref;
	Bitmapset  *varset;
	Index		specindex; /* index of parent SpecInfo */
	bool		isagg;
	
	/* When isagg  */
	bool		hasinvtrans;
	bool		hasprelim;
	bool		hasinvprelim;
	
	/* When iswin = !isagg */
	bool		needcount;
	Oid			winpretype;
	Oid			winfinfunc;
	
	int			trans_space;
	
	Expr	   *resultexpr; /* Result expression for upper target list */
} RefInfo;

/* SpecInfo represents a distinct, referenced WindowSpec from the query.
 * They are stored in the WindowContext's array (nspecinfos,specinfos).
 *
 * Later in planning, SpecInfo's with identical partitioning and compatible
 * ordering are grouped under a WindowInfo.
 */
typedef struct SpecInfo
{
	Index			specindex;		/* index of SpecInfo in context */
	Bitmapset	   *partset;		/* sortgrouprefs of partitioning keys */
	List		   *order;			/* ORDER BY clause */
	WindowFrame	   *frame;			/* framing clause */
	Bitmapset	   *refset;			/* indices of referencing RefInfo */
	Index			windowindex;	/* index of assigned Window node */
	int				keylevel;		/* key level of spec in Window node */
	List		   *partkey;		/* PARTITION BY clause - transient */
} SpecInfo;


/* WindowContext is the overall context for window planning.
 *
 */
typedef struct WindowContext
{
	List		   *upper_tlist;
	Bitmapset	   *upper_var_set;
	List		   *refinfos;
	
	int				nspecinfos;
	SpecInfo	   *specinfos;
	int				nwindowinfos;
	WindowInfo	   *windowinfos;

	int				rowkey_len;
	AttrNumber	   *rowkey_attrs;
	
	Index			coplan_count;
	
	bool			has_unordered_aggs;
	bool			has_deferred_window_fns;
	
	/* Map (varno,varattno) <--> (integer) for use in Bitmapsets recording
	 * use of vars from the window query's range table.
	 */
	int				max_varno;
	int			   *varattno_offsets;
	int			   *offset_varnos;
	int				offset_lim;
	
	AttrNumber	   *offset_upper_varattrnos;
	
	/* Target list for the "common subquery".
	 */
	List		   *lower_tlist;
	List		   *keyed_lower_tlist;
	Plan		   *subplan;
	CdbPathLocus	subplan_locus;
	List		   *subplan_pathkeys;
		
	/* Map (ressortgrpref) --> (resno) in lower_tlist */
	int				max_sortref;
	AttrNumber	   *sortref_resno;
	
	/* Transient state for preprocess_window_tlist */
	RefInfo		   *cur_refinfo;
} WindowContext;

/* Forward declarations (local) */

static WindowContext *newWindowContext(void);
static void deleteWindowContext(WindowContext *context);
static void build_sortref_index(List *tlist, int *max_sortref, AttrNumber **sortref_resno);
static void build_var_index(Query *parse, WindowContext *context);
static int index_of_var(Var * var, WindowContext *context);
static void preprocess_window_tlist(List *orig_tlist, WindowContext *context);
static Node * window_tlist_mutator(Node *node, WindowContext *context);
static bool window_tlist_vars_walker(Node *node, WindowContext *context);
static void inventory_window_specs(List *window_specs, WindowContext *context);
static int compare_order(List *a, List* b);
static int compare_edge(WindowFrameEdge *a, WindowFrameEdge *b);
static int compare_frame(WindowFrame *a, WindowFrame *b);
static int compare_spec_info_ptr(const void *arg1, const void *arg2);
static void make_lower_targetlist(Query *parse, WindowContext *context);
static void set_window_keys(WindowContext *context, int wind_index);
static void assign_window_info(WindowContext *context);
static bool is_order_prefix_of(List *sca, List *scb);
static void lookup_window_function(RefInfo *rinfo);
static Plan *plan_trivial_window_query(PlannerInfo *root, WindowContext *context, List **pathkeys_ptr);
static Plan *plan_non_trivial_window_query(PlannerInfo *root, WindowContext *context, List **pathkeys_ptr);
static void plan_windowinfo_coplans(PlannerInfo *root, WindowContext *context, int window_index);
static List * plan_window_rtable(PlannerInfo* root, WindowContext *context);
static List *make_rowkey_targets(void);
static Coplan *makeCoplan(CoplanType type, WindowContext *context);
static AttrNumber addTargetToCoplan(Node *target, Coplan *coplan, WindowContext *context);
static Aggref* makeWindowAggref(WindowRef *winref);
static Aggref* makeAuxCountAggref(void);
static List *translate_upper_tlist(List *orig_tlist, WindowContext *context);
static Node * translate_upper_tlist_mutator(Node *node, WindowContext *context);
static FromExpr *plan_window_jointree(PlannerInfo *root, WindowContext *context);
static Expr *make_window_join_term(Oid type, Index lftvarno, AttrNumber lftattrno, Index rgtvarno, AttrNumber rgtattrno);
static Oid *get_window_attr_types(List *targetlist, int nattrs, AttrNumber *attrs);

/* Helper function to get nth windowinfo's partkey len */
static int get_windowctxt_partkey_len(void *ctxt, int nth)
{
	WindowContext *winctxt = (WindowContext *) ctxt;
	WindowInfo* winfo;
	
	Assert(nth <= winctxt->nwindowinfos);
	winfo = &(winctxt->windowinfos[nth]);

	return winfo->partkey_len;
}
/* Helper function to get nth windowinfo's sort clause */
static List *get_windowctxt_sortclause(void *ctxt, int nth)
{
	WindowContext *winctxt = (WindowContext *) ctxt;
	WindowInfo* winfo;
	
	Assert(nth <= winctxt->nwindowinfos);
	winfo = &(winctxt->windowinfos[nth]);

	return winfo->sortclause;
}


Plan *
window_planner(PlannerInfo *root, double tuple_fraction, List **pathkeys_ptr)
{
	Query		   *parse = root->parse;
	WindowContext  *context = newWindowContext();
	Plan		   *result_plan = NULL;

	/* Assert existence of windowing in query. */
	Assert(parse->targetList != NIL);
	Assert(parse->windowClause != NULL);
	Assert(parse->hasWindFuncs);
	
	/* Assert no unsupported stuff */
	Assert(parse->utilityStmt == NULL);
	Assert(parse->setOperations == NULL);
	Assert(!parse->hasAggs);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	
#ifdef CDB_WINDOW_DISPLAY	
	elog_node_display(NOTICE, "Window  query target list", parse->targetList, true);
	elog_node_display(NOTICE, "Window  query window  clause", parse->windowClause, true);
#endif
	
	/* Create a map: (varno,varattno)  <--> (integer) for use in recording
	 * Var references in Bitmapsets.
	 */
	build_var_index(parse, context);
	
	/* Create a copy of the input target list for our use.  Save pointers
	 * to the contained WindowRef nodes on the side in a array of RefInfo
	 * structures.
	 */
	preprocess_window_tlist(parse->targetList, context);

	/* Create an array of SpecInfo structures corresponding to the distinct
	 * referenced WindowSpec nodes in the input. Refer back to this array
	 * from the array of RefInfo structures.
	 */
	inventory_window_specs(parse->windowClause, context);

	/* Generate the (lower) target list for the common subquery.  
	 * Importantly, the ressortgroupref values agree with those in
	 * the input (upper) target list (and all of them appear).
	 */
	make_lower_targetlist(root->parse, context);
	
	/* Divide the SpecInfos into (contiguous) groups to be handled by a
	 * single Window node and associated auxiliary stuff (in other words,
	 * by a single sort of the input) and assign a WindowInfo to represent
	 * each group.
	 */
	assign_window_info(context);
	
	Assert( context->upper_tlist != NULL );
	Assert( list_length(context->refinfos) > 0 );
	Assert( context->nspecinfos > 0 );
	
	
	if ( context->nwindowinfos == 1 && 
		!(context->has_unordered_aggs || context->has_deferred_window_fns) )
	{
		return plan_trivial_window_query(root, context, pathkeys_ptr);
	}
	else
	{
		return plan_non_trivial_window_query(root, context, pathkeys_ptr);
	}

	/* TODO Check our API.
	 *
	 * Note: pathkeys may be an important implicit result. 
	 */
	
	deleteWindowContext(context);
	
	if ( result_plan == NULL )
	{
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("That windowing query is not supported yet.")));
	}
	
	return result_plan;
}

/* Create a palloc'd WindowContext structure for local use. */
static WindowContext *newWindowContext()
{
	WindowContext *context = palloc(sizeof(WindowContext));
	
	context->upper_tlist = NIL;
	context->upper_var_set = NULL;
	context->refinfos = NIL;
	context->nspecinfos = 0;
	context->specinfos = NULL;
	context->nwindowinfos = 0;
	context->windowinfos = NULL;
	context->rowkey_len = 0;
	context->rowkey_attrs = NULL;
	
	context->coplan_count = 0;
	context->has_unordered_aggs = false;
	context->has_deferred_window_fns = false;
	
	context->max_varno = 0;
	context->varattno_offsets = NULL;
	context->offset_varnos = NULL;
	context->offset_lim = 0;
	context->offset_upper_varattrnos = NULL;
	
	context->max_sortref = 0;
	context->sortref_resno = NULL;

	context->cur_refinfo = NULL;
	
	return context;
}

/* Delete palloc'd WindowContext and owned sub-structures. */
static void deleteWindowContext(WindowContext *context)
{
	ListCell *cell;
	
	if ( context == NULL )
		return;
	
	/* no need to free upper_tlist */
	
	bms_free(context->upper_var_set);
	
	foreach( cell, context->refinfos )
	{
		RefInfo *ref = (RefInfo *)lfirst(cell);
		/* no need to free actual ref */
		bms_free(ref->varset);
	}
	list_free_deep(context->refinfos);
	
	if ( context->specinfos != NULL )
		pfree(context->specinfos);
	
	if ( context->rowkey_attrs != NULL )
		pfree(context->rowkey_attrs);
	
	if ( context->windowinfos != NULL )
		pfree(context->windowinfos);
	
	if ( context->varattno_offsets != NULL )
		pfree(context->varattno_offsets);
	
	if ( context->offset_varnos != NULL )
		pfree(context->offset_varnos);
	
	if ( context->offset_upper_varattrnos != NULL )
		pfree(context->offset_upper_varattrnos);
	
	if ( context->sortref_resno != NULL )
		pfree(context->sortref_resno);
	
	/* TODO Clean up handling of 
	 *   lower_tlist, 
	 *   keyed_lower_tlist, 
	 *   subplan, 
	 *   subplan_pathkeys
	 * so we know whether to free them!
	 */
	
	/* don't free cur_refinfo */
	
	pfree(context);
}



/* Build an index structure to convert a (varno, varattno) to an integer
 * index and save it in the context.  The intended use of the index is
 * as follows:
 *
 *    varattno_offsets[varno] + varattno  -->  index
 *
 *    offset_varnos[index]  -->  varno
 *    index - varattno_offsets[varno]  --> varattno
 */
static void build_var_index(Query *parse, WindowContext *context)
{
	int i, j, n;
	int max_varno = list_length(parse->rtable);
	
	context->max_varno = max_varno;
	context->varattno_offsets = palloc0((max_varno+1)*sizeof(int));
	
	expression_tree_walker((Node*)parse->targetList,
							window_tlist_vars_walker, 
							(void*)context );
							
	for ( i = 0, n = 0; i <= max_varno; i++ )
	{
		int max_attno = context->varattno_offsets[i];
		context->varattno_offsets[i] = n;
		n += max_attno;
	}
	
	context->offset_lim = (n+1)*sizeof(int);
	context->offset_varnos = palloc0(context->offset_lim);
	
	for ( i = 1, j = 1; i <= max_varno; i++ )
	{
		for ( ; j <= context->varattno_offsets[i]; j++ )
			context->offset_varnos[j] = i;
	}
}

/* Subroutine for build_var_index() updates varattno_offsets vector to
 * record largest attribute number seen per varno (at the current level). 
 */
static bool window_tlist_vars_walker(Node *node, WindowContext *context)
{
	if ( node == NULL )
		return false;
	
	if ( IsA(node, Var) )
	{
		Var *var = (Var *) node;
		Assert( var->varno <= context->max_varno );
		if ( var->varlevelsup > 0 )
			return false;
		if ( var->varattno > context->varattno_offsets[var->varno] )
			context->varattno_offsets[var->varno] = var->varattno;
		return false;
	}
	return expression_tree_walker(node, window_tlist_vars_walker, (void *)context);
}

/* Standard use of variable-to-offset index -- convert a Var on the lower
 * range to an integer, e.g., for use in a bit map. 
 */
static int index_of_var(Var * var, WindowContext *context)
{
	Assert( var->varno <= context->max_varno );
	
	return context->varattno_offsets[var->varno] + var->varattno;
}


/* Build a map of sortgroupref to resno for the given target list.
 * Store its maximum sortref value and the index where specified.
 */
static void build_sortref_index(List *tlist, int *max_sortref, AttrNumber **sortref_resno)
{
	ListCell *lc;
	AttrNumber *index = NULL;
	int n = 0;
	
	foreach ( lc, tlist )
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		if ( tle->ressortgroupref > n )
			n = tle->ressortgroupref;
	}

	index = (AttrNumber *)palloc0((n+1) * sizeof(AttrNumber));
	
	foreach ( lc, tlist )
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);
		if ( tle->ressortgroupref != 0 )
			index[tle->ressortgroupref] = tle->resno;
	}
	
	*max_sortref = n;
	*sortref_resno = index;
}


/*  preprocess_window_tlist
 *
 * Stores a deep copy of the given target list in the context and takes
 * inventory of its content as follows:
 *
 *	upper_tlist		the deep copy
 *
 *	upper_var_set	Vars used outside of WindowRef nodes
 *
 *	ref_infos		per WindowRef list of RefInfo containing
 *		ref		the WindowRef in upper_tlist
 *		varset	Vars used in the WindowRefs explicit arguments
 */
static void preprocess_window_tlist(List *orig_tlist, WindowContext *context)
{
	context->cur_refinfo = NULL;
	context->upper_var_set = NULL;
	
	context->upper_tlist =  (List*)  
		expression_tree_mutator((Node*)orig_tlist,
								 window_tlist_mutator, 
								 (void*)context );
}

/* Subroutine for preprocess_window_tlist.
 *
 * Deep copy the given expression (presumed to be a part of the parse tree
 * target list for a window query) while checking validity of and recording
 * information about contained WindowRef nodes.
 */
static Node * window_tlist_mutator(Node *node, WindowContext *context)
{
	if ( node == NULL )
		return NULL;
		
	if ( IsA(node, Var) )
	{
		Var *var = (Var *)node;
		int idx = index_of_var(var, context);
		
		if ( context->cur_refinfo == NULL ) /* above all WindowRef */
		{
			if ( var->varlevelsup == 0 )
			{
				context->upper_var_set = 
					bms_add_member(context->upper_var_set, idx);
			}
		}
		else /* below a WindowRef */
		{
			if ( var->varlevelsup == 0 )
			{
				context->cur_refinfo->varset = 
					bms_add_member(context->cur_refinfo->varset, idx);
			}
			else
			{
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("Call to window function may not reference outer queries.")));									
			}
		}
		return (Node *) copyObject(node); /* Exact copy of the Var */
	}
	else if ( IsA(node, WindowRef) )
	{
		WindowRef *window_ref = (WindowRef *)node;
		WindowRef *new_ref = NULL;
		
		if ( context->cur_refinfo == NULL ) /* top-level WindowRef */
		{
			new_ref = copyObject(window_ref);

			context->cur_refinfo = palloc0(sizeof(RefInfo));			
			context->cur_refinfo->ref = new_ref;
			context->cur_refinfo->varset = NULL;
			lookup_window_function(context->cur_refinfo);
			
			new_ref->args = (List *)
				window_tlist_mutator((Node*)new_ref->args, context);
							
			/* Record the WindowRef and its Vars referenced set. */
			new_ref->winindex = list_length(context->refinfos);
			context->refinfos = lappend(context->refinfos, context->cur_refinfo);
			
			/* Clean up context. */
			context->cur_refinfo = NULL;
			
			return (Node*)new_ref;
		}
		else
		{
			ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Window function calls may not be nested.")));					
		}
	}
	
	return expression_tree_mutator(node, window_tlist_mutator, (void*) context);
}


/* inventory_window_specs
 * 
 * Take inventory of the supplied list of WindowSpecs in the given context. 
 * Add an array of distinct SpecInfo to the context.  Exclude SpecInfos that
 * are not referenced by any WindowRefs.  Afterward, the specindex field of
 * a RefInfo identifies the parent SpecInfo of its WindowRef.
 *
 * Note that preprocess_window_tlist() must already have run in order to
 * fill in the list refinfos list in context.
 */
static void inventory_window_specs(List *window_specs, WindowContext *context)
{
	ListCell *lcs, *lcp, *lcr;
	SpecInfo **index;
	int i, j, ndistinct;
	unsigned nspec;
	SpecInfo *specinfos;
	
	/* Construct preliminary array of SpecInfo structs, one per WindowSpec. 
	 * Note that, at this point, the order is the same as the Query's 
	 * windowClause list. 
	 */
	nspec = list_length(window_specs);
	specinfos = palloc0(nspec * sizeof(SpecInfo));
	i = 0;
	foreach ( lcs, window_specs )
	{
		WindowSpec *spec = (WindowSpec *) lfirst(lcs);
		Bitmapset *map = NULL;
		
		foreach ( lcp, spec->partition )
		{
			SortClause *sc = (SortClause *) lfirst(lcp);
			map = bms_add_member(map, sc->tleSortGroupRef);
		}
		specinfos[i].specindex = i;
		specinfos[i].partset = map;
		specinfos[i].order = spec->order;
		specinfos[i].frame = spec->frame;
		specinfos[i].refset = NULL;
		specinfos[i].windowindex = 0;
		specinfos[i].keylevel = 0;
		specinfos[i].partkey = spec->partition;
		i++;
	}
	
	/* Note which WindowRefs each WindowSpec covers by saving the indexes
	 * of their RefInfos.  In addition, note in the context any occurence
	 * of a window function that requires an auxiliary aggregate coplan. 
	 */
	i = 0;
	foreach ( lcr, context->refinfos )
	{
		RefInfo *rinfo = (RefInfo *)lfirst(lcr);
		WindowRef *ref = rinfo->ref;
		SpecInfo *sinfo = &specinfos[ref->winspec];
		sinfo->refset = bms_add_member(sinfo->refset, i++);
		
		if ( rinfo->isagg && sinfo->order == NIL )
			context->has_unordered_aggs = true;
		if ( rinfo->needcount && ! rinfo->isagg )
			context->has_deferred_window_fns = true;
	}	
	
	/* Make an index to group by partition, order, and frame. */
	index = (SpecInfo**)palloc(nspec * sizeof(SpecInfo**));
	for ( i = 0; i < nspec; i++ )
		index[i] = &specinfos[i];
	qsort(index, nspec, sizeof(SpecInfo*), compare_spec_info_ptr);
	
	
	
	/* Identify distinct, referenced window specs and bubble up their
	 * refsets. Begin by finding the first referenced window spec and
	 * mark it to be the first  distinct representative window spec.  
	 * 
	 * Though not strictly necessary, we mark unreferenced window specs 
	 * with an index of nspec.
	 *
	 * In the second loop, j (via the index) indexes the current distinct 
	 * representative SpecInfo while i indexes the next candidate.
	 *
	 * Since processing moves forward through the index, we write the
	 * answer in its leading elements.
	 */
	for ( j = 0; j < nspec; j++ )
	{
		if ( ! bms_is_empty(index[j]->refset) )
			break;
		else
			index[j]->specindex = nspec; /* don't use */
	}
	Assert ( j < nspec ); 
	
	ndistinct = 0;
	index[j]->specindex = ndistinct;
	index[ndistinct++] = index[j];
	for ( i = j+1; i < nspec; i++ )
	{
		if ( compare_spec_info_ptr(&index[j],&index[i]) != 0 )
		{
			if ( bms_is_empty(index[i]->refset) )
			{
				index[i]->specindex = nspec; /* don't use */
				continue;
			}
			/* SpecInfo at index[i] is start of next run. */
			j = i;
			index[j]->specindex = ndistinct;
			index[ndistinct++] = index[j];
		}
		else
		{
			/* SpecInfo at index[i] is duplicate of last. */
			index[j]->refset = bms_union(index[j]->refset, index[i]->refset);
			index[i]->specindex = index[j]->specindex;
		}
	}
	
	/* Construct array of distinct windowspecs in the context. */
	context->nspecinfos = ndistinct;
	context->specinfos = palloc0(ndistinct * sizeof(SpecInfo));
	for ( j = 0; j < ndistinct; j++ )
	{
		Assert( index[j]->specindex == j );
		memcpy(&context->specinfos[j], index[j], sizeof(SpecInfo));
	}
	
	/* Adjust RefInfo specindex to index the parent SpecInfo in context. 
	 * Update summary info in the parent.
	 */
	foreach ( lcr, context->refinfos )
	{
		RefInfo *rinfo = (RefInfo *)lfirst(lcr);
		WindowRef *ref = rinfo->ref;
		SpecInfo *sinfo = & specinfos[ref->winspec];
		
		Assert( sinfo->specindex < ndistinct );
		rinfo->specindex = sinfo->specindex;
	}		
	pfree(specinfos);
}


static void assign_window_info(WindowContext *context)
{
	int i, j, k, n;
	Index current;
	ListCell *lc;
	
	Assert( context->nspecinfos > 0 );
	
	/* First divide the previously produced, sorted list of distinct
	 * SpecInfo into groups with matching partitioning requirements 
	 * and so that each group member's ordering key is a prefix of 
	 * each following member's keys.
	 */
	context->specinfos[0].windowindex = current = 0;	
	for ( i = 1, j = 0; i < context->nspecinfos; j = i, i++ )
	{
		if ( ! bms_equal(context->specinfos[j].partset, context->specinfos[i].partset) 
			 || ! is_order_prefix_of(context->specinfos[j].order, context->specinfos[i].order) )
		{
			current++;
		}
		context->specinfos[i].windowindex = current;
	}
	n = current + 1;
	
	/* Next allocate a WindowInfo for each group and setup its requirements.
	 */
	context->nwindowinfos = n;
	context->windowinfos = (WindowInfo *)palloc0(n * sizeof(WindowInfo));
	for ( i = k = 0; i < n; i++ )
	{
		SpecInfo *final_sinfo = NULL;
		List *sortkey = NIL;
		Bitmapset *partset = NULL;
		WindowInfo *winfo = &context->windowinfos[i];
		
		j = k; /* j indexes first SpecInfo in this WindowInfo */
		
		for ( ; k < context->nspecinfos; k++ )
		{
			SpecInfo *sinfo = &context->specinfos[k];
			
			if ( sinfo->windowindex != i )
				break; /* k indexes first SpecInfo in next WindowInfo */
			final_sinfo = sinfo; /* has longest sort key so far */
		}
		
		Assert( final_sinfo != NULL );

		winfo->firstspecindex = j;
		winfo->numspecindex = k-j;
		
		/* Remove order key prefix from temp part set to discover how much
		 * sort key is just for partitioning, i.e., not overlapping an ORDER
		 * BY specification.
		 */
		partset = bms_copy(final_sinfo->partset);
		foreach ( lc, final_sinfo->order )
		{
			SortClause *sc = (SortClause *)lfirst(lc);
			if ( ! bms_is_member(sc->tleSortGroupRef, partset) )
				break;				
			partset = bms_del_member(partset, sc->tleSortGroupRef);
		}

		/* Sort key begins with remaining part keys. Stictly speaking, key
		 * order and sort direction is arbitrary in the partitioning key, 
		 * however, mimic the final SpecInfo for this WindowInfo to minimize
		 * user astonishment. 
		 */
		sortkey = NIL;
		foreach ( lc, final_sinfo->partkey )
		{
			SortClause *sc = (SortClause *)lfirst(lc);
			if ( bms_is_member(sc->tleSortGroupRef, partset) )
				sortkey = lappend(sortkey, copyObject(sc));
		}
		winfo->partkey_len = list_length(final_sinfo->partkey);
		winfo->orderkeys_offset = list_length(sortkey);
		
		/* Remainder of sort key is the final ORDER BY key. */
		winfo->sortclause = list_concat(sortkey, copyObject(final_sinfo->order));
	}
	
	/* Set up the partitioning and ordering key levels (WindowKeys)  */
	for ( i = 0; i < context->nwindowinfos; i++ )
	{	
		set_window_keys(context, i);
	}
	
	foreach ( lc, context->refinfos )
	{
		RefInfo *rinfo = (RefInfo*)lfirst(lc);
		SpecInfo *sinfo = &context->specinfos[rinfo->specindex];
		WindowInfo *winfo = &context->windowinfos[sinfo->windowindex];
		
		if ( sinfo->keylevel < 0 )
		{
			/* Unordered */
			Assert( sinfo->order == NIL );
			
			rinfo->ref->winlevel = list_length (winfo->key_list);
			if ( rinfo->isagg )
				winfo->needpartkey = true; /* unordered aggregation */
		}
		else
		{
			/* Ordered */
			Assert( sinfo->order != NIL );
			
			rinfo->ref->winlevel = sinfo->keylevel;
		}
		
		/* Check for window function in need of auxiliary count. */
		if ( !rinfo->isagg && rinfo->needcount )
		{
			winfo->needpartkey = true;
			winfo->needauxcount = true;
		}
	}
}


static bool is_order_prefix_of(List *sca, List *scb)
{
	ListCell *lca, *lcb;
	
	if ( list_length(sca) > list_length(scb) )
		return false;
	
	forboth(lca, sca, lcb, scb)
	{
		if ( !equal(lfirst(lca),lfirst(lcb)) )
			return false;
	}
	return true;
}



/* Comparision functions for local use */

static int compare_spec_info_ptr(const void *arg1, const void *arg2)
{
	int n;
	SpecInfo *a = *(SpecInfo **)arg1;
	SpecInfo *b = *(SpecInfo **)arg2;
	
	/* partition */
	n = bms_compare(a->partset, b->partset);
	if ( n != 0 ) 
		return n;
	
	/* order */
	n = compare_order(a->order, b->order);
	if ( n != 0 ) 
		return n;
	
	/* frame */
	return compare_frame(a->frame, b->frame);
}

static int compare_order(List *a, List* b)
{
	ListCell *lca, *lcb;
	int na, nb;
	
	forboth ( lca, a, lcb, b )
	{
		SortClause *sca = (SortClause *)lfirst(lca);
		SortClause *scb = (SortClause *)lfirst(lcb);
		
		if ( sca->tleSortGroupRef < scb->tleSortGroupRef )
			return -1;
		else if ( sca->tleSortGroupRef > scb->tleSortGroupRef )
			return 1;
		else if ( sca->sortop < scb->sortop )
			return -1;
		else if ( sca->sortop > scb->sortop )
			return 1;
	}
	na = list_length(a);
	nb = list_length(b);
	if ( na < nb )
		return -1;
	else if ( na > nb )
		return 1;
	return 0;
}

static int compare_frame(WindowFrame *a, WindowFrame *b)
{
	int n;
	
	if ( a == b )
		return 0;
	else if ( a == NULL )
		return -1;
	else if ( b == NULL )
		return 1;
	
	if ( a->is_rows && ! b->is_rows )
		return -1;
	else if ( b->is_rows && ! a->is_rows )
		return 1;
	
	n = compare_edge(a->trail, b->trail);
	if ( n != 0 )
		return n;
		
	n = compare_edge(a->lead, b->lead);
	if ( n != 0 )
		return n;
	
	if ( a->exclude < b->exclude )
		return -1;
	else if ( a->exclude > b->exclude )
		return 1;
	
	return 0;
}

static int compare_edge(WindowFrameEdge *a, WindowFrameEdge *b)
{
	if ( a->kind < b->kind )
		return -1;
	else if ( a->kind > b->kind )
		return 1;
	else if ( a->kind == WINDOW_BOUND_PRECEDING || a->kind == WINDOW_BOUND_FOLLOWING )
	{
		/* TODO: compare a->val to b->val */
		elog(ERROR, "cannot compare framing bounds yet!!!");
	}
	return 0;
}


/*---------------
 * make_lower_targetlist
 *
 * When window_planner inserts Window, Aggregate, or Result plan nodes
 * above the result of query_planner, we may want to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan.
 *
 * The initial target list passed from the parser already contains entries
 * for all PARTITION BY and ORDER BY expressions. We flatten all expressions
 * except these into their component variables; the other expressions
 * will be computed by the inserted nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) OVER (ORDER BY a+b) FROM table;
 * we want to pass this targetlist to the subplan:
 *		a,b,c,d,a+b
 * where the a+b target will be used by the Sort/Window step, and the
 * other targets will be used for computing the final results.	(In the
 * above example we could theoretically suppress the a and b targets and
 * pass down only c,d,a+b, but it's not really worth the trouble to
 * eliminate simple var references from the subplan.  We will avoid doing
 * the extra computation to recompute a+b at the outer level; see
 * replace_vars_with_subplan_refs() in setrefs.c.)
 *
 * If we are grouping or aggregating, *and* there are no non-Var grouping
 * expressions, then the returned tlist is effectively dummy; we do not
 * need to force it to be evaluated, because all the Vars it contains
 * should be present in the output of query_planner anyway.
 *
 * The targetlist to be passed to the subplan goes in context->lower_tlist.
 *---------------
 */
static void
make_lower_targetlist(Query *parse,
					  WindowContext *context)
{
	List	   *tlist = parse->targetList;
	List	   *lower_tlist;
	bool		need_tlist_eval;
	int			i;
	SortClause *dummy;
	ListCell   *lc;

	Assert ( parse->hasWindFuncs );
	
	/* Start with a "flattened" tlist (having just the vars mentioned in 
	 * the targetlist --- but no upper-level Vars; they will be replaced 
	 * by Params later on).
	 */
	lower_tlist = flatten_tlist(tlist);
	need_tlist_eval = false;
	
	/* Find or add target list entries for partitioning or ordering exprs
	 * mentioned in window specs to be fed by this targetlist. 
	 * 
	 * TODO Revisit this when we're ready to handle complicated cases.
	 *      Probably need to track/reassign sortgroupref values. 
	 */
	dummy = makeNode(SortClause);
	for ( i = 0; i < context->nspecinfos; i++ )
	{
		TargetEntry *tle;
		int sortgroupref;
		List *extra_tles = NIL;
		
		bms_foreach ( sortgroupref, context->specinfos[i].partset )
		{
			dummy->tleSortGroupRef = sortgroupref;
			tle = get_sortgroupclause_tle(dummy, tlist);
			extra_tles = lappend(extra_tles, tle);
		}
		foreach ( lc, context->specinfos[i].order )
		{
			SortClause *sc = (SortClause *) lfirst(lc);
			tle = get_sortgroupclause_tle(sc, tlist);
			extra_tles = lappend(extra_tles, tle);
		}
			
		foreach ( lc, extra_tles )
		{
			ListCell *lct;
			TargetEntry *te;
			bool found = false;
			
			tle = (TargetEntry *)lfirst(lc);
			
			foreach (lct, lower_tlist)
			{
				te = (TargetEntry *)lfirst(lct);
				if (equal(tle->expr, te->expr) && 
					(te->ressortgroupref == tle->ressortgroupref ||
					 te->ressortgroupref == 0))
				{
					break;
				}
			}
			
			if ( lct != NULL )
			{
				/* Found one. */
				if ( te->ressortgroupref != tle->ressortgroupref )
				{
					Assert( te->ressortgroupref == 0 );
					te->ressortgroupref = tle->ressortgroupref;
				}
			}
			else
			{
				/* Need to add one. */
				te = copyObject(tle);
				te->resno = list_length(lower_tlist) + 1;
				lower_tlist = lappend(lower_tlist, te);
				/* NOTE that the addition must be an expr other than 
				 *  a simple Var.  Do we care? */
				need_tlist_eval = true;
			}
		}
		list_free(extra_tles);
	}
	pfree(dummy);
	dummy = NULL;
	
	build_sortref_index(lower_tlist, &context->max_sortref, &context->sortref_resno);

	context->lower_tlist = lower_tlist;
}


static void set_window_keys(WindowContext *context, int wind_index)
{
	WindowInfo *winfo;
	int i, j;
	int skoffset, nextsk;
	ListCell *lc;
	int nattrs;
	AttrNumber* sortattrs = NULL;
	
	/* results  */
	int partkey_len = 0;
	AttrNumber *partkey_attrs = NULL;
	List *window_keys = NIL;
	
	Assert( 0 <= wind_index && wind_index < context->nwindowinfos );
	
	winfo = &context->windowinfos[wind_index];

	/* Translate the sort clause to attribute numbers. */
	nattrs = list_length(winfo->sortclause);
	if ( nattrs > 0 )
	{
		sortattrs = (AttrNumber *)palloc(nattrs*sizeof(AttrNumber));
		i = 0;
		foreach ( lc, winfo->sortclause )
		{
			SortClause *sc = (SortClause *)lfirst(lc);
			sortattrs[i++] = context->sortref_resno[sc->tleSortGroupRef];
		}
	}
	
	/* Make a separate copy of just the partition key. */
	if ( winfo->partkey_len > 0 )
	{
		partkey_len = winfo->partkey_len;
		partkey_attrs = (AttrNumber *)palloc(partkey_len*sizeof(AttrNumber));
		for ( i = 0; i < partkey_len; i++ )
			partkey_attrs[i] = sortattrs[i];
	}
	
	/* Careful.  Within sort key, parition key may overlap order keys. */
	nextsk = skoffset = winfo->orderkeys_offset;
	
	/* Make a WindowKey per SpecInfo. */
	for ( i = 0; i < winfo->numspecindex; i++ )
	{
		WindowKey *wkey;
		SpecInfo *sinfo = &context->specinfos[winfo->firstspecindex + i];
		int keylen = list_length(sinfo->order);
		
		if ( keylen == 0 )
		{
			Assert( sinfo->frame == NULL );
			sinfo->keylevel = -1;  /* No ordering key, just partition key. */
			continue;
		}
		wkey = makeNode(WindowKey);
		wkey->numSortCols = (keylen + skoffset) - nextsk;
		
		Assert( wkey->numSortCols >= 0 );
		
		if (  wkey->numSortCols > 0 )
		{
			wkey->sortColIdx = (AttrNumber*)palloc(wkey->numSortCols * sizeof(AttrNumber));
			
			for ( j = 0; j < wkey->numSortCols; j++ )
			{
				wkey->sortColIdx[j] = sortattrs[nextsk++];
			}
		}
		wkey->frame = copyObject(sinfo->frame);
		sinfo->keylevel = list_length(window_keys); /* WindowKey position. */
		window_keys = lappend(window_keys,  wkey);
	}
	
	winfo->partkey_attrs = partkey_attrs;
	winfo->key_list = window_keys;
}

/* Fill in the fields of the given RefInfo that characterize the window 
 * function represented by its WindowRef. 
 */
static void
lookup_window_function(RefInfo *rinfo)
{
	HeapTuple	tuple;
	Form_pg_proc proform;
	bool isagg, iswin;
	Oid transtype = InvalidOid;
	
	Oid fnoid = rinfo->ref->winfnoid;
	
	/* pg_proc */
	tuple = SearchSysCache(PROCOID,
							  ObjectIdGetDatum(fnoid),
							  0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fnoid);
	proform = (Form_pg_proc) GETSTRUCT(tuple);
	isagg = proform->proisagg;
	iswin = proform->proiswin;
	ReleaseSysCache(tuple);
	
	Assert( isagg != iswin );
	
	if ( isagg )
	{
		Form_pg_aggregate aggform;
	
		tuple = SearchSysCache(AGGFNOID,
								  ObjectIdGetDatum(fnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for aggregate function %u", fnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(tuple);
		
		rinfo->isagg = true;
		rinfo->hasinvtrans = (aggform->agginvtransfn != InvalidOid);
		rinfo->hasprelim = (aggform->aggprelimfn != InvalidOid);
		rinfo->hasinvprelim = (aggform->agginvprelimfn != InvalidOid);
		transtype = aggform->aggtranstype;

		ReleaseSysCache(tuple);
		
		/*
		 * If the transition type is pass-by-refernce, note the estimated 
		 * size of the value itself, plus palloc overhead.
		 */
		if (!get_typbyval(transtype))
		{
			int32		avgwidth;

			avgwidth = get_typavgwidth(transtype, -1);
			avgwidth = MAXALIGN(avgwidth);

			rinfo->trans_space += avgwidth + 2 * sizeof(void *);
		}
		
	}
	else /* iswin */
	{
		Form_pg_window winform;
	
		tuple = SearchSysCache(WINFNOID,
								  ObjectIdGetDatum(fnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for window function %u", fnoid);
		winform = (Form_pg_window) GETSTRUCT(tuple);

		rinfo->needcount = winform->wincount;
		rinfo->winpretype = winform->winpretype;
		rinfo->winfinfunc = winform->winfinfunc;		

		ReleaseSysCache(tuple);
	}
}


static Plan *plan_common_subquery(PlannerInfo *root, List *lower_tlist,
								  List *order_hint,
								  CdbPathLocus *result_locus,
								  List **result_pathkeys)
{
	Plan *result_plan = NULL;
	Path *cheapest_path = NULL;
	Path *sorted_path = NULL;
	Path *best_path = NULL;
	double num_groups = 0.0;
	QualCost tlist_cost;
	
	root->query_pathkeys = NIL;
	root->group_pathkeys = NIL;
	root->sort_pathkeys = NIL;
	
	/* If an order hint is specified, try for it.   */
	if ( order_hint != NIL )
		root->sort_pathkeys = make_pathkeys_for_sortclauses(order_hint, lower_tlist);


	/* Generate the best unsorted and presorted paths for this Query. */
	query_planner(root, lower_tlist, 0.0, 
				  &cheapest_path, &sorted_path, &num_groups);
	if ( order_hint != NIL && sorted_path != NULL )
		best_path = sorted_path;
	else
		best_path = cheapest_path;
	
	/* Make the plan. */
	result_plan = create_plan(root, best_path);
	result_plan = plan_pushdown_tlist(result_plan, lower_tlist);
	
	Assert(result_plan->flow);

	/* Account for the cost of evaluation of the lower tlist. */
	cost_qual_eval(&tlist_cost, lower_tlist);
	result_plan->startup_cost += tlist_cost.startup;
	result_plan->total_cost +=
		tlist_cost.startup +
		tlist_cost.per_tuple * result_plan->plan_rows;

	/* Set up results. */
	*result_locus = best_path->locus;
	*result_pathkeys = root->query_pathkeys;	
	return result_plan;
}


/* Plan a window query that can be implemented without any joins or 
 * input sharing, i.e., it involves one scan of the input plan result
 * and a single Window node.
 *
 * Returns plan and, implicitly, pathkeys.
 */
static Plan *plan_trivial_window_query(PlannerInfo *root, WindowContext *context, List **pathkeys_ptr)
{
	Plan *result_plan;
	WindowInfo *winfo;
	List *lower_tlist = context->lower_tlist;
	CdbPathLocus input_locus, output_locus;
	List *pathkeys = NIL;
	List *window_pathkeys = NIL;	
	
	Assert ( pathkeys_ptr );
	Assert (context->nwindowinfos == 1);
	winfo = context->windowinfos;


	result_plan = plan_common_subquery(root, lower_tlist,
								  winfo->sortclause, /* order hint */
								  &input_locus,
								  &pathkeys);
								  
	
	window_pathkeys = make_pathkeys_for_sortclauses(winfo->sortclause, lower_tlist);
	window_pathkeys = canonicalize_pathkeys(root, window_pathkeys);
	
	/* Assure needed colocation and order. */
	result_plan = condition_input(&shareplan_simple,
								  root,
								  result_plan,
								  winfo->partkey_len,
								  winfo->partkey_attrs,
								  winfo->sortclause,
								  input_locus,
								  &pathkeys);
	
	/* Add the single Window node. */
	result_plan = (Plan*) make_window(root, context->upper_tlist, 
									  winfo->partkey_len, winfo->partkey_attrs,
									  winfo->key_list, /* XXX copy? */
									  result_plan);
	
	/* 
	 * Add a Flow node to the top node, if it doesn't have one yet.
	 * Retrieve distribution info from the next-to-top node which
	 * must have a Flow node.
	 */
	if (!result_plan->flow)
		result_plan->flow = pull_up_Flow(result_plan, 
										 result_plan->lefttree, 
										 (pathkeys != NIL));

	/* TODO Check our API.
	 *
	 * Note: locus and pathkeys may be an important implicit result. 
	 */
	CdbPathLocus_MakeSingleQE(&output_locus); /* We don't partition yet. */
	
	*pathkeys_ptr = pathkeys;
	return result_plan;
}



/* Plan a window query that requires a join and/or input sharing. 
 *
 * Returns plan and, implicitly, pathkeys.
 */
static Plan *plan_non_trivial_window_query(PlannerInfo *root, WindowContext *context, List **pathkeys_ptr)
{
	Plan *subplan;
	Plan *result_plan;
	List *result_pathkeys;
	CdbPathLocus input_locus;
	List *pathkeys = NIL;
	List *targetlist;
	List *rtable;
	FromExpr *jointree;
	
	int i;
	
	Assert ( pathkeys_ptr != NULL );

	subplan = plan_common_subquery(root, context->lower_tlist,
								  NULL, /* order hint */
								  &input_locus,
								  &pathkeys);
	
	/* If necessary, add row key to the lower plan. */
	if ( context->nwindowinfos > 1 )
	{
		AttrNumber resno;
		ListCell *lc;
		List *lower_tlist = copyObject(context->lower_tlist);
		List *rowkey_tlist = make_rowkey_targets();
		
		context->rowkey_len = list_length(rowkey_tlist);
		context->rowkey_attrs = palloc(context->rowkey_len * sizeof(AttrNumber));
			
		resno = list_length(lower_tlist); /* XXX safe? */
		i = 0;
		foreach ( lc, rowkey_tlist )
		{
			TargetEntry *tle = (TargetEntry *)lfirst(lc);
			tle->resno = ++resno;
			context->rowkey_attrs[i++] = resno;
		}
		lower_tlist = list_concat(lower_tlist, rowkey_tlist);
		context->keyed_lower_tlist = lower_tlist;
		
		subplan = (Plan*) make_window(root, lower_tlist, 
										  0, NULL, /* No paritioning */
										  NIL, /* No ordering */
										  subplan);
		if (!subplan->flow)
			subplan->flow = pull_up_Flow(subplan, 
											 subplan->lefttree, 
											 (pathkeys != NIL));
	}
	else
	{
		context->rowkey_len = 0;
		context->rowkey_attrs = NULL;	
		context->keyed_lower_tlist = context->lower_tlist;	
	}
	
	/* Record common subquery information in context. */
	context->subplan = subplan;
	context->subplan_locus = input_locus;
	context->subplan_pathkeys = pathkeys;
	
	/* Generate coplans for each WindowInfo */								  								  
	context->coplan_count = 0;
	
	for ( i = 0; i < context->nwindowinfos; i++ )
	{
		plan_windowinfo_coplans(root, context, i);
	}
	
	/* Construct the range table for the join query to be constructed. */
	rtable = plan_window_rtable(root, context);
	
	/* Mutate the upper target list to refer to the new range table. */
	targetlist = translate_upper_tlist(context->upper_tlist, context);
	
	/* Arrange to join the coplans back together. */
	jointree = plan_window_jointree(root,  context);
	
	/* XXX This is the evil part!
	 *
	 * Modify the input query to look like a new upper level join
	 * query.  Retain what's above  the jointree (ordering, etc)
	 * from the original.  This works because the targetlist is
	 * "conformable" with the original w.r.t. sort/group refs,
	 * resnos, types, etc.
	 *
	 * We don't need to descend into the range table to fix varlevelsup
	 * due to the new query level we're adding, because the range table
	 * has already been planned. (Right?)
	 */
	root->parse->targetList = targetlist;
	root->parse->rtable = rtable;
	root->parse->jointree = jointree;
	root->parse->windowClause = NIL;
	
	/* Plan the join.
	 */
	{
		Path *cheapest_path;
		Path *best_path;
		Path *sorted_path;
		double ngroups;
		QualCost tlist_cost;
						
		root->group_pathkeys = NIL;
		root->sort_pathkeys = 
			make_pathkeys_for_sortclauses(root->parse->sortClause, targetlist);
		root->query_pathkeys = root->sort_pathkeys;
		
		query_planner(root, targetlist, 0.0, &cheapest_path, &sorted_path, &ngroups);
		
		if ( sorted_path != NULL )
			best_path = sorted_path;
		else
			best_path = cheapest_path;
		
		result_plan = create_plan(root, best_path);
		result_pathkeys = best_path->pathkeys;
		
		/* Adjust target list (create_plan returns a flat one) and costs
		 * since our target list may have expressions.  (Could track this
		 * and avoid, but not yet.)
		 */
		result_plan = plan_pushdown_tlist(result_plan, targetlist);
		cost_qual_eval(&tlist_cost, targetlist);
		result_plan->startup_cost += tlist_cost.startup;
		result_plan->total_cost += tlist_cost.startup + tlist_cost.per_tuple * result_plan->plan_rows;

		Assert( result_plan->flow != NULL );
	}

	*pathkeys_ptr = result_pathkeys;
	return result_plan;
}


static void plan_windowinfo_coplans(PlannerInfo *root, WindowContext *context, int window_index)
{
	Coplan *window_coplan = NULL;
	Coplan *agg_coplan = NULL;
	size_t sz;
	int i;
	ListCell *lc;
	WindowInfo *winfo = context->windowinfos + window_index;
	
	/* There's always a window coplan. If there is more than one WindowInfo, 
	 * we must make sure they all contain the row key.
	 *
	 * XXX Except for the first (required) window coplan, we don't really
	 *     need a window coplan when there's only a widow aggregate over
	 *     an unpartitioned, unordered window.  Currently, we ignore
	 *     this optimization and always include one.
	 */
	window_coplan = makeCoplan(COPLAN_WINDOW, context);
	
	/* There's an agg coplan only if there are unordered aggregates or
	 * deferred strategy window functions.  The agg coplan will join to
	 * its window coplan on the partition key, if one exists, or by 
	 * cartesian product.
	 */
	if ( winfo->needpartkey || winfo->needauxcount )
		agg_coplan = makeCoplan(COPLAN_AGG, context);
	
	if ( window_index == 0 )
	{
		/* First window coplan is special.  It contains the entire lower
		 * target list along with the row key, if any.  Subsequent window 
		 * coplans are sparse.  They contain only those targets needed 
		 * locally by their WindowInfo.
		 *
		 * Note that, at this point, the upper and lower target lists refer 
		 * to the same range -- that of the common subquery.  But soon the 
		 * upper list will refer to the middle range we are about to build.  
		 *
		 * Here, we initialize the Var translation map we'll used to adjust
		 * upper Vars to reference the new middle range. During processing 
		 * we add an entry to this map for the entries corresponding to plain 
		 * Vars.  By construction, this will include an entry for each 
		 * distinct Var in the range.
		 */		
		context->offset_upper_varattrnos = palloc0(context->offset_lim * sizeof(Var*));
		
		foreach ( lc, context->keyed_lower_tlist )
		{
			TargetEntry *tle = (TargetEntry *)lfirst(lc);
			AttrNumber resno = addTargetToCoplan((Node*)tle, window_coplan, context);
			
			/* This assumption used below to copy key attributes. */
			Assert( resno == tle->resno ); 
			
			if ( IsA(tle->expr, Var) )
			{
				Var *var = (Var*)tle->expr;
				context->offset_upper_varattrnos[index_of_var(var,context)] = resno;
			}
		}

		/* The attribute numbers of keys are the same as in the lower target 
		 * list, so we can just copy them. 
		 */
		if ( context->rowkey_len > 0 )
		{
			window_coplan->rowkey_len = context->rowkey_len;
			sz = context->rowkey_len * sizeof(AttrNumber);
			window_coplan->rowkey_attrs = palloc(sz);
			memcpy(window_coplan->rowkey_attrs, context->rowkey_attrs, sz);
		}

		if ( winfo->partkey_len > 0 )
		{
			sz = winfo->partkey_len * sizeof(AttrNumber);
			window_coplan->partkey_attrs = palloc(sz);
			memcpy(window_coplan->partkey_attrs, winfo->partkey_attrs, sz);
			window_coplan->partkey_len = winfo->partkey_len;
		}
			
		if ( (agg_coplan != NULL) && winfo->partkey_len > 0 )
		{
			agg_coplan->partkey_len = winfo->partkey_len;
			agg_coplan->partkey_attrs = palloc(sz);
			for ( i = 0; i < winfo->partkey_len; i ++ )
			{
				TargetEntry *tle; 
				tle = get_tle_by_resno(context->lower_tlist, winfo->partkey_attrs[i]);
				agg_coplan->partkey_attrs[i] = 
					addTargetToCoplan((Node*)tle, agg_coplan, context);
			}
		}
	}
	else
	{
		/* Later window coplans are all similar and need contain only 
		 * targets specifically used by this WindowInfo's WindowRefs. */			
		
		/* Include input key, if any. */
		if ( context->rowkey_len > 0 )
		{
			window_coplan->rowkey_len = context->rowkey_len;
			sz = winfo->rowkey_len * sizeof(AttrNumber);
			window_coplan->rowkey_attrs = palloc(sz);
			for ( i = 0; i < context->rowkey_len; i ++ )
			{
				TargetEntry *tle;
				
				
				tle = get_tle_by_resno(context->keyed_lower_tlist, 
									   context->rowkey_attrs[i]);
									 
				window_coplan->rowkey_attrs[i] = 
					addTargetToCoplan((Node*)tle, window_coplan, context);
			}
		}
			
		/* Include partition key, if needed. */
		if ( (agg_coplan != NULL) && winfo->partkey_len > 0 )
		{
			sz = winfo->partkey_len * sizeof(AttrNumber);
			window_coplan->partkey_len = winfo->partkey_len;
			window_coplan->partkey_attrs = (AttrNumber*)palloc(sz);
			agg_coplan->partkey_len = winfo->partkey_len;
			agg_coplan->partkey_attrs = (AttrNumber*)palloc(sz);
			for ( i = 0; i < winfo->partkey_len; i++ )
			{
				TargetEntry *tle; 
			
				tle = get_tle_by_resno(context->keyed_lower_tlist, 
									   winfo->partkey_attrs[i]);

				window_coplan->partkey_attrs[i] = 
					addTargetToCoplan((Node*)tle, window_coplan, context);
				agg_coplan->partkey_attrs[i] = 
					addTargetToCoplan((Node*)tle, agg_coplan, context);
			}
		}
	}
	
	
	/* Now add targets for each RefInfo covered by this WindowInfo.
	 *
	 * TODO Reorganize context->refinfos into an array.  Use varsets
	 *      to access the refinfos. 
	 */
	foreach ( lc, context->refinfos )
	{
		WindowRef *ref;
		AttrNumber resno;
		RefInfo *rinfo = (RefInfo *)lfirst(lc);
		SpecInfo *sinfo = &context->specinfos[rinfo->specindex];
		
		if ( sinfo->windowindex != window_index )
			continue;
		
		ref = rinfo->ref;
		
		if ( rinfo->isagg && sinfo->order == NIL )
		{
			/* Plan unordered aggregate */
			Aggref *aggref;
			Var *var;
			
			aggref = makeWindowAggref(ref);
			resno = addTargetToCoplan((Node*)aggref, agg_coplan, context);
			
			/* Add result expr to ref */
			var = makeVar(agg_coplan->varno, resno, ref->restype, -1, false);
			rinfo->resultexpr = (Expr*)var;
		}
		else if ( !rinfo->isagg && rinfo->needcount )
		{
			/* Plan deferred window function */
			FuncExpr *func;
			Var *arg1, *arg2;
			Oid counttype = 20; /* TODO count(*) type in pg_proc */
			Oid restype = ref->restype;
			
			ref->winstage = WINSTAGE_PRELIMINARY;
			ref->restype = rinfo->winpretype;

			resno = addTargetToCoplan((Node*)ref, window_coplan, context);			
			arg1 = makeVar(window_coplan->varno, resno, ref->restype, -1, false);
			
			resno = addTargetToCoplan((Node*)makeAuxCountAggref(), agg_coplan, context);
			arg2 = makeVar(agg_coplan->varno, resno, counttype , -1, false);
			
			/* Add result expr to ref */
			func = makeFuncExpr(rinfo->winfinfunc, restype, list_make2(arg1, arg2), COERCE_DONTCARE);
			rinfo->resultexpr = (Expr*)func;
		}
		else if ( rinfo->isagg || !rinfo->needcount )
		{
			/* Plan immediate window function or ordered window agg) */
			ref->winstage = WINSTAGE_IMMEDIATE;
			resno = addTargetToCoplan((Node*)ref, window_coplan, context);
			
			/* Add result expr to ref */
			rinfo->resultexpr = (Expr*)makeVar(window_coplan->varno, resno, ref->restype, -1, false);
		}
		else
		{
			elog(ERROR,"internal error planning window function call");
		}
		
		if ( agg_coplan != NULL )
			agg_coplan->trans_space += rinfo->trans_space; /* may over count, just an estimate */
	}
	
	/* Store results. */
	winfo->window_coplan = window_coplan;
	winfo->agg_coplan = agg_coplan;
}


/* Return a target list to use as the join key for the separate window
 * coplans generated by a non-trivial window query.  The key consists
 * of a segment number and row number and must be evaluated within a
 * Window node.  
 *
 * The caller is responsible for assigning appropriate resno values 
 * in the targets.  (They are initialized, here, to zero!).
 */
static List *make_rowkey_targets()
{
	FuncExpr *seg;
	WindowRef *row;
	
	seg = makeFuncExpr(MPP_EXECUTION_SEGMENT_OID, 
					   MPP_EXECUTION_SEGMENT_TYPE, 
					   NIL, 
					   COERCE_DONTCARE);
								  
	row = makeNode(WindowRef);
	row->winfnoid = ROW_NUMBER_OID;
	row->restype = ROW_NUMBER_TYPE;
	row->args = NIL;
	row->winlevelsup = row->winspec = row->winindex = 0;
	row->winstage = WINSTAGE_ROWKEY; /* so setrefs doesn't get confused  */
	row->winlevel = 0;
	
	return list_make2(
		makeTargetEntry((Expr*)seg, 1, pstrdup("segment_join_key"), false),
		makeTargetEntry((Expr*)row, 1, pstrdup("row_join_key"), false) );
}


static Coplan *makeCoplan(CoplanType type, WindowContext *context)
{
	Index varno =  ++context->coplan_count;
	Coplan *coplan = palloc0(sizeof(Coplan));
	
	Assert ( type==COPLAN_WINDOW || type==COPLAN_AGG );
	Assert ( varno > 0 );
	
	coplan->type = type;
	coplan->varno = varno;
	coplan->num_aggs = 0;
	
	return coplan;
}

/* Return the atttribute number of the given target in the given coplan.
 *
 * The target may be a TargetEntry (in which case we copy it) or an Expr
 * (in which case we construct a TargetEntry and copy the Expr into it).
 * The result is the attribute number (resno) of the new target in the
 * coplan's target list.
 *
 * TODO Don't make a new target if an identical one exists already.
 * TODO API? 
 */
static AttrNumber addTargetToCoplan(Node *target, Coplan *coplan, WindowContext *context)
{
	char *name;
	TargetEntry *tle;
	AttrNumber resno = ++coplan->last_resno;
	
	if ( IsA(target,TargetEntry) )
	{
		tle = copyObject(target);
	}
	else
	{
		tle = makeTargetEntry(copyObject(target), resno, 
							  pstrdup("window_column"), false);
	}
	
	if ( IsA(target, Aggref) )
		coplan->num_aggs++;
	
	/* TODO Really should do better.  Need names for rangetable erefs. */
	if ( tle->resname != NULL )
		name = pstrdup(tle->resname);
	else
		name = pstrdup("coplan_target");
	
		
	tle->resno = resno;
	coplan->targetlist = lappend(coplan->targetlist, tle);
	coplan->targetnames = lappend(coplan->targetnames, makeString(name));
	
	Assert( resno == list_length(coplan->targetlist) );
	return resno;
}


static Aggref* makeWindowAggref(WindowRef *winref)
{
	Aggref *aggref = makeNode(Aggref);
	
	aggref->aggfnoid = winref->winfnoid;
	aggref->aggtype = winref->restype;
	aggref->args = copyObject(winref->args);
	aggref->agglevelsup = 0;
	aggref->aggstar = false; /* at this point in processing, doesn't matter */
	aggref->aggdistinct = false; /* not allowed in Window aggregates */
	aggref->aggstage = AGGSTAGE_NORMAL;

	return aggref;
}

static Aggref* makeAuxCountAggref()
{
	Aggref *aggref = makeNode(Aggref);
	
	aggref->aggfnoid = 2803; /* TODO count(*) oid define in pg_proc.h */
	aggref->aggtype = 20; /* TODO count(*) result type oid in pg_proc.h */
	aggref->args = NIL;
	aggref->agglevelsup = 0;
	aggref->aggstar = true; 
	aggref->aggdistinct = false; 
	aggref->aggstage = AGGSTAGE_NORMAL;
	
	return aggref;
}

/* Constructs a plan for a single coplan of a window query and returns it 
 * as a variant form of subquery RTE.  
 * 
 * The input subplan (and associated locus and pathkeys) should have the 
 * same result as the "common subquery", but need not be a plan for that 
 * query.  For example, it might be a ShareInputScan. The subplan is copied, 
 * not used directly. 
 *
 * XXX Perhaps copy should be optional based an additional argument?
 *
 * The generated RTE differs from an ordinary subquery RTE in that it
 * it holds the produced plan and associated pathkeys.  These are used
 * later in the optimizer short-circuit the recursive planning of the
 * RTE's subquery.
 *
 * TODO The spare fields that hold the plan aren't in, e.g., copyfuncs.c.
 *      Decide whether or not we need to add them!!!
 */

static RangeTblEntry *rte_for_coplan(
		ISHAREPLAN *share,
		PlannerInfo *root,
		Plan *input_plan, 
		CdbPathLocus subplan_locus,
		List *subplan_pathkeys,
		WindowInfo *winfo,
		Coplan *coplan)
{
	Query *subquery;
	RangeTblEntry *rte;
	Plan *new_plan;
	
	new_plan = condition_input(
						share,
						root,
						(share == &shareplan_simple) ? copyObject(input_plan) : input_plan,
						winfo->partkey_len, 
						winfo->partkey_attrs,
						winfo->sortclause,
						subplan_locus,
						&subplan_pathkeys);

	/* Now new_plan is suitable input for a coplan of this winfo. 
	 * Add the coplan processing.
	 */
	switch ( coplan->type )
	{
		case COPLAN_WINDOW:
			new_plan = (Plan*) 
				make_window(root, coplan->targetlist, 
						winfo->partkey_len, winfo->partkey_attrs,
						winfo->key_list, /* XXX copy? */
						new_plan);
		break;
	case COPLAN_AGG:
		if ( coplan->partkey_len > 0 )
		{
			/* Grouped aggregation. */
			
			/* TODO Fix this cheesy estimate. */
			double d = new_plan->plan_rows / 100.0;
			long num_groups = (d < 0)? 0: ( d > LONG_MAX )? LONG_MAX: (long)d;
			
			new_plan = (Plan*)
				make_agg(root, coplan->targetlist, NULL,
						 AGG_SORTED,
						 winfo->partkey_len, winfo->partkey_attrs,
						 num_groups,
						 0, /* num_nullcols */
						 0, /* input_grouping */
						 0, /* grouping_id */
						 0, /* rollup_gs_times */
						 coplan->num_aggs, coplan->trans_space,
						 new_plan);
		}
		else
		{
			/* Plain aggregation, no grouping. */
			new_plan = (Plan*)
				make_agg(root, coplan->targetlist, NULL,
						 AGG_PLAIN,
						 0, NULL,
						 1, /* number of groups */
						 0, /* num_nullcols */
						 0, /* input_grouping */
						 0, /* grouping_id */
						 0, /* rollup_gs_times */
						 coplan->num_aggs, coplan->trans_space,
						 new_plan);
		}
		break;
	default:
		elog(ERROR,"invalid window coplan");
	}

	if (!new_plan->flow)
		new_plan->flow = pull_up_Flow(new_plan, 
				new_plan->lefttree, 
				(subplan_pathkeys != NIL));

	/* The RTE we produce is pre-planned, but it should carry a plausible
	 * subquery.
	 */
	subquery = makeNode(Query);
	memcpy(subquery, root->parse, sizeof(Query));
	
	subquery->querySource = QSRC_PLANNER;
	subquery->canSetTag = false;
	subquery->resultRelation = 0;
	subquery->into = NULL;
	subquery->intoOptions = NIL;
	subquery->intoOnCommit = ONCOMMIT_NOOP;
	subquery->intoTableSpaceName = NULL;
	
	subquery->targetList = copyObject(new_plan->targetlist);
	subquery->windowClause = NIL;
	
	subquery->distinctClause = NIL;
	subquery->sortClause = NIL;
	subquery->limitOffset = NULL;
	subquery->limitCount = NULL;
	
	Assert( subquery->setOperations == NULL );
	
	/* Package up the RTE. 
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = subquery;
	rte->eref = makeNode(Alias);
	rte->eref->aliasname = pstrdup("coplan");
	rte->eref->colnames = coplan->targetnames;
	rte->subquery_plan = new_plan;
	rte->subquery_pathkeys = subplan_pathkeys;

	return rte;
}

/* 
 * The function plan_window_rtable_func brings together the coplans that make
 * up a window plan.  
 *
 * The ISHAREPLAN interface will points to the appropriate routines that will
 * dup or share the underlying subplan.
 */
static List * plan_window_rtable_share(ISHAREPLAN *share, 
		PlannerInfo* root, WindowContext *context)
{
	int i;
	RangeTblEntry *rte;
	List *rtable = NIL;
	
	if(share->init_ctxt)
		(*share->init_ctxt) (share->ctxt);

	for ( i = 0; i < context->nwindowinfos; i++ )
	{
		WindowInfo *winfo = &context->windowinfos[i];
		
		if ( winfo->window_coplan != NULL )
		{
			rte = rte_for_coplan(
					share,
					root, 
					context->subplan, 
					context->subplan_locus,
					context->subplan_pathkeys,
					winfo,
					winfo->window_coplan);

			rtable = lappend(rtable, rte);
		}

		if ( winfo->agg_coplan != NULL )
		{
			rte = rte_for_coplan(
					share,
					root, 
					context->subplan, 
					context->subplan_locus,
					context->subplan_pathkeys,
					winfo,
					winfo->agg_coplan);

			rtable = lappend(rtable, rte);
		}
	}

	Assert( list_length(rtable) == context->coplan_count );

	return rtable;
}

/* 
 * plan_window_rtable is the stub for plan_window_rtable_dup/share.
 *
 * XXX TODO: Right now we either always dup the subplan or always share
 * the sub plan, depending on a guc gp_window_shareinput.
 * Should make this cost based
 */
List *plan_window_rtable(PlannerInfo *root, WindowContext *ctxt)
{
	if((!gp_window_shareinput)
		|| ctxt->nwindowinfos == 0 
		|| (ctxt->nwindowinfos == 1 && 
				(ctxt->windowinfos[0].window_coplan == NULL ||
			 	 ctxt->windowinfos[0].agg_coplan == NULL)
	  	   )
	  )
		return plan_window_rtable_share(&shareplan_simple, root, ctxt);
	else 
	{
		ShareType stype = add_sharenode(
				root, 
				(Plan **) &ctxt->subplan, 
				&ctxt->subplan_pathkeys,
				ctxt->subplan_locus,
				(void *) ctxt,
				ctxt->nwindowinfos,
				get_windowctxt_partkey_len,
				get_windowctxt_sortclause
				);

		Assert(stype == SHARE_MATERIAL);
		Assert(IsA(ctxt->subplan, Material));
		return plan_window_rtable_share(&shareplan_ms, root, ctxt);
	}

	Assert(!"Should never reach");
	return NULL;
}


/* Plan the FROM-WHERE clauses to rejoin the coplans.
 */
static FromExpr *plan_window_jointree(PlannerInfo *root, WindowContext *context)
{
	FromExpr *jointree;
	List *from = NIL;
	List *quals = NIL;
	RangeTblRef *rtr;
	Coplan *coplan;
	int i,j;
	Oid *rowkey_types = NULL;
	Coplan *base_window_coplan = NULL;
	
	
	for ( i = 0; i < context->nwindowinfos; i++ )
	{
		WindowInfo *winfo = &context->windowinfos[i];
		
		for ( j = 0,	coplan = winfo->window_coplan; 
			  j < 2; 
			  j++,		coplan = winfo->agg_coplan ) 
		{
			if ( coplan != NULL ) 
			{
				rtr = makeNode(RangeTblRef);
				rtr->rtindex = coplan->varno;
				from = lappend(from, rtr);
			}
		}

		if ( context->rowkey_len > 0 )
		{
			/* Join window_coplans on the row key */
			if ( i == 0 )
			{
				Assert( winfo->window_coplan != NULL );
				base_window_coplan = winfo->window_coplan;
				rowkey_types = get_window_attr_types(((Plan *)(context->subplan))->targetlist, context->rowkey_len, context->rowkey_attrs);
			}
			else if ( winfo->window_coplan != NULL )
			{
				/* Append qual to join base_window_coplan to 
				 * winfo->window_coplan on the row key.
				 */
				for ( j = 0; j < context->rowkey_len; j++ )
				{
					Expr *term = make_window_join_term(rowkey_types[j], 
													   base_window_coplan->varno,
													   base_window_coplan->rowkey_attrs[j],
													   winfo->window_coplan->varno,
													   winfo->window_coplan->rowkey_attrs[j]);
					quals = lappend(quals, term);
				}
			}
		}
		
		if ( winfo->partkey_len > 0 && winfo->window_coplan != NULL && winfo->agg_coplan != NULL )
		{
			Oid *partkey_types;
			partkey_types = get_window_attr_types(context->lower_tlist, winfo->partkey_len, winfo->partkey_attrs);
			
			/* Append quals to join winfo->window_coplan to winfo->agg_coplan
			 * on the  the partitioning key. 
			 */
			for ( j = 0; j < winfo->partkey_len; j++ )
			{
				Expr *term = make_window_join_term(partkey_types[j], 
												   winfo->window_coplan->varno,
												   winfo->window_coplan->partkey_attrs[j],
												   winfo->agg_coplan->varno,
												   winfo->agg_coplan->partkey_attrs[j]);
				quals = lappend(quals, term);
			}
			
			pfree(partkey_types);
		}
	}
	if ( rowkey_types != NULL )
		pfree(rowkey_types);
		
	jointree = makeNode(FromExpr);
	jointree->fromlist = from;
	jointree->quals = (Node*)quals;
	
	return jointree;
}

static Expr *make_window_join_term(Oid type, Index lftvarno, AttrNumber lftattrno, Index rgtvarno, AttrNumber rgtattrno)
{
	Var *lft = makeVar(lftvarno, lftattrno, type, -1, 0);
	Var *rgt = makeVar(rgtvarno, rgtattrno, type, -1, 0);
	
	return make_op(NULL, list_make1(makeString("=")), (Node*)lft, (Node*)rgt, -1);
}

static Oid *get_window_attr_types(List *targetlist, int nattrs, AttrNumber *attrs)
{
	int i;
	Oid *types = palloc0(nattrs*sizeof(Oid));
	
	for ( i = 0; i < nattrs; i++ )
	{
		TargetEntry *tle = get_tle_by_resno(targetlist, attrs[i]);
		types[i] = exprType((Node*)tle->expr);
	}
	return types;
}


/* Translate a targetlist on the lower range (of the common subquery) to
 * a targetlist on the upper range.
 *
 * TODO Avoid reevaluating sort/group expressions that have already been
 *      evaluated in the common subquery. 
 */
static List *translate_upper_tlist(List *orig_tlist, WindowContext *context)
{
	List *new_tlist;
	
	new_tlist =  (List*)  
		expression_tree_mutator((Node*)orig_tlist,
								 translate_upper_tlist_mutator, 
								 (void*)context );
	
	return new_tlist;
}

/* Subroutine for translate_upper_tlist.
 */
static Node * translate_upper_tlist_mutator(Node *node, WindowContext *context)
{
	if ( node == NULL )
		return NULL;
		
	if ( IsA(node, Var) )
	{
		Var *new_var;
		Var *var = (Var *)node;
		
		/* We don't descend into WindowRefs, so Var isn't in one. */
		if ( var->varlevelsup == 0 )
		{
			int idx = index_of_var(var, context);
			AttrNumber attrno = context->offset_upper_varattrnos[idx];
			new_var = makeVar(
						1, /* first RTE has all entries */
						attrno,
						var->vartype,
						var->vartypmod,
						0 /* levels up known 0 */);
		}
		else
			new_var = copyObject(var);

		return (Node *) new_var;
	}
	else if ( IsA(node, WindowRef) )
	{
		WindowRef *ref = (WindowRef *)node;
		RefInfo *rinfo = (RefInfo *)list_nth(context->refinfos, ref->winindex);
		return copyObject(rinfo->resultexpr);
	}
	
	return expression_tree_mutator(node, translate_upper_tlist_mutator, (void*) context);
}
