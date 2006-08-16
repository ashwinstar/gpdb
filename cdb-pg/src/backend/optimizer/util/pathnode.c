/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/pathnode.c,v 1.125 2005/10/15 02:49:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "cdb/cdbpath.h"        /* cdb_join_motion() etc */

static List *translate_sub_tlist(List *tlist, int relid);
static bool query_is_distinct_for(Query *query, List *colnos);
static bool hash_safe_tlist(List *tlist);


/*****************************************************************************
 *		MISC. PATH UTILITIES
 *****************************************************************************/

/*
 * compare_path_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for the specified criterion.
 */
int
compare_path_costs(Path *path1, Path *path2, CostSelector criterion)
{
	if (criterion == STARTUP_COST)
	{
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;

		/*
		 * If paths have the same startup cost (not at all unlikely), order
		 * them by total cost.
		 */
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;
	}
	else
	{
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;

		/*
		 * If paths have the same total cost, order them by startup cost.
		 */
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;
	}
	return 0;
}

/*
 * compare_fuzzy_path_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for the specified criterion.
 *
 * This differs from compare_path_costs in that we consider the costs the
 * same if they agree to within a "fuzz factor".  This is used by add_path
 * to avoid keeping both of a pair of paths that really have insignificantly
 * different cost.
 */
static int
compare_fuzzy_path_costs(Path *path1, Path *path2, CostSelector criterion)
{
	/*
	 * We use a fuzz factor of 1% of the smaller cost.
	 *
	 * XXX does this percentage need to be user-configurable?
	 */
	if (criterion == STARTUP_COST)
	{
		if (path1->startup_cost > path2->startup_cost * 1.01)
			return +1;
		if (path2->startup_cost > path1->startup_cost * 1.01)
			return -1;

		/*
		 * If paths have the same startup cost (not at all unlikely), order
		 * them by total cost.
		 */
		if (path1->total_cost > path2->total_cost * 1.01)
			return +1;
		if (path2->total_cost > path1->total_cost * 1.01)
			return -1;
	}
	else
	{
		if (path1->total_cost > path2->total_cost * 1.01)
			return +1;
		if (path2->total_cost > path1->total_cost * 1.01)
			return -1;

		/*
		 * If paths have the same total cost, order them by startup cost.
		 */
		if (path1->startup_cost > path2->startup_cost * 1.01)
			return +1;
		if (path2->startup_cost > path1->startup_cost * 1.01)
			return -1;
	}
	return 0;
}

/*
 * compare_path_fractional_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for fetching the specified fraction
 *	  of the total tuples.
 *
 * If fraction is <= 0 or > 1, we interpret it as 1, ie, we select the
 * path with the cheaper total_cost.
 */
int
compare_fractional_path_costs(Path *path1, Path *path2,
							  double fraction)
{
	Cost		cost1,
				cost2;

	if (fraction <= 0.0 || fraction >= 1.0)
		return compare_path_costs(path1, path2, TOTAL_COST);
	cost1 = path1->startup_cost +
		fraction * (path1->total_cost - path1->startup_cost);
	cost2 = path2->startup_cost +
		fraction * (path2->total_cost - path2->startup_cost);
	if (cost1 < cost2)
		return -1;
	if (cost1 > cost2)
		return +1;
	return 0;
}

/*
 * set_cheapest
 *	  Find the minimum-cost paths from among a relation's paths,
 *	  and save them in the rel's cheapest-path fields.
 *
 * This is normally called only after we've finished constructing the path
 * list for the rel node.
 *
 * If we find two paths of identical costs, try to keep the better-sorted one.
 * The paths might have unrelated sort orderings, in which case we can only
 * guess which might be better to keep, but if one is superior then we
 * definitely should keep it.
 */
void
set_cheapest(RelOptInfo *parent_rel)
{
	List	   *pathlist = parent_rel->pathlist;
	ListCell   *p;
	Path	   *cheapest_startup_path;
	Path	   *cheapest_total_path;

	Assert(IsA(parent_rel, RelOptInfo));

	if (pathlist == NIL)
		elog(ERROR, "could not devise a query plan for the given query");

	cheapest_startup_path = cheapest_total_path = (Path *) linitial(pathlist);

	for_each_cell(p, lnext(list_head(pathlist)))
	{
		Path	   *path = (Path *) lfirst(p);
		int			cmp;

		cmp = compare_path_costs(cheapest_startup_path, path, STARTUP_COST);
		if (cmp > 0 ||
			(cmp == 0 &&
			 compare_pathkeys(cheapest_startup_path->pathkeys,
							  path->pathkeys) == PATHKEYS_BETTER2))
			cheapest_startup_path = path;

		cmp = compare_path_costs(cheapest_total_path, path, TOTAL_COST);
		if (cmp > 0 ||
			(cmp == 0 &&
			 compare_pathkeys(cheapest_total_path->pathkeys,
							  path->pathkeys) == PATHKEYS_BETTER2))
			cheapest_total_path = path;
	}

	parent_rel->cheapest_startup_path = cheapest_startup_path;
	parent_rel->cheapest_total_path = cheapest_total_path;
	parent_rel->cheapest_unique_path = NULL;	/* computed only if needed */
}

/*
 * add_path
 *	  Consider a potential implementation path for the specified parent rel,
 *	  and add it to the rel's pathlist if it is worthy of consideration.
 *	  A path is worthy if it has either a better sort order (better pathkeys)
 *	  or cheaper cost (on either dimension) than any of the existing old paths.
 *
 *	  We also remove from the rel's pathlist any old paths that are dominated
 *	  by new_path --- that is, new_path is both cheaper and at least as well
 *	  ordered.
 *
 *	  The pathlist is kept sorted by TOTAL_COST metric, with cheaper paths
 *	  at the front.  No code depends on that for correctness; it's simply
 *	  a speed hack within this routine.  Doing it that way makes it more
 *	  likely that we will reject an inferior path after a few comparisons,
 *	  rather than many comparisons.
 *
 *	  NOTE: discarded Path objects are immediately pfree'd to reduce planner
 *	  memory consumption.  We dare not try to free the substructure of a Path,
 *	  since much of it may be shared with other Paths or the query tree itself;
 *	  but just recycling discarded Path nodes is a very useful savings in
 *	  a large join tree.  We can recycle the List nodes of pathlist, too.
 *
 *	  BUT: we do not pfree IndexPath objects, since they may be referenced as
 *	  children of BitmapHeapPaths as well as being paths in their own right.
 *
 * 'parent_rel' is the relation entry to which the path corresponds.
 * 'new_path' is a potential path for parent_rel.
 *
 * Returns nothing, but modifies parent_rel->pathlist.
 */
void add_path(PlannerInfo *root, RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;		/* unless we find a superior old path */
	ListCell   *insert_after = NULL;	/* where to insert new item */
	ListCell   *p1_prev = NULL;
	ListCell   *p1;

    if (!new_path)
        return;

    /* CDB: Might need to remove dups caused by flattened subqueries. */
    if (root->ndedup > 0)
    {
        new_path = (Path *)create_unique_rowid_path(root, new_path, root->dedup_relids);
        if (!new_path)
            return;
    }

    /*
	 * Loop to check proposed new path against old paths.  Note it is possible
	 * for more than one old path to be tossed out because new_path dominates
	 * it.
	 */
	p1 = list_head(parent_rel->pathlist);		/* cannot use foreach here */
	while (p1 != NULL)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* unless new proves superior */
		int			costcmp;

		/*
		 * As of Postgres 8.0, we use fuzzy cost comparison to avoid wasting
		 * cycles keeping paths that are really not significantly different in
		 * cost.
		 */
		costcmp = compare_fuzzy_path_costs(new_path, old_path, TOTAL_COST);

		/*
		 * If the two paths compare differently for startup and total cost,
		 * then we want to keep both, and we can skip the (much slower)
		 * comparison of pathkeys.	If they compare the same, proceed with the
		 * pathkeys comparison.  Note: this test relies on the fact that
		 * compare_fuzzy_path_costs will only return 0 if both costs are
		 * effectively equal (and, therefore, there's no need to call it twice
		 * in that case).
		 */
		if (costcmp == 0 ||
			costcmp == compare_fuzzy_path_costs(new_path, old_path,
												STARTUP_COST))
		{
			switch (compare_pathkeys(new_path->pathkeys, old_path->pathkeys))
			{
				case PATHKEYS_EQUAL:
					if (costcmp < 0)
						remove_old = true;		/* new dominates old */
					else if (costcmp > 0)
						accept_new = false;		/* old dominates new */
					else
					{
						/*
						 * Same pathkeys, and fuzzily the same cost, so keep
						 * just one --- but we'll do an exact cost comparison
						 * to decide which.
						 */
						if (compare_path_costs(new_path, old_path,
											   TOTAL_COST) < 0)
							remove_old = true;	/* new dominates old */
						else
							accept_new = false; /* old equals or dominates new */
					}
					break;
				case PATHKEYS_BETTER1:
					if (costcmp <= 0)
						remove_old = true;		/* new dominates old */
					break;
				case PATHKEYS_BETTER2:
					if (costcmp >= 0)
						accept_new = false;		/* old dominates new */
					break;
				case PATHKEYS_DIFFERENT:
					/* keep both paths, since they have different ordering */
					break;
			}
		}

		/*
		 * Remove current element from pathlist if dominated by new.
		 */
		if (remove_old)
		{
			parent_rel->pathlist = list_delete_cell(parent_rel->pathlist,
													p1, p1_prev);

			/*
			 * Delete the data pointed-to by the deleted cell, if possible
			 */
			if (!IsA(old_path, IndexPath))
				pfree(old_path);
			/* Advance list pointer */
			if (p1_prev)
				p1 = lnext(p1_prev);
			else
				p1 = list_head(parent_rel->pathlist);
		}
		else
		{
			/* new belongs after this old path if it has cost >= old's */
			if (costcmp >= 0)
				insert_after = p1;
			/* Advance list pointers */
			p1_prev = p1;
			p1 = lnext(p1);
		}

		/*
		 * If we found an old path that dominates new_path, we can quit
		 * scanning the pathlist; we will not add new_path, and we assume
		 * new_path cannot dominate any other elements of the pathlist.
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/* Accept the new path: insert it at proper place in pathlist */
		if (insert_after)
			lappend_cell(parent_rel->pathlist, insert_after, new_path);
		else
			parent_rel->pathlist = lcons(new_path, parent_rel->pathlist);
	}
	else
	{
		/* Reject and recycle the new path */
		if (!IsA(new_path, IndexPath))
			pfree(new_path);
	}
}


/*****************************************************************************
 *		PATH NODE CREATION ROUTINES
 *****************************************************************************/

/*
 * create_seqscan_path
 *	  Creates a path corresponding to a sequential scan, returning the
 *	  pathnode.
 */
Path *
create_seqscan_path(PlannerInfo *root, RelOptInfo *rel)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->pathkeys = NIL;	/* seqscan has unordered result */

    pathnode->locus = cdbpathlocus_from_baserel(root, rel);
    pathnode->motionHazard = false;
	pathnode->rescannable = true;

	cost_seqscan(pathnode, root, rel);

	return pathnode;
}

/*
 * create_index_path
 *	  Creates a path node for an index scan.
 *
 * 'index' is a usable index.
 * 'clause_groups' is a list of lists of RestrictInfo nodes
 *			to be used as index qual conditions in the scan.
 * 'pathkeys' describes the ordering of the path.
 * 'indexscandir' is ForwardScanDirection or BackwardScanDirection
 *			for an ordered index, or NoMovementScanDirection for
 *			an unordered index.
 * 'isjoininner' is TRUE if this is a join inner indexscan path.
 *			(pathkeys and indexscandir are ignored if so.)
 *
 * Returns the new path node.
 */
IndexPath *
create_index_path(PlannerInfo *root,
				  IndexOptInfo *index,
				  List *clause_groups,
				  List *pathkeys,
				  ScanDirection indexscandir,
				  bool isjoininner)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	RelOptInfo *rel = index->rel;
	List	   *indexquals,
			   *allclauses;

	/*
	 * For a join inner scan, there's no point in marking the path with any
	 * pathkeys, since it will only ever be used as the inner path of a
	 * nestloop, and so its ordering does not matter.  For the same reason we
	 * don't really care what order it's scanned in.  (We could expect the
	 * caller to supply the correct values, but it's easier to force it here.)
	 */
	if (isjoininner)
	{
		pathkeys = NIL;
		indexscandir = NoMovementScanDirection;
	}

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = pathkeys;

	/* Convert clauses to indexquals the executor can handle */
	indexquals = expand_indexqual_conditions(index, clause_groups);

	/* Flatten the clause-groups list to produce indexclauses list */
	allclauses = flatten_clausegroups_list(clause_groups);

	/* Fill in the pathnode */
	pathnode->indexinfo = index;
	pathnode->indexclauses = allclauses;
	pathnode->indexquals = indexquals;

	pathnode->isjoininner = isjoininner;
	pathnode->indexscandir = indexscandir;

	if (isjoininner)
	{
		/*
		 * We must compute the estimated number of output rows for the
		 * indexscan.  This is less than rel->rows because of the additional
		 * selectivity of the join clauses.  Since clause_groups may contain
		 * both restriction and join clauses, we have to do a set union to get
		 * the full set of clauses that must be considered to compute the
		 * correct selectivity.  (Without the union operation, we might have
		 * some restriction clauses appearing twice, which'd mislead
		 * clauselist_selectivity into double-counting their selectivity.
		 * However, since RestrictInfo nodes aren't copied when linking them
		 * into different lists, it should be sufficient to use pointer
		 * comparison to remove duplicates.)
		 *
		 * Always assume the join type is JOIN_INNER; even if some of the join
		 * clauses come from other contexts, that's not our problem.
		 */
		allclauses = list_union_ptr(rel->baserestrictinfo, allclauses);
		pathnode->rows = rel->tuples *
			clauselist_selectivity(root,
								   allclauses,
								   rel->relid,	/* do not use 0! */
								   JOIN_INNER);
		/* Like costsize.c, force estimate to be at least one row */
		pathnode->rows = clamp_row_est(pathnode->rows);
	}
	else
	{
		/*
		 * The number of rows is the same as the parent rel's estimate, since
		 * this isn't a join inner indexscan.
		 */
		pathnode->rows = rel->rows;
	}

	/* Distribution is same as the base table. */
	pathnode->path.locus = cdbpathlocus_from_baserel(root, rel);
	pathnode->path.motionHazard = false;
	pathnode->path.rescannable = true;

	cost_index(pathnode, root, index, indexquals, isjoininner);

	return pathnode;
}

/*
 * create_bitmap_heap_path
 *	  Creates a path node for a bitmap scan.
 *
 * 'bitmapqual' is a tree of IndexPath, BitmapAndPath, and BitmapOrPath nodes.
 */
BitmapHeapPath *
create_bitmap_heap_path(PlannerInfo *root,
						RelOptInfo *rel,
						Path *bitmapqual,
						bool isjoininner)
{
	BitmapHeapPath *pathnode = makeNode(BitmapHeapPath);

	pathnode->path.pathtype = T_BitmapHeapScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;		/* always unordered */

    /* Distribution is same as the base table. */
    pathnode->path.locus = cdbpathlocus_from_baserel(root, rel);
    pathnode->path.motionHazard = false;
    pathnode->path.rescannable = true;

	pathnode->bitmapqual = bitmapqual;
	pathnode->isjoininner = isjoininner;

	if (isjoininner)
	{
		/*
		 * We must compute the estimated number of output rows for the
		 * indexscan.  This is less than rel->rows because of the additional
		 * selectivity of the join clauses.  We make use of the selectivity
		 * estimated for the bitmap to do this; this isn't really quite right
		 * since there may be restriction conditions not included in the
		 * bitmap ...
		 */
		Cost		indexTotalCost;
		Selectivity indexSelectivity;

		cost_bitmap_tree_node(bitmapqual, &indexTotalCost, &indexSelectivity);
		pathnode->rows = rel->tuples * indexSelectivity;
		if (pathnode->rows > rel->rows)
			pathnode->rows = rel->rows;
		/* Like costsize.c, force estimate to be at least one row */
		pathnode->rows = clamp_row_est(pathnode->rows);
	}
	else
	{
		/*
		 * The number of rows is the same as the parent rel's estimate, since
		 * this isn't a join inner indexscan.
		 */
		pathnode->rows = rel->rows;
	}

	cost_bitmap_heap_scan(&pathnode->path, root, rel, bitmapqual, false);

	return pathnode;
}

/*
 * create_bitmap_and_path
 *	  Creates a path node representing a BitmapAnd.
 */
BitmapAndPath *
create_bitmap_and_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   List *bitmapquals)
{
	BitmapAndPath *pathnode = makeNode(BitmapAndPath);

	pathnode->path.pathtype = T_BitmapAnd;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_and_node(pathnode, root);

	return pathnode;
}

/*
 * create_bitmap_or_path
 *	  Creates a path node representing a BitmapOr.
 */
BitmapOrPath *
create_bitmap_or_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  List *bitmapquals)
{
	BitmapOrPath *pathnode = makeNode(BitmapOrPath);

	pathnode->path.pathtype = T_BitmapOr;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;		/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_or_node(pathnode, root);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  Creates a path corresponding to a tid_direct scan, returning the
 *	  pathnode.
 */
TidPath *
create_tidscan_path(PlannerInfo *root, RelOptInfo *rel, List *tideval)
{
	TidPath    *pathnode = makeNode(TidPath);

	pathnode->path.pathtype = T_TidScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;

	pathnode->tideval = tideval;

    /* Distribution is same as the base table. */
    pathnode->path.locus = cdbpathlocus_from_baserel(root, rel);
    pathnode->path.motionHazard = false;
    pathnode->path.rescannable = true;

	cost_tidscan(&pathnode->path, root, rel, tideval);

	/*
	 * divide selectivity for each clause to get an equal selectivity as
	 * IndexScan does OK ?
	 */

	return pathnode;
}

/*
 * create_append_path
 *	  Creates a path corresponding to an Append plan, returning the
 *	  pathnode.
 */
AppendPath *
create_append_path(RelOptInfo *rel, List *subpaths)
{
	AppendPath *pathnode = makeNode(AppendPath);
	ListCell   *l;

	pathnode->path.pathtype = T_Append;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;		/* result is always considered
										 * unsorted */
	pathnode->subpaths = subpaths;

    pathnode->path.motionHazard = false;
    pathnode->path.rescannable = true;

	pathnode->path.startup_cost = 0;
	pathnode->path.total_cost = 0;
	foreach(l, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		if (l == list_head(subpaths))	/* first node? */
			pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost += subpath->total_cost;

        /*
         * CDB: This function is called only when combining rel scans in an
         * inheritance tree.  We assume - and CREATE TABLE enforces - that
         * all tables in an inheritance tree are distributed alike; but
         * check a little just for luck.
         */
        if (l == list_head(subpaths))
            pathnode->path.locus = subpath->locus;
        else if (pathnode->path.locus.locustype != subpath->locus.locustype ||
                 CdbPathLocus_Degree(pathnode->path.locus) !=
                    CdbPathLocus_Degree(subpath->locus))
            ereport(ERROR, (errcode(ERRCODE_MPP_FEATURE_NOT_SUPPORTED),
                            errmsg_internal("Table inherits from parent with "
                                            "incompatible distribution") ));

        if (subpath->motionHazard)
            pathnode->path.motionHazard = true;
		
		if ( !subpath->rescannable )
			pathnode->path.rescannable = false;
	}

	return pathnode;
}

/*
 * create_result_path
 *	  Creates a path corresponding to a Result plan, returning the
 *	  pathnode.
 */
ResultPath *
create_result_path(RelOptInfo *rel, Path *subpath, List *constantqual)
{
	ResultPath *pathnode = makeNode(ResultPath);

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;	/* may be NULL */

	if (subpath)
		pathnode->path.pathkeys = subpath->pathkeys;
	else
		pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;
	pathnode->constantqual = constantqual;

	/* Ideally should define cost_result(), but I'm too lazy */
	if (subpath)
	{
		pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost = subpath->total_cost;

        pathnode->path.locus = subpath->locus;
        pathnode->path.motionHazard = subpath->motionHazard;
		pathnode->path.rescannable = subpath->rescannable;
	}
	else
	{
		pathnode->path.startup_cost = 0;
		pathnode->path.total_cost = cpu_tuple_cost;

        CdbPathLocus_MakeGeneral(&pathnode->path.locus);
        pathnode->path.motionHazard = false;
        pathnode->path.rescannable = true;
	}

	return pathnode;
}

/*
 * create_material_path
 *	  Creates a path corresponding to a Material plan, returning the
 *	  pathnode.
 */
MaterialPath *
create_material_path(RelOptInfo *rel, Path *subpath)
{
	MaterialPath *pathnode = makeNode(MaterialPath);

	pathnode->path.pathtype = T_Material;
	pathnode->path.parent = rel;

	pathnode->path.pathkeys = subpath->pathkeys;

    pathnode->path.locus = subpath->locus;
    pathnode->path.motionHazard = subpath->motionHazard;
    pathnode->cdb_strict = false;
    pathnode->path.rescannable = true; /* Independent of sub-path */

	pathnode->subpath = subpath;

	cost_material(&pathnode->path,
				  subpath->total_cost,
				  cdbpath_rows(subpath),
				  rel->width);

	return pathnode;
}


/*
 * create_unique_common_path
 *	  Creates a path representing elimination of distinct rows from the
 *	  input data.
 */
static UniquePath *
create_unique_common_path(PlannerInfo *root,
                          Path        *subpath,
                          List        *distinct_on_exprs,
                          Relids       dedup_relids)
{
	UniquePath *pathnode;
	Path		sort_path;		/* dummy for result of cost_sort */
	Path		agg_path;		/* dummy for result of cost_agg */
    MemoryContext oldcontext;
    RelOptInfo *rel = subpath->parent;
	int			numCols;
    double      subpath_rows;
    bool        hashable = false;

    subpath_rows = cdbpath_rows(subpath);

	/*
	 * We must ensure path struct is allocated in same context as parent rel;
	 * otherwise GEQO memory management causes trouble.  (Compare
	 * best_inner_indexscan().)
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	pathnode = makeNode(UniquePath);

	/* There is no substructure to allocate, so can switch back right away */
	MemoryContextSwitchTo(oldcontext);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;

    pathnode->path.locus = subpath->locus;
    pathnode->path.motionHazard = subpath->motionHazard;
    pathnode->path.rescannable = subpath->rescannable;

	/*
	 * Treat the output as always unsorted, since we don't necessarily have
	 * pathkeys to represent it.
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;

    pathnode->distinct_on_exprs = distinct_on_exprs;
    pathnode->dedup_relids = dedup_relids;  /* CDB: share caller's relids */
    pathnode->must_repartition = false;

	/*
	 * If the input is a subquery whose output must be unique already, then we
	 * don't need to do anything.  The test for uniqueness has to consider
	 * exactly which columns we are extracting; for example "SELECT DISTINCT
	 * x,y" doesn't guarantee that x alone is distinct. So we cannot check for
	 * this optimization unless we found our own targetlist above, and it
	 * consists only of simple Vars referencing subquery outputs.  (Possibly
	 * we could do something with expressions in the subquery outputs, too,
	 * but for now keep it simple.)
	 */
	if (distinct_on_exprs && rel->rtekind == RTE_SUBQUERY)
	{
		RangeTblEntry *rte = rt_fetch(rel->relid, root->parse->rtable);
		List	   *sub_tlist_colnos;

		sub_tlist_colnos = translate_sub_tlist(distinct_on_exprs, rel->relid);

		if (sub_tlist_colnos &&
			query_is_distinct_for(rte->subquery, sub_tlist_colnos))
		{
			pathnode->umethod = UNIQUE_PATH_NOOP;
			pathnode->rows = subpath_rows;
			pathnode->path.startup_cost = subpath->startup_cost;
			pathnode->path.total_cost = subpath->total_cost;
			pathnode->path.pathkeys = subpath->pathkeys;
			return pathnode;
		}
	}

	/*
	 * If we know the targetlist, try to estimate number of result rows;
	 * otherwise punt.
	 */
	if (distinct_on_exprs)
	{
		pathnode->rows = estimate_num_groups(root, distinct_on_exprs, subpath_rows);
		numCols = list_length(distinct_on_exprs);
	}
	else
	{
		pathnode->rows = subpath_rows;
		numCols = list_length(rel->reltargetlist);
	}

	/*
	 * Estimate cost for sort+unique implementation
	 */
	cost_sort(&sort_path, root, NIL,
			  subpath->total_cost,
			  subpath_rows,
			  rel->width);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.	(XXX probably this is
	 * an overestimate.)  This should agree with make_unique.
	 */
	sort_path.total_cost += cpu_operator_cost * subpath_rows * numCols;

	/*
	 * Is it safe to use a hashed implementation?  If so, estimate and compare
	 * costs.  We only try this if we know the targetlist for sure (else we
	 * can't be sure about the datatypes involved).
     *
     * CDB: For dedup, the distinct_on_exprs list isn't built until later,
     * but we know the data types will be hashable.
	 */
	pathnode->umethod = UNIQUE_PATH_SORT;
    if (distinct_on_exprs && hash_safe_tlist(distinct_on_exprs))
        hashable = true;
    if (dedup_relids)
        hashable = true;
	if (hashable && enable_hashagg)
	{
		/*
		 * Estimate the overhead per hashtable entry at 64 bytes (same as in
		 * planner.c).
		 */
		int			hashentrysize = rel->width + 64;

		if (hashentrysize * pathnode->rows <= work_mem * 1024L)
		{
			cost_agg(&agg_path, root,
					 AGG_HASHED, 0,
					 numCols, pathnode->rows,
					 subpath->startup_cost,
					 subpath->total_cost,
					 rel->rows);
			if (agg_path.total_cost < sort_path.total_cost)
				pathnode->umethod = UNIQUE_PATH_HASH;
		}
	}

	if (pathnode->umethod == UNIQUE_PATH_HASH)
	{
		pathnode->path.startup_cost = agg_path.startup_cost;
		pathnode->path.total_cost = agg_path.total_cost;
	}
	else
	{
		pathnode->path.startup_cost = sort_path.startup_cost;
		pathnode->path.total_cost = sort_path.total_cost;
	}

    /* Hash or sort implies materialization and breaks deadlock cycle.
     *  (NB: Must not reset motionHazard when sort is eliminated due to
     *  existing ordering; but Unique sort is never optimized away at present.)
     */
    pathnode->path.motionHazard = false;
	
	/* Same reasoning applies to rescanablilty.  If no actual sort is placed
	 * in the plan, then rescannable is set correctly to the subpath value.
	 * If hash or sort intervenes, it should be set to true.  We depend
	 * on the above claim that sort will always intervene.
	 */
    pathnode->path.rescannable = true;

	return pathnode;
}                               /* create_unique_common_path */


/*
 * create_unique_path
 *	  Creates a path representing elimination of distinct rows from the
 *	  input data.
 *
 * If used at all, this is likely to be called repeatedly on the same rel;
 * and the input subpath should always be the same (the cheapest_total path
 * for the rel).  So we cache the result.
 */
UniquePath *
create_unique_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath)
{
	UniquePath *pathnode;
	List	   *sub_targetlist;
	ListCell   *l;

	/* If result already cached, return it */
    if (subpath == rel->cheapest_total_path &&
        rel->cheapest_unique_path)
		return (UniquePath *) rel->cheapest_unique_path;

	/*
	 * Try to identify the targetlist that will actually be unique-ified. In
	 * current usage, this routine is only used for sub-selects of IN clauses,
	 * so we should be able to find the tlist in in_info_list.
	 */
	sub_targetlist = NIL;
	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		if (bms_equal(ininfo->righthand, rel->relids))
		{
			sub_targetlist = ininfo->sub_targetlist;
			break;
		}
	}

    /* Create the UniquePath node. */
    pathnode = create_unique_common_path(root, subpath, sub_targetlist, NULL);

    /* Cache it in case we need it again. */
    if (subpath == rel->cheapest_total_path)
    	rel->cheapest_unique_path = (Path *) pathnode;

	return pathnode;
}                               /* create_unique_path */


/*
 * create_unique_rowid_path
 *
 * CDB: Used when a subquery predicate (such as "x IN (SELECT ...)") has
 * been flattened into a join with the tables of the current query.  A row
 * of the cross-product of the current query's tables might join with more
 * than one row from the subquery and thus be duplicated.  Removing such
 * duplicates after the join is called deduping.
 *
 * If a row's duplicates might occur in more than one partition, a Motion
 * operator will be needed to bring them together.  Since this path might
 * not be chosen, we won't take the time to create a MotionPath node here;
 * just estimate what the cost would be, and assign a dummy locus.  Save
 * the real work for create_plan().
 */
UniquePath *
create_unique_rowid_path(PlannerInfo *root,
                         Path        *subpath,
                         Relids       dedup_relids)
{
	UniquePath *uniquepath;

    Assert(!bms_is_empty(dedup_relids));

    /* Create the UniquePath node. */
    uniquepath = create_unique_common_path(root, subpath, NIL, dedup_relids);

    /* Add repartitioning cost if duplicates might be on different QEs. */
    if (!CdbPathLocus_IsBottleneck(subpath->locus) &&
        !cdbpathlocus_is_hashed_on_relids(subpath->locus, dedup_relids))
    {
        CdbMotionPath   motionpath;     /* dummy for cost estimate */
        Cost            repartition_cost;

        /* Tell create_unique_plan() to insert Motion operator atop subpath. */
        uniquepath->must_repartition = true;

        /* Set a fake locus.  Repartitioning key won't be built until later. */
        CdbPathLocus_MakeStrewn(&uniquepath->path.locus);

        /* Estimate repartitioning cost. */
        memset(&motionpath, 0, sizeof(motionpath));
        motionpath.path.type = T_CdbMotionPath;
        motionpath.path.parent = subpath->parent;
        motionpath.path.locus = uniquepath->path.locus;
        cdbpath_cost_motion(&motionpath, subpath);

        /* Add MotionPath cost to UniquePath cost. */
        repartition_cost = motionpath.path.total_cost - subpath->total_cost;
        uniquepath->path.total_cost += repartition_cost;
    }

	return uniquepath;
}                               /* create_unique_rowid_path */


/*
 * translate_sub_tlist - get subquery column numbers represented by tlist
 *
 * The given targetlist should contain only Vars referencing the given relid.
 * Extract their varattnos (ie, the column numbers of the subquery) and return
 * as an integer List.
 *
 * If any of the tlist items is not a simple Var, we cannot determine whether
 * the subquery's uniqueness condition (if any) matches ours, so punt and
 * return NIL.
 */
static List *
translate_sub_tlist(List *tlist, int relid)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, tlist)
	{
		Var		   *var = (Var *) lfirst(l);

		if (!var || !IsA(var, Var) ||
			var->varno != relid)
			return NIL;			/* punt */

		result = lappend_int(result, var->varattno);
	}
	return result;
}

/*
 * query_is_distinct_for - does query never return duplicates of the
 *		specified columns?
 *
 * colnos is an integer list of output column numbers (resno's).  We are
 * interested in whether rows consisting of just these columns are certain
 * to be distinct.
 */
static bool
query_is_distinct_for(Query *query, List *colnos)
{
	ListCell   *l;

	/*
	 * DISTINCT (including DISTINCT ON) guarantees uniqueness if all the
	 * columns in the DISTINCT clause appear in colnos.
	 */
	if (query->distinctClause)
	{
		foreach(l, query->distinctClause)
		{
			SortClause *scl = (SortClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(scl,
													   query->targetList);

			if (!list_member_int(colnos, tle->resno))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}

	/*
	 * Similarly, GROUP BY guarantees uniqueness if all the grouped columns
	 * appear in colnos.
	 */
	if (query->groupClause)
	{
		foreach(l, query->groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(grpcl,
													   query->targetList);

			if (!list_member_int(colnos, tle->resno))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}
	else
	{
		/*
		 * If we have no GROUP BY, but do have aggregates or HAVING, then the
		 * result is at most one row so it's surely unique.
		 */
		if (query->hasAggs || query->havingQual)
			return true;
	}

	/*
	 * UNION, INTERSECT, EXCEPT guarantee uniqueness of the whole output row,
	 * except with ALL
	 */
	if (query->setOperations)
	{
		SetOperationStmt *topop = (SetOperationStmt *) query->setOperations;

		Assert(IsA(topop, SetOperationStmt));
		Assert(topop->op != SETOP_NONE);

		if (!topop->all)
		{
			/* We're good if all the nonjunk output columns are in colnos */
			foreach(l, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);

				if (!tle->resjunk &&
					!list_member_int(colnos, tle->resno))
					break;		/* exit early if no match */
			}
			if (l == NULL)		/* had matches for all? */
				return true;
		}
	}

	/*
	 * XXX Are there any other cases in which we can easily see the result
	 * must be distinct?
	 */

	return false;
}

/*
 * hash_safe_tlist - can datatypes of given tlist be hashed?
 *
 * We assume hashed aggregation will work if the datatype's equality operator
 * is marked hashjoinable.
 *
 * XXX this probably should be somewhere else.	See also hash_safe_grouping
 * in plan/planner.c.
 */
static bool
hash_safe_tlist(List *tlist)
{
	ListCell   *tl;

	foreach(tl, tlist)
	{
		Node	   *expr = (Node *) lfirst(tl);
		Operator	optup;
		bool		oprcanhash;

		optup = equality_oper(exprType(expr), true);
		if (!optup)
			return false;
		oprcanhash = ((Form_pg_operator) GETSTRUCT(optup))->oprcanhash;
		ReleaseSysCache(optup);
		if (!oprcanhash)
			return false;
	}
	return true;
}

/*
 * create_subqueryscan_path
 *	  Creates a path corresponding to a sequential scan of a subquery,
 *	  returning the pathnode.
 */
Path *
create_subqueryscan_path(PlannerInfo *root, RelOptInfo *rel, List *pathkeys)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SubqueryScan;
	pathnode->parent = rel;
	pathnode->pathkeys = pathkeys;

    pathnode->locus = cdbpathlocus_from_subquery(root, rel);
    pathnode->motionHazard = true;          /* better safe than sorry */
    pathnode->rescannable = false;

	cost_subqueryscan(pathnode, rel);

	return pathnode;
}

/*
 * create_functionscan_path
 *	  Creates a path corresponding to a sequential scan of a function,
 *	  returning the pathnode.
 */
Path *
create_functionscan_path(PlannerInfo *root, RelOptInfo *rel)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_FunctionScan;
	pathnode->parent = rel;
	pathnode->pathkeys = NIL;	/* for now, assume unordered result */

    /* MPP TODO: handle function scan */
    /* Assign all function evaluations to the query dispatcher, for now. */
    CdbPathLocus_MakeRoot(&pathnode->locus);
    pathnode->motionHazard = false;
	
	/* For now, be conservative. */
	pathnode->rescannable = false;

	cost_functionscan(pathnode, root, rel);

	return pathnode;
}

/*
 * create_nestloop_path
 *	  Creates a pathnode corresponding to a nestloop join between two
 *	  relations.
 *
 * 'joinrel' is the join relation.
 * 'jointype' is the type of join required
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 *
 * Returns the resulting path node.
 */
NestPath *
create_nestloop_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
                     List *mergeclause_list,    /*CDB*/
					 List *pathkeys)
{
	NestPath       *pathnode;
    CdbPathLocus    join_locus;
    Path           *unmoved_inner_path = inner_path;
    bool            inner_must_be_local = false;

    /* CDB: Inner indexpath must execute in the same backend as the
     * nested join to receive input values from the outer rel.
     */
    if (IsA(inner_path, IndexPath) &&
        ((IndexPath *)inner_path)->isjoininner)
        inner_must_be_local = true;
    else if (IsA(inner_path, BitmapHeapPath) &&
             ((BitmapHeapPath *)inner_path)->isjoininner)
        inner_must_be_local = true;

    /* Add motion nodes above subpaths and decide where to join. */
    join_locus = cdbpath_motion_for_join(root,
                                         jointype,
                                         &outer_path,       /* INOUT */
                                         &inner_path,       /* INOUT */
                                         mergeclause_list,
                                         pathkeys,
                                         NIL,
                                         false,
                                         inner_must_be_local);
    if (CdbPathLocus_IsNull(join_locus))
        return NULL;

    /* Outer might not be ordered anymore after motion. */
    if (!outer_path->pathkeys)
        pathkeys = NULL;

    /* Materialize inner rel after motion, so NJ can loop over results.
     * Skip if an intervening Unique operator will materialize.
     * CDB TODO: Can skip if outer has at most one row.
     */
    if ( (inner_path != unmoved_inner_path && inner_path->motionHazard)
       || !inner_path->rescannable )
        inner_path = (Path *)create_material_path(inner_path->parent, inner_path);

    pathnode = makeNode(NestPath);
	pathnode->path.pathtype = T_NestLoop;
	pathnode->path.parent = joinrel;
	pathnode->jointype = jointype;
	pathnode->outerjoinpath = outer_path;
	pathnode->innerjoinpath = inner_path;
	pathnode->joinrestrictinfo = restrict_clauses;
	pathnode->path.pathkeys = pathkeys;

    pathnode->path.locus = join_locus;
    pathnode->path.motionHazard = outer_path->motionHazard || inner_path->motionHazard;
    pathnode->path.rescannable = outer_path->rescannable;

	cost_nestloop(pathnode, root);

	return pathnode;
}

/*
 * create_mergejoin_path
 *	  Creates a pathnode corresponding to a mergejoin join between
 *	  two relations
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 * 'mergeclauses' are the RestrictInfo nodes to use as merge clauses
 *		(this should be a subset of the restrict_clauses list)
 * 'allmergeclauses' are the RestrictInfo nodes that are of the form
 *      required of merge clauses (equijoin between outer and inner rel).
 *      Consists of the ones to be used for merging ('mergeclauses') plus
 *      any others in 'restrict_clauses' that are to be applied after the
 *      merge.  We use them for motion planning.  (CDB)
 * 'outersortkeys' are the sort varkeys for the outer relation
 *      or NIL to use existing ordering
 * 'innersortkeys' are the sort varkeys for the inner relation
 *      or NIL to use existing ordering
 */
MergePath *
create_mergejoin_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  List *mergeclauses,
                      List *allmergeclauses,    /*CDB*/
					  List *outersortkeys,
					  List *innersortkeys)
{
    MergePath      *pathnode;
    CdbPathLocus    join_locus;
    List           *outermotionkeys;
    List           *innermotionkeys;

    /*
     * Do subpaths have useful ordering?
     */
    if (outersortkeys == NIL)           /* must preserve existing ordering */
        outermotionkeys = outer_path->pathkeys;
    else if (pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
        outermotionkeys = outersortkeys;/* lucky coincidence, already ordered */
    else                                /* existing order useless; must sort */
        outermotionkeys = NIL;

    if (innersortkeys == NIL)
        innermotionkeys = inner_path->pathkeys;
    else if (pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
        innermotionkeys = innersortkeys;
    else
        innermotionkeys = NIL;

    /*
     * Add motion nodes above subpaths and decide where to join.
     */
    join_locus = cdbpath_motion_for_join(root,
                                         jointype,
                                         &outer_path,       /* INOUT */
                                         &inner_path,       /* INOUT */
                                         allmergeclauses,
                                         outermotionkeys,
                                         innermotionkeys,
                                         outersortkeys == NIL,
                                         innersortkeys == NIL);
    if (CdbPathLocus_IsNull(join_locus))
        return NULL;

	/*
	 * Sort is not needed if subpath is already well enough ordered and a
     * disordering motion node (with pathkeys == NIL) hasn't been added.
	 */
	if (outermotionkeys &&
		outer_path->pathkeys)
		outersortkeys = NIL;
	if (innermotionkeys &&
		inner_path->pathkeys)
		innersortkeys = NIL;

    pathnode = makeNode(MergePath);

	/*
	 * If we are not sorting the inner path, we may need a materialize node to
	 * ensure it can be marked/restored.  (Sort does support mark/restore, so
	 * no materialize is needed in that case.)
	 *
	 * Since the inner side must be ordered, and only Sorts and IndexScans can
	 * create order to begin with, you might think there's no problem --- but
	 * you'd be wrong.  Nestloop and merge joins can *preserve* the order of
	 * their inputs, so they can be selected as the input of a mergejoin, and
	 * they don't support mark/restore at present.
	 */
	if (innersortkeys == NIL &&
		!ExecSupportsMarkRestore(inner_path->pathtype))
		inner_path = (Path *)
			create_material_path(inner_path->parent, inner_path);

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->jpath.path.pathkeys = pathkeys;
    pathnode->jpath.path.locus = join_locus;
    pathnode->jpath.path.motionHazard = outer_path->motionHazard || inner_path->motionHazard;
    pathnode->jpath.path.rescannable = outer_path->rescannable && inner_path->rescannable;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;

	cost_mergejoin(pathnode, root);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'outer_path' is the cheapest outer path
 * 'inner_path' is the cheapest inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'hashclauses' are the RestrictInfo nodes to use as hash clauses
 *		(this should be a subset of the restrict_clauses list)
 */
HashPath *
create_hashjoin_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
                     List *mergeclause_list,    /*CDB*/
					 List *hashclauses)
{
    HashPath       *pathnode;
    CdbPathLocus    join_locus;

    /* Add motion nodes above subpaths and decide where to join. */
    join_locus = cdbpath_motion_for_join(root,
                                         jointype,
                                         &outer_path,       /* INOUT */
                                         &inner_path,       /* INOUT */
                                         mergeclause_list,
                                         NIL,   /* don't care about ordering */
                                         NIL,
                                         false,
                                         false);
    if (CdbPathLocus_IsNull(join_locus))
        return NULL;

	pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	/* A hashjoin never has pathkeys, since its ordering is unpredictable */
	pathnode->jpath.path.pathkeys = NIL;
    pathnode->jpath.path.locus = join_locus;
    pathnode->jpath.path.motionHazard = false;  /* outer + inner are materialized */
	pathnode->path_hashclauses = hashclauses;

    /*
     * If hash table overflows to disk, and an ancestor node requests rescan
     * (e.g. because the HJ is in the inner subtree of a NJ), then the HJ has
     * to be redone, including rescanning the inner rel in order to rebuild
     * the hash table.
     */
    pathnode->jpath.path.rescannable = outer_path->rescannable && inner_path->rescannable;

    cost_hashjoin(pathnode, root);

	return pathnode;
}


/*
 * pathnode_walk_node
 *    Calls a 'walker' function for the given Path node; or returns
 *    CdbVisit_Walk if 'path' is NULL.
 *
 *    If 'walker' returns CdbVisit_Walk, then this function calls
 *    pathnode_walk_kids() to visit the node's children, and returns
 *    the result.
 *
 *    If 'walker' returns CdbVisit_Skip, then this function immediately
 *    returns CdbVisit_Walk and does not visit the node's children.
 *
 *    If 'walker' returns CdbVisit_Stop or another value, then this function
 *    immediately returns that value and does not visit the node's children.
 *
 * pathnode_walk_list
 *    Calls pathnode_walk_node() for each Path node in the given List.
 *
 *    Quits if the result of pathnode_walk_node() is CdbVisit_Stop or another
 *    value other than CdbVisit_Walk, and returns that result without visiting
 *    any more nodes.
 *
 *    Returns CdbVisit_Walk if all of the subtrees return CdbVisit_Walk, or
 *    if the list is empty.
 *
 *    Note that this function never returns CdbVisit_Skip to its caller.
 *    Only the 'walker' can return CdbVisit_Skip.
 *
 * pathnode_walk_kids
 *    Calls pathnode_walk_node() for each child of the given Path node.
 *
 *    Quits if the result of pathnode_walk_node() is CdbVisit_Stop or another
 *    value other than CdbVisit_Walk, and returns that result without visiting
 *    any more nodes.
 *
 *    Returns CdbVisit_Walk if all of the children return CdbVisit_Walk, or
 *    if there are no children.
 *
 *    Note that this function never returns CdbVisit_Skip to its caller.
 *    Only the 'walker' can return CdbVisit_Skip.
 */
inline CdbVisitOpt
pathnode_walk_node(Path            *path,
			       CdbVisitOpt    (*walker)(Path *path, void *context),
			       void            *context)
{
    CdbVisitOpt     whatnext;

    if (path == NULL)
        whatnext = CdbVisit_Walk;
    else
    {
        whatnext = walker(path, context);
        if (whatnext == CdbVisit_Walk)
            whatnext = pathnode_walk_kids(path, walker, context);
        else if (whatnext == CdbVisit_Skip)
            whatnext = CdbVisit_Walk;
    }
    Assert(whatnext != CdbVisit_Skip);
    return whatnext;
}	                            /* pathnode_walk_node */

CdbVisitOpt
pathnode_walk_list(List            *pathlist,
			       CdbVisitOpt    (*walker)(Path *path, void *context),
			       void            *context)
{
    ListCell       *cell;
    Path           *path;
    CdbVisitOpt     v = CdbVisit_Walk;

	foreach(cell, pathlist)
	{
        path = (Path *)lfirst(cell);
        v = pathnode_walk_node(path, walker, context);
        if (v != CdbVisit_Walk) /* stop */
            break;
	}
	return v;
}	                            /* pathnode_walk_list */

CdbVisitOpt
pathnode_walk_kids(Path            *path,
			       CdbVisitOpt    (*walker)(Path *path, void *context),
			       void            *context)
{
    CdbVisitOpt v;

    Assert(path != NULL);

    switch (path->pathtype)
	{
	    case T_SeqScan:
	    case T_IndexScan:
	    case T_TidScan:
	    case T_SubqueryScan:
	    case T_FunctionScan:
            return CdbVisit_Walk;
	    case T_BitmapHeapScan:
	        v = pathnode_walk_node(((BitmapHeapPath *)path)->bitmapqual, walker, context);
            break;
        case T_BitmapAnd:
            v = pathnode_walk_list(((BitmapAndPath *)path)->bitmapquals, walker, context);
            break;
        case T_BitmapOr:
            v = pathnode_walk_list(((BitmapOrPath *)path)->bitmapquals, walker, context);
            break;
        case T_HashJoin:
	    case T_MergeJoin:
	    case T_NestLoop:
            v = pathnode_walk_node(((JoinPath *)path)->outerjoinpath, walker, context);
            if (v != CdbVisit_Walk)     /* stop */
                break;
            v = pathnode_walk_node(((JoinPath *)path)->innerjoinpath, walker, context);
            break;
        case T_Append:
            v = pathnode_walk_list(((AppendPath *)path)->subpaths, walker, context);
            break;
	    case T_Result:
	        v = pathnode_walk_node(((ResultPath *)path)->subpath, walker, context);
            break;
	    case T_Material:
	        v = pathnode_walk_node(((MaterialPath *)path)->subpath, walker, context);
            break;
	    case T_Unique:
	        v = pathnode_walk_node(((UniquePath *)path)->subpath, walker, context);
            break;
        case T_Motion:
	        v = pathnode_walk_node(((CdbMotionPath *)path)->subpath, walker, context);
            break;
        default:
            v = CdbVisit_Walk;  /* keep compiler quiet */
		    elog(ERROR, "unrecognized path type: %d", (int)path->pathtype);
	}
    return v;
}	                            /* pathnode_walk_kids */
