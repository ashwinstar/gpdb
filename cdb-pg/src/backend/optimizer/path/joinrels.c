/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/joinrels.c,v 1.76.2.1 2005/11/22 18:23:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"                  /* CHECK_FOR_INTERRUPTS */
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


static List *make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  ListCell *other_rels);
static List *make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  ListCell *other_rels);
static bool is_inside_IN(PlannerInfo *root, RelOptInfo *rel);


/*
 * make_rels_by_joins
 *	  Consider ways to produce join relations containing exactly 'level'
 *	  jointree items.  (This is one step of the dynamic-programming method
 *	  embodied in make_one_rel_by_joins.)  Join rel nodes for each feasible
 *	  combination of lower-level rels are created and returned in a list.
 *	  Implementation paths are created for each such joinrel, too.
 *
 * level: level of rels we want to make this time.
 * joinrels[j], 1 <= j < level, is a list of rels containing j items.
 */
List *
make_rels_by_joins(PlannerInfo *root, int level, List **joinrels)
{
	List	   *result_rels = NIL;
	List	   *new_rels;
	ListCell   *r;
	int			k;

	/*
	 * First, consider left-sided and right-sided plans, in which rels of
	 * exactly level-1 member relations are joined against initial relations.
	 * We prefer to join using join clauses, but if we find a rel of level-1
	 * members that has no join clauses, we will generate Cartesian-product
	 * joins against all initial rels not already contained in it.
	 *
	 * In the first pass (level == 2), we try to join each initial rel to each
	 * initial rel that appears later in joinrels[1].  (The mirror-image joins
	 * are handled automatically by make_join_rel.)  In later passes, we try
	 * to join rels of size level-1 from joinrels[level-1] to each initial rel
	 * in joinrels[1].
	 */
	foreach(r, joinrels[level - 1])
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
		ListCell   *other_rels;

		if (level == 2)
			other_rels = lnext(r);		/* only consider remaining initial
										 * rels */
		else
			other_rels = list_head(joinrels[1]);		/* consider all initial
														 * rels */

		if (old_rel->joininfo != NIL)
		{
			/*
			 * Note that if all available join clauses for this rel require
			 * more than one other rel, we will fail to make any joins against
			 * it here.  In most cases that's OK; it'll be considered by
			 * "bushy plan" join code in a higher-level pass where we have
			 * those other rels collected into a join rel.
			 */
			new_rels = make_rels_by_clause_joins(root,
												 old_rel,
												 other_rels);

			/*
			 * An exception occurs when there is a clauseless join inside an
			 * IN (sub-SELECT) construct.  Here, the members of the subselect
			 * all have join clauses (against the stuff outside the IN), but
			 * they *must* be joined to each other before we can make use of
			 * those join clauses.	So do the clauseless join bit.
			 *
			 * See also the last-ditch case below.
			 */
			if (new_rels == NIL && is_inside_IN(root, old_rel))
				new_rels = make_rels_by_clauseless_joins(root,
														 old_rel,
														 other_rels);
		}
		else
		{
			/*
			 * Oops, we have a relation that is not joined to any other
			 * relation.  Cartesian product time.
			 */
			new_rels = make_rels_by_clauseless_joins(root,
													 old_rel,
													 other_rels);
		}

		/*
		 * At levels above 2 we will generate the same joined relation in
		 * multiple ways --- for example (a join b) join c is the same
		 * RelOptInfo as (b join c) join a, though the second case will add a
		 * different set of Paths to it.  To avoid making extra work for
		 * subsequent passes, do not enter the same RelOptInfo into our output
		 * list multiple times.
		 */
		result_rels = list_concat_unique_ptr(result_rels, new_rels);
	}

	/*
	 * Now, consider "bushy plans" in which relations of k initial rels are
	 * joined to relations of level-k initial rels, for 2 <= k <= level-2.
	 *
	 * We only consider bushy-plan joins for pairs of rels where there is a
	 * suitable join clause, in order to avoid unreasonable growth of planning
	 * time.
	 */
	for (k = 2;; k++)
	{
		int			other_level = level - k;

		/*
		 * Since make_join_rel(x, y) handles both x,y and y,x cases, we only
		 * need to go as far as the halfway point.
		 */
		if (k > other_level)
			break;

		foreach(r, joinrels[k])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			ListCell   *other_rels;
			ListCell   *r2;

			if (old_rel->joininfo == NIL)
				continue;		/* we ignore clauseless joins here */

			if (k == other_level)
				other_rels = lnext(r);	/* only consider remaining rels */
			else
				other_rels = list_head(joinrels[other_level]);

			for_each_cell(r2, other_rels)
			{
				RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);

				if (!bms_overlap(old_rel->relids, new_rel->relids))
				{
					/*
					 * OK, we can build a rel of the right level from this
					 * pair of rels.  Do so if there is at least one usable
					 * join clause.
					 */
					if (have_relevant_joinclause(old_rel, new_rel))
					{
						RelOptInfo *jrel;

						jrel = make_join_rel(root, old_rel, new_rel,
											 JOIN_INNER);
						/* Avoid making duplicate entries ... */
						if (jrel)
							result_rels = list_append_unique_ptr(result_rels,
																 jrel);
					}
				}
			}
		}
	}

	/*
	 * Last-ditch effort: if we failed to find any usable joins so far, force
	 * a set of cartesian-product joins to be generated.  This handles the
	 * special case where all the available rels have join clauses but we
	 * cannot use any of the joins yet.  An example is
	 *
	 * SELECT * FROM a,b,c WHERE (a.f1 + b.f2 + c.f3) = 0;
	 *
	 * The join clause will be usable at level 3, but at level 2 we have no
	 * choice but to make cartesian joins.	We consider only left-sided and
	 * right-sided cartesian joins in this case (no bushy).
	 */
	if (result_rels == NIL)
	{
		/*
		 * This loop is just like the first one, except we always call
		 * make_rels_by_clauseless_joins().
		 */
		foreach(r, joinrels[level - 1])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			ListCell   *other_rels;

			if (level == 2)
				other_rels = lnext(r);	/* only consider remaining initial
										 * rels */
			else
				other_rels = list_head(joinrels[1]);	/* consider all initial
														 * rels */

			new_rels = make_rels_by_clauseless_joins(root,
													 old_rel,
													 other_rels);

			result_rels = list_concat_unique_ptr(result_rels, new_rels);
		}

		/*----------
		 * When IN clauses are involved, there may be no legal way to make
		 * an N-way join for some values of N.	For example consider
		 *
		 * SELECT ... FROM t1 WHERE
		 *	 x IN (SELECT ... FROM t2,t3 WHERE ...) AND
		 *	 y IN (SELECT ... FROM t4,t5 WHERE ...)
		 *
		 * We will flatten this query to a 5-way join problem, but there are
		 * no 4-way joins that make_join_rel() will consider legal.  We have
		 * to accept failure at level 4 and go on to discover a workable
		 * bushy plan at level 5.
		 *
		 * However, if there are no IN clauses then make_join_rel() should
		 * never fail, and so the following sanity check is useful.
		 *----------
		 */
		if (result_rels == NIL && root->in_info_list == NIL)
			elog(ERROR, "failed to build any %d-way joins", level);
	}

	return result_rels;
}

/*
 * make_rels_by_clause_joins
 *	  Build joins between the given relation 'old_rel' and other relations
 *	  that are mentioned within old_rel's joininfo list (i.e., relations
 *	  that participate in join clauses that 'old_rel' also participates in).
 *	  The join rel nodes are returned in a list.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': the first cell in a linked list containing the other
 * rels to be considered for joining
 *
 * Currently, this is only used with initial rels in other_rels, but it
 * will work for joining to joinrels too.
 */
static List *
make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  ListCell *other_rels)
{
	List	   *result = NIL;
	ListCell   *l;

	for_each_cell(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(old_rel->relids, other_rel->relids) &&
			have_relevant_joinclause(old_rel, other_rel))
		{
			RelOptInfo *jrel;

			jrel = make_join_rel(root, old_rel, other_rel, JOIN_INNER);
			if (jrel)
				result = lcons(jrel, result);
		}
	}

	return result;
}

/*
 * make_rels_by_clauseless_joins
 *	  Given a relation 'old_rel' and a list of other relations
 *	  'other_rels', create a join relation between 'old_rel' and each
 *	  member of 'other_rels' that isn't already included in 'old_rel'.
 *	  The join rel nodes are returned in a list.
 *
 * 'old_rel' is the relation entry for the relation to be joined
 * 'other_rels': the first cell of a linked list containing the
 * other rels to be considered for joining
 *
 * Currently, this is only used with initial rels in other_rels, but it would
 * work for joining to joinrels too.
 */
static List *
make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  ListCell *other_rels)
{
	List	   *result = NIL;
	ListCell   *i;

	for_each_cell(i, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(i);

		if (!bms_overlap(other_rel->relids, old_rel->relids))
		{
			RelOptInfo *jrel;

			jrel = make_join_rel(root, old_rel, other_rel, JOIN_INNER);

			/*
			 * As long as given other_rels are distinct, don't need to test to
			 * see if jrel is already part of output list.
			 */
			if (jrel)
				result = lcons(jrel, result);
		}
	}

	return result;
}


/*
 * is_inside_IN
 *		Detect whether the specified relation is inside an IN (sub-SELECT).
 *
 * Note that we are actually only interested in rels that have been pulled up
 * out of an IN, so the routine name is a slight misnomer.
 */
static bool
is_inside_IN(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, ininfo->righthand))
			return true;
	}
	return false;
}


/*
 * make_jointree_rel
 *		Find or build a RelOptInfo join rel representing a specific
 *		jointree item.	For JoinExprs, we only consider the construction
 *		path that corresponds exactly to what the user wrote.
 */
RelOptInfo *
make_jointree_rel(PlannerInfo *root, Node *jtnode)
{
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		return find_base_rel(root, varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;

		/* Recurse back to multi-way-join planner */
		return make_fromexpr_rel(root, f);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		RelOptInfo *rel,
				   *lrel,
				   *rrel;

		/* Recurse */
		lrel = make_jointree_rel(root, j->larg);
		rrel = make_jointree_rel(root, j->rarg);

		/* Make this join rel */
		rel = make_join_rel(root, lrel, rrel, j->jointype);

		if (rel == NULL)		/* oops */
			elog(ERROR, "invalid join order");

		/*
		 * Since we are only going to consider this one way to do it, we're
		 * done generating Paths for this joinrel and can now select the
		 * cheapest.  In fact we *must* do so now, since next level up will
		 * need it!
		 */
		set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
		debug_print_rel(root, rel);
#endif

		return rel;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return NULL;				/* keep compiler quiet */
}


/*
 * cdb_add_subquery_join_paths
 *
 * When a subquery from a search condition has been flattened into a join,
 * the join may need some special mojo.  A row of the main query must not
 * join with more than one row of the subquery, or in case it does, the
 * consequent multiple result rows must be collapsed to a single row
 * afterwards.
 */
static void
cdb_add_subquery_join_paths(PlannerInfo    *root,
					        RelOptInfo     *joinrel,
					        RelOptInfo     *rel1,
					        RelOptInfo     *rel2,
					        JoinType        jointype,
					        JoinType        swapjointype,
					        List           *restrictlist)
{
    ListCell   *cell;
    int         ndedup = 0;

    Assert(root->ndedup == 0 && root->adedup != NULL);

    foreach(cell, root->in_info_list)
	{
        InClauseInfo   *ininfo = (InClauseInfo *)lfirst(cell);

        /*
         * Skip if duplicate removal already handled in subplan.
         *
         * CDB TODO: Consider delaying the duplicate removal in case a
         * later join might reduce the amount of data thus making duplicate
         * removal cheaper.  For now, we remove dups as early as possible.
         */
        if (bms_is_subset(ininfo->allrelids, rel1->relids) ||
            bms_is_subset(ininfo->allrelids, rel2->relids))
            continue;

        /*
         * If joinrel includes all baserels needed for evaluating the
         * clause, consider ordinary join followed by duplicate removal.
         * Add to post-join duplicate removal list for add_path().
         */
        if (bms_is_subset(ininfo->allrelids, joinrel->relids))
        {
            if (ndedup == 0)
                root->dedup_relids = bms_copy(joinrel->relids);
            root->dedup_relids = bms_difference(root->dedup_relids, ininfo->righthand);
            root->adedup[ndedup++] = ininfo;
        }

        /*
         * If either input rel happens to be exactly a subquery result,
         * consider suppressing duplicates before or during the join.
         */
        if (bms_equal(ininfo->righthand, rel1->relids) ||
            bms_equal(ininfo->righthand, rel2->relids))
        {
            RelOptInfo *subqrel = bms_equal(ininfo->righthand, rel1->relids)
                                ? rel1
                                : rel2;
            RelOptInfo *curqrel = (subqrel == rel1)
                                ? rel2
                                : rel1;

            Assert(jointype == JOIN_INNER);

	        /*
	         * JOIN_IN matches each row of outer rel (current query) with at
             * most one row of the inner rel (subquery), using modified join
             * logic in the executor.  It doesn't need an extra step for
             * duplicate removal.  Consider it if all of the baserels needed
             * for evaluating the clause are present.
	         */
            if (bms_is_subset(ininfo->allrelids, joinrel->relids))
		        add_paths_to_joinrel(root, joinrel, curqrel, subqrel, JOIN_IN,
							         restrictlist);

            /*
             * JOIN_UNIQUE removes duplicates from the rel containing the
             * subquery result, then does an ordinary equijoin.
             *
             * CDB TODO: We wouldn't have to do anything special if we knew
             * that the subquery result rows were already distinct.
             *
             * CDB TODO: Correlated subquery could use JOIN_UNIQUE if all outer
             * refs can be pulled up into the join operator (or inner indexscan).
             */
            if (ininfo->op_is_equality &&
                !ininfo->correlating)
            {
		        add_paths_to_joinrel(root, joinrel, curqrel, subqrel, JOIN_UNIQUE_INNER,
							         restrictlist);
		        add_paths_to_joinrel(root, joinrel, subqrel, curqrel, JOIN_UNIQUE_OUTER,
							         restrictlist);
            }
        }
	}

    /*
	 * Consider paths using each rel as both outer and inner (i.e., normal
     * join planning).  Set root->ndedup so add_path() will apply duplicate
     * removal to the paths for any subquery clauses that are completed by
     * this join.
     */
    root->ndedup = ndedup;
    add_paths_to_joinrel(root, joinrel, rel1, rel2, jointype, restrictlist);
    add_paths_to_joinrel(root, joinrel, rel2, rel1, swapjointype, restrictlist);
    root->ndedup = 0;
}                               /* cdb_add_subquery_join_paths */


/*
 * make_join_rel
 *	   Find or create a join RelOptInfo that represents the join of
 *	   the two given rels, and add to it path information for paths
 *	   created with the two rels as outer and inner rel.
 *	   (The join rel may already contain paths generated from other
 *	   pairs of rels that add up to the same set of base rels.)
 *
 * NB: will return NULL if attempted join is not valid.  This can only
 * happen when working with IN clauses that have been turned into joins.
 */
RelOptInfo *
make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
			  JoinType jointype)
{
	Relids		joinrelids;
	RelOptInfo *joinrel;
	List	   *restrictlist;
    JoinType    swapjointype = (jointype == JOIN_LEFT)  ? JOIN_RIGHT
                             : (jointype == JOIN_RIGHT) ? JOIN_LEFT
                             :                            jointype;

	/* This is a convenient place to check for query cancel. */
	CHECK_FOR_INTERRUPTS();

	/* We should never try to join two overlapping sets of rels. */
	Assert(!bms_overlap(rel1->relids, rel2->relids));
    Assert(root->ndedup == 0);

	/* Construct Relids set that identifies the joinrel. */
	joinrelids = bms_union(rel1->relids, rel2->relids);

	/*
	 * Find or build the join RelOptInfo, and compute the restrictlist that
	 * goes with this particular joining.
	 */
	joinrel = build_join_rel(root, joinrelids, rel1, rel2, jointype,
							 &restrictlist);

    /*
     * CDB: Handle removal of duplicates introduced by flattened subqueries.
     */
    if (root->in_info_list)
        cdb_add_subquery_join_paths(root,
                                    joinrel,
                                    rel1,
                                    rel2,
                                    jointype,
                                    swapjointype,
                                    restrictlist);

    /*
	 * Consider paths using each rel as both outer and inner.
	 */
    else
    {
        add_paths_to_joinrel(root, joinrel, rel1, rel2, jointype, restrictlist);
        add_paths_to_joinrel(root, joinrel, rel2, rel1, swapjointype, restrictlist);
    }

	bms_free(joinrelids);
	return joinrel;
}                               /* make_join_rel */
