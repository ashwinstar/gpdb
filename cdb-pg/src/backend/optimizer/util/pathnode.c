/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Portions Copyright (c) 2005, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/pathnode.c,v 1.111 2004/12/31 22:00:23 pgsql Exp $
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

static bool is_distinct_query(Query *query);
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
		 * If paths have the same startup cost (not at all unlikely),
		 * order them by total cost.
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
	Cost		fuzz;

	/*
	 * The fuzz factor is set at one percent of the smaller total_cost,
	 * but not less than 0.01 cost units (just in case total cost is
	 * zero).
	 *
	 * XXX does this percentage need to be user-configurable?
	 */
	fuzz = Min(path1->total_cost, path2->total_cost) * 0.01;
	fuzz = Max(fuzz, 0.01);

	if (criterion == STARTUP_COST)
	{
		if (Abs(path1->startup_cost - path2->startup_cost) > fuzz)
		{
			if (path1->startup_cost < path2->startup_cost)
				return -1;
			else
				return +1;
		}

		/*
		 * If paths have the same startup cost (not at all unlikely),
		 * order them by total cost.
		 */
		if (Abs(path1->total_cost - path2->total_cost) > fuzz)
		{
			if (path1->total_cost < path2->total_cost)
				return -1;
			else
				return +1;
		}
	}
	else
	{
		if (Abs(path1->total_cost - path2->total_cost) > fuzz)
		{
			if (path1->total_cost < path2->total_cost)
				return -1;
			else
				return +1;
		}

		/*
		 * If paths have the same total cost, order them by startup cost.
		 */
		if (Abs(path1->startup_cost - path2->startup_cost) > fuzz)
		{
			if (path1->startup_cost < path2->startup_cost)
				return -1;
			else
				return +1;
		}
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
 * 'parent_rel' is the relation entry to which the path corresponds.
 * 'new_path' is a potential path for parent_rel.
 *
 * Returns nothing, but modifies parent_rel->pathlist.
 */
void
add_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;		/* unless we find a superior old
										 * path */
	ListCell   *insert_after = NULL;	/* where to insert new item */
	ListCell   *p1_prev = NULL;
	ListCell   *p1;

    if (!new_path)
        return;

    /*
	 * Loop to check proposed new path against old paths.  Note it is
	 * possible for more than one old path to be tossed out because
	 * new_path dominates it.
	 */
	p1 = list_head(parent_rel->pathlist);		/* cannot use foreach here */
	while (p1 != NULL)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* unless new proves superior */
		int			costcmp;

		/*
		 * As of Postgres 8.0, we use fuzzy cost comparison to avoid
		 * wasting cycles keeping paths that are really not significantly
		 * different in cost.
		 */
		costcmp = compare_fuzzy_path_costs(new_path, old_path, TOTAL_COST);

		/*
		 * If the two paths compare differently for startup and total
		 * cost, then we want to keep both, and we can skip the (much
		 * slower) comparison of pathkeys.	If they compare the same,
		 * proceed with the pathkeys comparison.  Note: this test relies
		 * on the fact that compare_fuzzy_path_costs will only return 0 if
		 * both costs are effectively equal (and, therefore, there's no
		 * need to call it twice in that case).
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
						 * Same pathkeys, and fuzzily the same cost, so
						 * keep just one --- but we'll do an exact cost
						 * comparison to decide which.
						 */
						if (compare_path_costs(new_path, old_path,
											   TOTAL_COST) < 0)
							remove_old = true;	/* new dominates old */
						else
							accept_new = false; /* old equals or dominates
												 * new */
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
			/* Delete the data pointed-to by the deleted cell */
			pfree(old_path);
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
create_seqscan_path(Query *root, RelOptInfo *rel)
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
 * 'rel' is the parent rel
 * 'index' is an index on 'rel'
 * 'restriction_clauses' is a list of lists of RestrictInfo nodes
 *			to be used as index qual conditions in the scan.
 * 'pathkeys' describes the ordering of the path.
 * 'indexscandir' is ForwardScanDirection or BackwardScanDirection
 *			for an ordered index, or NoMovementScanDirection for
 *			an unordered index.
 *
 * Returns the new path node.
 */
IndexPath *
create_index_path(Query *root,
				  RelOptInfo *rel,
				  IndexOptInfo *index,
				  List *restriction_clauses,
				  List *pathkeys,
				  ScanDirection indexscandir)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	List	   *indexquals;

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = pathkeys;

    /* Convert clauses to indexquals the executor can handle */
	indexquals = expand_indexqual_conditions(index, restriction_clauses);

	/* Flatten the clause-groups list to produce indexclauses list */
	restriction_clauses = flatten_clausegroups_list(restriction_clauses);

	/*
	 * We are making a pathnode for a single-scan indexscan; therefore,
	 * indexinfo etc should be single-element lists.
	 */
	pathnode->indexinfo = list_make1(index);
	pathnode->indexclauses = list_make1(restriction_clauses);
	pathnode->indexquals = list_make1(indexquals);

	/* It's not an innerjoin path. */
	pathnode->isjoininner = false;

	pathnode->indexscandir = indexscandir;

	/*
	 * The number of rows is the same as the parent rel's estimate, since
	 * this isn't a join inner indexscan.
	 */
	pathnode->rows = rel->rows;

    /* Distribution is same as the base table. */
    pathnode->path.locus = cdbpathlocus_from_baserel(root, rel);
    pathnode->path.motionHazard = false;
    pathnode->path.rescannable = true;

	cost_index(&pathnode->path, root, rel, index, indexquals, false);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  Creates a path corresponding to a tid_direct scan, returning the
 *	  pathnode.
 */
TidPath *
create_tidscan_path(Query *root, RelOptInfo *rel, List *tideval)
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
         * CDB: At present this function is called only when combining rel
         * scans in an inheritance tree.  Allow inheritance on root only.
         */
        if (l == list_head(subpaths))
            pathnode->path.locus = subpath->locus;
        if (!CdbPathLocus_IsRoot(subpath->locus))
            ereport(ERROR, (errcode(ERRCODE_CDB_FEATURE_NOT_SUPPORTED),
                            errmsg("Distributed tables cannot use inheritance")));

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
 * create_unique_path
 *	  Creates a path representing elimination of distinct rows from the
 *	  input data.
 *
 * If used at all, this is likely to be called repeatedly on the same rel;
 * and the input subpath should always be the same (the cheapest_total path
 * for the rel).  So we cache the result.
 */
UniquePath *
create_unique_path(Query *root, RelOptInfo *rel, Path *subpath)
{
	UniquePath *pathnode;
	Path		sort_path;		/* dummy for result of cost_sort */
	Path		agg_path;		/* dummy for result of cost_agg */
	MemoryContext oldcontext;
	List	   *sub_targetlist;
	ListCell   *l;
	int			numCols;
    double      subpath_rows;

	/* If result already cached, return it */
    if (subpath == rel->cheapest_total_path &&
        rel->cheapest_unique_path)
        return (UniquePath *) rel->cheapest_unique_path;

    subpath_rows = cdbpath_rows(subpath);

	/*
	 * We must ensure path struct is allocated in same context as parent
	 * rel; otherwise GEQO memory management causes trouble.  (Compare
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
	 * Treat the output as always unsorted, since we don't necessarily
	 * have pathkeys to represent it.
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;

	/*
	 * If the input is a subquery whose output must be unique already, we
	 * don't need to do anything.
	 */
	if (rel->rtekind == RTE_SUBQUERY)
	{
		RangeTblEntry *rte = rt_fetch(rel->relid, root->rtable);

		if (is_distinct_query(rte->subquery))
		{
			pathnode->umethod = UNIQUE_PATH_NOOP;
			pathnode->rows = subpath_rows;
			pathnode->path.startup_cost = subpath->startup_cost;
			pathnode->path.total_cost = subpath->total_cost;
			pathnode->path.pathkeys = subpath->pathkeys;

            if (subpath == rel->cheapest_total_path)
    			rel->cheapest_unique_path = (Path *) pathnode;

			return pathnode;
		}
	}

	/*
	 * Try to identify the targetlist that will actually be unique-ified.
	 * In current usage, this routine is only used for sub-selects of IN
	 * clauses, so we should be able to find the tlist in in_info_list.
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

	/*
	 * If we know the targetlist, try to estimate number of result rows;
	 * otherwise punt.
	 */
	if (sub_targetlist)
	{
		pathnode->rows = estimate_num_groups(root, sub_targetlist, subpath_rows);
		numCols = list_length(sub_targetlist);
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
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.  (XXX
	 * probably this is an overestimate.)  This should agree with
	 * make_unique.
	 */
	sort_path.total_cost += cpu_operator_cost * subpath_rows * numCols;

	/*
	 * Is it safe to use a hashed implementation?  If so, estimate and
	 * compare costs.  We only try this if we know the targetlist for sure
	 * (else we can't be sure about the datatypes involved).
	 */
	pathnode->umethod = UNIQUE_PATH_SORT;
	if (enable_hashagg && sub_targetlist && hash_safe_tlist(sub_targetlist))
	{
		/*
		 * Estimate the overhead per hashtable entry at 64 bytes (same as
		 * in planner.c).
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

    if (subpath == rel->cheapest_total_path)
    	rel->cheapest_unique_path = (Path *) pathnode;

	return pathnode;
}

/*
 * is_distinct_query - does query never return duplicate rows?
 */
static bool
is_distinct_query(Query *query)
{
	/* DISTINCT (but not DISTINCT ON) guarantees uniqueness */
	if (has_distinct_clause(query))
		return true;

	/* UNION, INTERSECT, EXCEPT guarantee uniqueness, except with ALL */
	if (query->setOperations)
	{
		SetOperationStmt *topop = (SetOperationStmt *) query->setOperations;

		Assert(IsA(topop, SetOperationStmt));
		Assert(topop->op != SETOP_NONE);

		if (!topop->all)
			return true;
	}

	/*
	 * GROUP BY guarantees uniqueness if all the grouped columns appear in
	 * the output.	In our implementation this means checking they are non
	 * resjunk columns.
	 */
	if (query->groupClause)
	{
		ListCell   *gl;

		foreach(gl, query->groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(gl);
			TargetEntry *tle = get_sortgroupclause_tle(grpcl,
													   query->targetList);

			if (tle->resdom->resjunk)
				break;
		}
		if (!gl)				/* got to the end? */
			return true;
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
create_subqueryscan_path(Query *root, RelOptInfo *rel, List *pathkeys)
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
create_functionscan_path(Query *root, RelOptInfo *rel)
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
create_nestloop_path(Query *root,
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
 * 'outersortkeys' are the sort varkeys for the outer relation
 *      or NIL to use existing ordering
 * 'innersortkeys' are the sort varkeys for the inner relation
 *      or NIL to use existing ordering
 */
MergePath *
create_mergejoin_path(Query *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  List *mergeclauses,
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
                                         mergeclauses,
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
	 * If we are not sorting the inner path, we may need a materialize
	 * node to ensure it can be marked/restored.  (Sort does support
	 * mark/restore, so no materialize is needed in that case.)
	 *
	 * Since the inner side must be ordered, and only Sorts and IndexScans
	 * can create order to begin with, you might think there's no problem
	 * --- but you'd be wrong.  Nestloop and merge joins can *preserve*
	 * the order of their inputs, so they can be selected as the input of
	 * a mergejoin, and they don't support mark/restore at present.
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
create_hashjoin_path(Query *root,
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
    pathnode->jpath.path.rescannable = outer_path->rescannable; /* inner materialized by Hash */
	pathnode->path_hashclauses = hashclauses;

	cost_hashjoin(pathnode, root);

	return pathnode;
}

