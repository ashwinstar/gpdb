/*-------------------------------------------------------------------------
 *
 * cdbplan.c
 *	  Provides routines supporting plan tree manipulation.
 *
 * Copyright (c) 2004-2006, Greenplum
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	See src/backend/optimizer/util/clauses.c for background information
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "cdb/cdbplan.h"

static bool walk_plan_node_fields(Plan *plan, bool (*walker) (), void *context);
static bool walk_scan_node_fields(Scan *scan, bool (*walker) (), void *context);
static bool walk_join_node_fields(Join *join, bool (*walker) (), void *context);

static void mutate_plan_fields(Plan *newplan, Plan *oldplan, Node *(*mutator) (), void *context);
static void mutate_join_fields(Join *newplan, Join *oldplan, Node *(*mutator) (), void *context);


/* ----------------------------------------------------------------------- *
 * Plan Tree Walker Framework
 * ----------------------------------------------------------------------- *
 */

/* Function walk_plan_node_fields is a subroutine used by plan_tree_walker()
 * to walk the fields of Plan nodes.  Plan is actually an abstract superclass
 * of all plan nodes and this function encapsulates the common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.
 *
 * Caution: walk_scan_node_fields and walk_join_node_fields call this
 * function.  Use only the most specific function.
 */
bool
walk_plan_node_fields(Plan *plan,
					  bool (*walker) (),
					  void *context)
{
	/* target list to be computed at this node */
	if (walker((Node *) (plan->targetlist), context))
		return true;

	/* implicitly ANDed qual conditions */
	if (walker((Node *) (plan->qual), context))
		return true;

	/* input plan tree(s) */
	if (walker((Node *) (plan->lefttree), context))
		return true;

	/* target list to be computed at this node */
	if (walker((Node *) (plan->righttree), context))
		return true;

	/* Init Plan nodes (uncorrelated expr subselects */
	if (walker((Node *) (plan->initPlan), context))
		return true;

	/* Greenplum Database Flow node */
	if (walker((Node *) (plan->flow), context))
		return true;

	return false;
}


/* Function walk_scan_node_fields is a subroutine used by plan_tree_walker()
 * to walk the fields of Scan nodes.  Scan is actually an abstract superclass
 * of all scan nodes and a subclass of Plan.  This function encapsulates the
 * common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.
 *
 * Caution: This function calls walk_plan_node_fields so callers shouldn't,
 * else they will walk common plan fields twice.
 */
bool
walk_scan_node_fields(Scan *scan,
					  bool (*walker) (),
					  void *context)
{
	/* A Scan node is a kind of Plan node. */
	if (walk_plan_node_fields((Plan *) scan, walker, context))
		return true;

	/* The only additional field is an Index so no extra walking. */
	return false;
}


/* Function walk_join_node_fields is a subroutine used by plan_tree_walker()
 * to walk the fields of Join nodes.  Join is actually an abstract superclass
 * of all join nodes and a subclass of Plan.  This function encapsulates the
 * common structure.
 *
 * Most specific walkers won't need to call this function, but complicated
 * ones may find it a useful utility.
 *
 * Caution: This function calls walk_plan_node_fields so callers shouldn't,
 * else they will walk common plan fields twice.
 */
bool
walk_join_node_fields(Join *join,
					  bool (*walker) (),
					  void *context)
{
	/* A Join node is a kind of Plan node. */
	if (walk_plan_node_fields((Plan *) join, walker, context))
		return true;

	return walker((Node *) (join->joinqual), context);
}


/* Function plan_tree_walker is a general walker for Plan trees.
 *
 * It is based on (and uses) the expression_tree_walker() framework from
 * src/backend/optimizer/util/clauses.c.  See the comments there for more
 * insight into how this function works.
 *
 * The basic idea is that this function (and its helpers) walk plan-specific
 * nodes and delegate other nodes to expression_tree_walker().	The caller
 * may supply a specialized walker
 */
bool
plan_tree_walker(Node *node,
				 bool (*walker) (),
				 void *context)
{
	/*
	 * The walker has already visited the current node, and so we need
	 * only recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se, so
	 * when we expect a List we just recurse directly to self without
	 * bothering to call the walker.
	 */
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * Plan nodes aren't handled by expression_tree_walker, so we need
			 * to do them here.
			 */
		case T_Plan:
			/* Abstract: really should see only subclasses. */
			return walk_plan_node_fields((Plan *) node, walker, context);

		case T_Result:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			if (walker((Node *) ((Result *) node)->resconstantqual, context))
				return true;
			break;

		case T_Append:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			if (walker((Node *) ((Append *) node)->appendplans, context))
				return true;
			break;

        case T_BitmapAnd:
            if (walk_plan_node_fields((Plan *) node, walker, context))
                return true;
            if (walker((Node *) ((BitmapAnd *) node)->bitmapplans, context))
                return true;
            break;
        case T_BitmapOr:
            if (walk_plan_node_fields((Plan *) node, walker, context))
                return true;
            if (walker((Node *) ((BitmapOr *) node)->bitmapplans, context))
                return true;
            break;

		case T_Scan:
			/* Abstract: really should see only subclasses. */
			return walk_scan_node_fields((Scan *) node, walker, context);

		case T_SeqScan:
		case T_ExternalScan:
		case T_BitmapHeapScan:
		case T_FunctionScan:
		case T_ValuesScan:
			if (walk_scan_node_fields((Scan *) node, walker, context))
				return true;
			break;

		case T_IndexScan:
			if (walk_scan_node_fields((Scan *) node, walker, context))
				return true;
			if (walker((Node *) ((IndexScan *) node)->indexqual, context))
				return true;
			/* Other fields are lists of basic items, nothing to walk. */
			break;
			
		case T_BitmapIndexScan:
			if (walk_scan_node_fields((Scan *) node, walker, context))
				return true;
			if (walker((Node *) ((BitmapIndexScan *) node)->indexqual, context))
				return true;
			/* Other fields are lists of basic items, nothing to walk. */
			break;

		case T_TidScan:
			if (walk_scan_node_fields((Scan *) node, walker, context))
				return true;
			if (walker((Node *) ((TidScan *) node)->tidquals, context))
				return true;
			break;

		case T_SubqueryScan:
			if (walk_scan_node_fields((Scan *) node, walker, context))
				return true;
			if (walker((Node *) ((SubqueryScan *) node)->subplan, context))
				return true;
			break;

		case T_Join:
			/* Abstract: really should see only subclasses. */
			return walk_join_node_fields((Join *) node, walker, context);

		case T_NestLoop:
			if (walk_join_node_fields((Join *) node, walker, context))
				return true;
			break;

		case T_MergeJoin:
			if (walk_join_node_fields((Join *) node, walker, context))
				return true;
			if (walker((Node *) ((MergeJoin *) node)->mergeclauses, context))
				return true;
			break;

		case T_HashJoin:
			if (walk_join_node_fields((Join *) node, walker, context))
				return true;
			if (walker((Node *) ((HashJoin *) node)->hashclauses, context))
				return true;
			break;

		case T_Material:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			break;

		case T_Sort:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple counts and lists of indexes and oids. */
			break;

		case T_Group:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple count and lists of indexes. */
			break;

		case T_Agg:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;

		case T_Unique:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;

		case T_Hash:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other info is in parent HashJoin node. */
			break;

		case T_SetOp:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;

		case T_Limit:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
			/* Other fields are simple items and lists of simple items. */
			break;

		case T_Motion:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;

			if (walker((Node *) ((Motion *) node)->hashExpr, context))
				return true;

			if (walker((Node *) ((Motion *) node)->hashDataTypes, context))
				return true;

			break;

		case T_AdaptiveNestLoop:
			if (walk_plan_node_fields((Plan *) node, walker, context))
				return true;
            break;

		case T_Flow:
			/* Nothing to walk in Flow nodes at present. */
			break;

		case T_SubPlan:

			/*
			 * SubPlan is handled by expression_tree_walker() , but that
			 * walker doesn't recur into the plan itself.  Here, we do the
			 * whole item, though users of the function will probably need to
			 * update their context to reflect the range table.
			 */
			{
				SubPlan    *subplan = (SubPlan *) node;

				/* recurse into the exprs list */
				if (expression_tree_walker((Node *) subplan->testexpr,
										   walker, context))
					return true;

				/* recur into the subplan, a kind of Plan node */
				if (walker((Node *) subplan->plan, context))
					return true;

				/* also examine args list */
				if (expression_tree_walker((Node *) subplan->args,
										   walker, context))
					return true;
			}
			break;

		case T_IntList:
		case T_OidList:

			/*
			 * Note that expression_tree_walker handles T_List but not these.
			 * No contained stuff, so just succeed.
			 */
			break;

			/*
			 * Query nodes are handled by the Postgres query_tree_walker. We
			 * just use it without setting any ignore flags.  The caller must
			 * handle query nodes specially to get other behavior, e.g. by
			 * calling query_tree_walker directly.
			 */
		case T_Query:
			return query_tree_walker((Query *) node, walker, context, 0);

			/*
			 * The following cases are handled by expression_tree_walker.  In
			 * addition, we let expression_tree_walker handle unrecognized
			 * nodes.
			 *
			 * TODO: Identify node types that should never appear in plan trees
			 * and disallow them here by issuing an error or asserting false.
			 */
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_RangeTblRef:
		case T_Aggref:
		case T_ArrayRef:
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_ScalarArrayOpExpr:
		case T_BoolExpr:
		case T_SubLink:
		case T_FieldSelect:
		case T_FieldStore:
		case T_RelabelType:
		case T_CaseExpr:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_NullIfExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CoerceToDomain:
		case T_TargetEntry:
		case T_List:
		case T_FromExpr:
		case T_JoinExpr:
		case T_SetOperationStmt:
		case T_InClauseInfo:

		default:
			return expression_tree_walker(node, walker, context);
	}
	return false;
}



/* ----------------------------------------------------------------------- *
 * Plan Tree Mutator Framework
 * ----------------------------------------------------------------------- *
 */

/*--------------------
 * expression_tree_mutator() is designed to support routines that make a
 * modified copy of an expression tree, with some nodes being added,
 * removed, or replaced by new subtrees.  The original tree is (normally)
 * not changed.  Each recursion level is responsible for returning a copy of
 * (or appropriately modified substitute for) the subtree it is handed.
 * A mutator routine should look like this:
 *
 * Node * my_mutator (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return NULL;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... create and return modified copy of Var node
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special transformations of other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_mutator(node, my_mutator, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the mutator routine needs --- it can be used to return extra
 * data gathered by the mutator, too.  This argument is not touched by
 * expression_tree_mutator, but it is passed down to recursive sub-invocations
 * of my_mutator.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_mutator with the
 * top-level node of the tree, and does any required post-processing.
 *
 * Each level of recursion must return an appropriately modified Node.
 * If expression_tree_mutator() is called, it will make an exact copy
 * of the given Node, but invoke my_mutator() to copy the sub-node(s)
 * of that Node.  In this way, my_mutator() has full control over the
 * copying process but need not directly deal with expression trees
 * that it has no interest in.
 *
 * Just as for expression_tree_walker, the node types handled by
 * expression_tree_mutator include all those normally found in target lists
 * and qualifier clauses during the planning stage.
 *
 * expression_tree_mutator will handle SubLink nodes by recursing normally
 * into the "lefthand" arguments (which are expressions belonging to the outer
 * plan).  It will also call the mutator on the sub-Query node; however, when
 * expression_tree_mutator itself is called on a Query node, it does nothing
 * and returns the unmodified Query node.  The net effect is that unless the
 * mutator does something special at a Query node, sub-selects will not be
 * visited or modified; the original sub-select will be linked to by the new
 * SubLink node.  Mutators that want to descend into sub-selects will usually
 * do so by recognizing Query nodes and calling query_tree_mutator (below).
 *
 * expression_tree_mutator will handle a SubPlan node by recursing into
 * the "exprs" and "args" lists (which belong to the outer plan), but it
 * will simply copy the link to the inner plan, since that's typically what
 * expression tree mutators want.  A mutator that wants to modify the subplan
 * can force appropriate behavior by recognizing SubPlan expression nodes
 * and doing the right thing.
 *--------------------
 */

Node *
plan_tree_mutator(Node *node,
				  Node *(*mutator) (),
				  void *context)
{
	/*
	 * The mutator has already decided not to modify the current node, but
	 * we must call the mutator for any sub-nodes.
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define CHECKFLATCOPY(newnode, node, nodetype)	\
	( AssertMacro(IsA((node), nodetype)), \
	  (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), context) )

#define PLANMUTATE(newplan, oldplan) \
		mutate_plan_fields((Plan*)(newplan), (Plan*)(oldplan), mutator, context)

/* This is just like  PLANMUTATE because Scan adds only scalar fields. */
#define SCANMUTATE(newplan, oldplan) \
		mutate_plan_fields((Plan*)(newplan), (Plan*)(oldplan), mutator, context)

#define JOINMUTATE(newplan, oldplan) \
		mutate_join_fields((Join*)(newplan), (Join*)(oldplan), mutator, context)

#define COPYARRAY(dest,src,lenfld,datfld) \
	do { \
		(dest)->lenfld = (src)->lenfld; \
		if ( (src)->lenfld > 0  && \
             (src)->datfld != NULL) \
		{ \
			Size _size = ((src)->lenfld*sizeof(*((src)->datfld))); \
			(dest)->datfld = palloc(_size); \
			memcpy((dest)->datfld, (src)->datfld, _size); \
		} \
		else \
		{ \
			(dest)->datfld = NULL; \
		} \
	} while (0)



	if (node == NULL)
		return NULL;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * Plan nodes aren't handled by expression_tree_walker, so we need
			 * to do them here.
			 */
		case T_Plan:
			/* Abstract: Should see only subclasses. */
			elog(ERROR, "abstract node type not allowed: T_Plan");

		case T_Result:
			{
				Result	   *result = (Result *) node;
				Result	   *newresult;

				FLATCOPY(newresult, result, Result);
				PLANMUTATE(newresult, result);
				MUTATE(newresult->resconstantqual, result->resconstantqual, Node *);
				return (Node *) newresult;
			}
			break;

		case T_Append:
			{
				Append	   *append = (Append *) node;
				Append	   *newappend;

				FLATCOPY(newappend, append, Append);
				PLANMUTATE(newappend, append);
				MUTATE(newappend->appendplans, append->appendplans, List *);
				/* isTarget is scalar. */
				return (Node *) newappend;
			}
			break;

        case T_BitmapAnd:
			{
				BitmapAnd  *old = (BitmapAnd *) node;
                BitmapAnd  *mut;

				FLATCOPY(mut, old, BitmapAnd);
				PLANMUTATE(mut, old);
				MUTATE(mut->bitmapplans, old->bitmapplans, List *);
				return (Node *)mut;
			}
			break;
        case T_BitmapOr:
            {
                BitmapOr   *old = (BitmapOr *) node;
                BitmapOr   *mut;

                FLATCOPY(mut, old, BitmapOr);
                PLANMUTATE(mut, old);
                MUTATE(mut->bitmapplans, old->bitmapplans, List *);
                return (Node *)mut;
            }
            break;

		case T_Scan:
			/* Abstract: Should see only subclasses. */
			elog(ERROR, "abstract node type not allowed: T_Scan");

		case T_SeqScan:
			{
				SeqScan    *seqscan = (SeqScan *) node;
				SeqScan    *newseqscan;

				FLATCOPY(newseqscan, seqscan, SeqScan);
				SCANMUTATE(newseqscan, seqscan);
				/* A SeqScan is really just a Scan, so we're done. */
				return (Node *) newseqscan;
			}
			break;

		case T_ExternalScan:
			{
				ExternalScan    *extscan = (ExternalScan *) node;
				ExternalScan    *newextscan;
				
				FLATCOPY(newextscan, extscan, ExternalScan);
				SCANMUTATE(newextscan, extscan);
				
				MUTATE(newextscan->uriList, extscan->uriList, List *);
				MUTATE(newextscan->fmtOpts, extscan->fmtOpts, List *);
				newextscan->isText = extscan->isText;
				
				return (Node *) newextscan;
			}
			break;
			
		case T_IndexScan:
			{
				IndexScan  *idxscan = (IndexScan *) node;
				IndexScan  *newidxscan;

				FLATCOPY(newidxscan, idxscan, IndexScan);
				SCANMUTATE(newidxscan, idxscan);
				newidxscan->indexid = idxscan->indexid;
				//MUTATE(newidxscan->indexid, idxscan->indexid, List *);
				MUTATE(newidxscan->indexqual, idxscan->indexqual, List *);
				MUTATE(newidxscan->indexqualorig, idxscan->indexqualorig, List *);
				MUTATE(newidxscan->indexstrategy, idxscan->indexstrategy, List *);
				MUTATE(newidxscan->indexsubtype, idxscan->indexsubtype, List *);
				/* indxorderdir  is  scalar */
				return (Node *) newidxscan;
			}
			break;
			
		case T_BitmapIndexScan:
			{
				BitmapIndexScan  *idxscan = (BitmapIndexScan *) node;
				BitmapIndexScan  *newidxscan;

				FLATCOPY(newidxscan, idxscan, BitmapIndexScan);
				SCANMUTATE(newidxscan, idxscan);
				newidxscan->indexid = idxscan->indexid;

				MUTATE(newidxscan->indexqual, idxscan->indexqual, List *);
				MUTATE(newidxscan->indexqualorig, idxscan->indexqualorig, List *);
				MUTATE(newidxscan->indexstrategy, idxscan->indexstrategy, List *);
				MUTATE(newidxscan->indexsubtype, idxscan->indexsubtype, List *);

				return (Node *) newidxscan;
			}
			break;
			
		case T_BitmapHeapScan:
			{
				BitmapHeapScan  *bmheapscan = (BitmapHeapScan *) node;
				BitmapHeapScan  *newbmheapscan;

				FLATCOPY(newbmheapscan, bmheapscan, BitmapHeapScan);
				SCANMUTATE(newbmheapscan, bmheapscan);
				
				MUTATE(newbmheapscan->bitmapqualorig, bmheapscan->bitmapqualorig, List *);
	
				return (Node *) newbmheapscan;
			}
			break;
		

		case T_TidScan:
			{
				TidScan    *tidscan = (TidScan *) node;
				TidScan    *newtidscan;

				FLATCOPY(newtidscan, tidscan, TidScan);
				SCANMUTATE(newtidscan, tidscan);
				MUTATE(newtidscan->tidquals, tidscan->tidquals, List *);
				/* isTarget is scalar. */
				return (Node *) newtidscan;
			}
			break;

		case T_SubqueryScan:
			{
				SubqueryScan *sqscan = (SubqueryScan *) node;
				SubqueryScan *newsqscan;

				FLATCOPY(newsqscan, sqscan, SubqueryScan);
				SCANMUTATE(newsqscan, sqscan);
				MUTATE(newsqscan->subplan, sqscan->subplan, Plan *);
				return (Node *) newsqscan;
			}
			break;

		case T_FunctionScan:
			{
				FunctionScan *fnscan = (FunctionScan *) node;
				FunctionScan *newfnscan;

				FLATCOPY(newfnscan, fnscan, FunctionScan);
				SCANMUTATE(newfnscan, fnscan);
				/* A FunctionScan is really just a Scan, so we're done. */
				return (Node *) newfnscan;
			}
			break;

		case T_ValuesScan:
			{
				ValuesScan *scan = (ValuesScan *)node;
				ValuesScan *newscan;

				FLATCOPY(newscan, scan, ValuesScan);
				SCANMUTATE(newscan, scan);
				return (Node *) newscan;
			}
			break;

		case T_Join:
			/* Abstract: Should see only subclasses. */
			elog(ERROR, "abstract node type not allowed: T_Join");

		case T_NestLoop:
			{
				NestLoop   *loopscan = (NestLoop *) node;
				NestLoop   *newloopscan;

				FLATCOPY(newloopscan, loopscan, NestLoop);
				JOINMUTATE(newloopscan, loopscan);

				/* A NestLoop is really just a Join. */
				return (Node *) newloopscan;
			}
			break;
		case T_AdaptiveNestLoop:
			/* This may walk the shared_outer twice; but it has copied
			 * it as part of the mutate, so the underlying nodes don't
			 * get confused
			 */
			{
				AdaptiveNestLoop   *loop = (AdaptiveNestLoop *) node;
				AdaptiveNestLoop   *newloop;

				FLATCOPY(newloop, loop, AdaptiveNestLoop);
				PLANMUTATE(newloop, loop);

				return (Node *) newloop;
			}
			break;
		case T_MergeJoin:
			{
				MergeJoin  *merge = (MergeJoin *) node;
				MergeJoin  *newmerge;

				FLATCOPY(newmerge, merge, MergeJoin);
				JOINMUTATE(newmerge, merge);
				MUTATE(newmerge->mergeclauses, merge->mergeclauses, List *);
				return (Node *) newmerge;
			}
			break;

		case T_HashJoin:
			{
				HashJoin   *hjoin = (HashJoin *) node;
				HashJoin   *newhjoin;

				FLATCOPY(newhjoin, hjoin, HashJoin);
				JOINMUTATE(newhjoin, hjoin);
				MUTATE(newhjoin->hashclauses, hjoin->hashclauses, List *);
				return (Node *) newhjoin;
			}
			break;

		case T_Material:
			{
				Material   *material = (Material *) node;
				Material   *newmaterial;

				FLATCOPY(newmaterial, material, Material);
				PLANMUTATE(newmaterial, material);
				return (Node *) newmaterial;
			}
			break;

		case T_Sort:
			{
				Sort	   *sort = (Sort *) node;
				Sort	   *newsort;

				FLATCOPY(newsort, sort, Sort);
				PLANMUTATE(newsort, sort);
				COPYARRAY(newsort, sort, numCols, sortColIdx);
				COPYARRAY(newsort, sort, numCols, sortOperators);
				return (Node *) newsort;
			}
			break;

		case T_Group:
			{
				Group	   *group = (Group *) node;
				Group	   *newgroup;

				FLATCOPY(newgroup, group, Group);
				PLANMUTATE(newgroup, group);
				COPYARRAY(newgroup, group, numCols, grpColIdx);
				return (Node *) newgroup;
			}
			break;

		case T_Agg:
			{
				Agg		   *agg = (Agg *) node;
				Agg		   *newagg;

				FLATCOPY(newagg, agg, Agg);
				PLANMUTATE(newagg, agg);
				COPYARRAY(newagg, agg, numCols, grpColIdx);
				return (Node *) newagg;
			}
			break;

		case T_Unique:
			{
				Unique	   *uniq = (Unique *) node;
				Unique	   *newuniq;

				FLATCOPY(newuniq, uniq, Unique);
				PLANMUTATE(newuniq, uniq);
				COPYARRAY(newuniq, uniq, numCols, uniqColIdx);
				return (Node *) newuniq;
			}
			break;

		case T_Hash:
			{
				Hash	   *hash = (Hash *) node;
				Hash	   *newhash;

				FLATCOPY(newhash, hash, Hash);
				PLANMUTATE(newhash, hash);
				return (Node *) newhash;
			}
			break;

		case T_SetOp:
			{
				SetOp	   *setop = (SetOp *) node;
				SetOp	   *newsetop;

				FLATCOPY(newsetop, setop, SetOp);
				PLANMUTATE(newsetop, setop);
				COPYARRAY(newsetop, setop, numCols, dupColIdx);
				return (Node *) newsetop;
			}
			break;

		case T_Limit:
			{
				Limit	   *limit = (Limit *) node;
				Limit	   *newlimit;

				FLATCOPY(newlimit, limit, Limit);
				PLANMUTATE(newlimit, limit);
				MUTATE(newlimit->limitOffset, limit->limitOffset, Node *);
				MUTATE(newlimit->limitCount, limit->limitCount, Node *);
				return (Node *) newlimit;
			}
			break;

		case T_Motion:
			{
				Motion	   *motion = (Motion *) node;
				Motion	   *newmotion;

				FLATCOPY(newmotion, motion, Motion);
				PLANMUTATE(newmotion, motion);
                COPYARRAY(newmotion, motion, numInputSegs, inputSegIdx);
				COPYARRAY(newmotion, motion, numHashSegs, hashSegIdx);
				MUTATE(newmotion->hashExpr, motion->hashExpr, List *);
				MUTATE(newmotion->hashDataTypes, motion->hashDataTypes, List *);
				COPYARRAY(newmotion, motion, numOutputSegs, outputSegIdx);
				COPYARRAY(newmotion, motion, numSortCols, sortColIdx);
				COPYARRAY(newmotion, motion, numSortCols, sortOperators);
				return (Node *) newmotion;
			}
			break;

		case T_Flow:
			{
				Flow	   *flow = (Flow *) node;
				Flow	   *newflow;

				FLATCOPY(newflow, flow, Flow);
				MUTATE(newflow->hashExpr, flow->hashExpr, List *);
				COPYARRAY(newflow, flow, numSortCols, sortColIdx);
				COPYARRAY(newflow, flow, numSortCols, sortOperators);
				return (Node *) newflow;
			}
			break;

		case T_IntList:
		case T_OidList:

			/*
			 * Note that expression_tree_mutator handles T_List but not these.
			 * A shallow copy will do.
			 */
			return (Node *) list_copy((List *) node);

			break;

		case T_Query:

			/*
			 * Since expression_tree_mutator doesn't descend into Query nodes,
			 * we use ...
			 */
			return (Node *) query_tree_mutator((Query *) node, mutator, context, 0);
			break;

		case T_SubPlan:

			/*
			 * Since expression_tree_mutator doesn't descend into the plan in
			 * a SubPlan node, we handle the case directly.
			 */
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlan    *newnode;

				FLATCOPY(newnode, subplan, SubPlan);

				MUTATE(newnode->testexpr, subplan->testexpr, Node *);
				MUTATE(newnode->plan, subplan->plan, Plan *);
				MUTATE(newnode->rtable, subplan->rtable, List *);
				MUTATE(newnode->args, subplan->args, List *);

                /* An IntList isn't interesting to mutate; just copy. */
				newnode->paramIds = (List *)copyObject(subplan->paramIds);
				newnode->setParam = (List *)copyObject(subplan->setParam);
				newnode->parParam = (List *)copyObject(subplan->parParam);

				return (Node *) newnode;
			}
			break;

		case T_RangeTblEntry:

			/*
			 * Also expression_tree_mutator doesn't recognize range table
			 * entries.
			 *
			 * TODO Figure out what's to do and handle this case. ***************************************************
			 *
			 */
			{
				RangeTblEntry *rte = (RangeTblEntry *) node;
				RangeTblEntry *newrte;

				FLATCOPY(newrte, rte, RangeTblEntry);
				switch (rte->rtekind)
				{
					case RTE_RELATION:	/* ordinary relation reference */
					case RTE_SPECIAL:	/* special rule relation (NEW or OLD) */
                    case RTE_VOID:      /* deleted entry */
						/* No extras. */
						break;

					case RTE_SUBQUERY:	/* subquery in FROM */
						newrte->subquery = copyObject(rte->subquery);
						break;

					case RTE_JOIN:		/* join */
						newrte->joinaliasvars = copyObject(rte->joinaliasvars);
						break;

					case RTE_FUNCTION:	/* function in FROM */
						MUTATE(newrte->funcexpr, rte->funcexpr, Node *);
						// TODO is this right? //newrte->coldeflist = (List *) copyObject(rte->coldeflist);
						break;

                    case RTE_VALUES:
				        MUTATE(newrte->values_lists, rte->values_lists, List *);
				        break;
				}
				return (Node *) newrte;
			}
			break;

			/*
			 * The following cases are handled by expression_tree_mutator.	In
			 * addition, we let expression_tree_mutator handle unrecognized
			 * nodes.
			 *
			 * TODO: Identify node types that should never appear in plan trees
			 * and disallow them here by issuing an error or asserting false.
			 */
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_RangeTblRef:
		case T_Aggref:
		case T_ArrayRef:
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_ScalarArrayOpExpr:
		case T_BoolExpr:
		case T_SubLink:
		case T_FieldSelect:
		case T_FieldStore:
		case T_RelabelType:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_NullIfExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CoerceToDomain:
		case T_TargetEntry:
		case T_List:
		case T_FromExpr:
		case T_JoinExpr:
		case T_SetOperationStmt:
		case T_InClauseInfo:

		default:

			/*
			 * Let expression_tree_mutator handle remaining cases or complain
			 * of unrecognized node type.
			 */
			return expression_tree_mutator(node, mutator, context);
			break;
	}
	/* can't get here, but keep compiler happy */
	return NULL;
}


/* Function mutate_plan_fields() is a subroutine for plan_tree_mutator().
 * It "hijacks" the macro MUTATE defined for use in that function, so don't
 * change the argument names "mutator" and "context" use in the macro
 * definition.
 *
 */
void		mutate_plan_fields(Plan *newplan, Plan *oldplan, Node *(*mutator) (), void *context)
{
	/*
	 * Scalar fields startup_cost total_cost plan_rows plan_width nParamExec
	 * need no mutation.
	 */

	/* Node fields need mutation. */
	MUTATE(newplan->targetlist, oldplan->targetlist, List *);
	MUTATE(newplan->qual, oldplan->qual, List *);
	MUTATE(newplan->lefttree, oldplan->lefttree, Plan *);
	MUTATE(newplan->righttree, oldplan->righttree, Plan *);
	MUTATE(newplan->initPlan, oldplan->initPlan, List *);

	/* Bitmapsets aren't nodes but need to be copied to palloc'd space. */
	newplan->extParam = bms_copy(oldplan->extParam);
	newplan->allParam = bms_copy(oldplan->allParam);
}


/* Function mutate_plan_fields() is a subroutine for plan_tree_mutator().
 * It "hijacks" the macro MUTATE defined for use in that function, so don't
 * change the argument names "mutator" and "context" use in the macro
 * definition.
 *
 */
void		mutate_join_fields(Join *newjoin, Join *oldjoin, Node *(*mutator) (), void *context)
{
	/* A Join node is a Plan node. */
	mutate_plan_fields((Plan *) newjoin, (Plan *) oldjoin, mutator, context);

	/* Scalar field jointype needs no mutation. */

	/* Node fields need mutation. */
	MUTATE(newjoin->joinqual, oldjoin->joinqual, List *);
}
