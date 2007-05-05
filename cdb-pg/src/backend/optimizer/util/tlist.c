/*-------------------------------------------------------------------------
 *
 * tlist.c
 *	  Target list manipulation routines
 *
 * Portions Copyright (c) 2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/tlist.c,v 1.73 2006/08/10 02:36:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"


/*****************************************************************************
 *		Target list creation and searching utilities
 *****************************************************************************/

/*
 * tlist_member
 *	  Finds the (first) member of the given tlist whose expression is
 *	  equal() to the given expression.	Result is NULL if no such member.
 */
TargetEntry *
tlist_member(Node *node, List *targetlist)
{
	ListCell   *temp;

	foreach(temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);

        Assert(IsA(tlentry, TargetEntry));

		if (equal(node, tlentry->expr))
			return tlentry;
	}
	return NULL;
}


/*
 * tlist_member_ignoring_RelabelType
 *	  Finds the (first) member of the given tlist whose expression is
 *	  equal() to the given expression.	Result is NULL if no such member.
 *
 * Disregards the presence or absence of a RelabelType node atop the
 * given expression and/or the tlist expressions.
 */
TargetEntry *
tlist_member_ignoring_RelabelType(Expr *expr, List *targetlist)
{
    ListCell   *temp;

    if (IsA(expr, RelabelType))
        expr = ((RelabelType *)expr)->arg;

    foreach(temp, targetlist)
    {
        TargetEntry *tlentry = (TargetEntry *)lfirst(temp);
        Expr        *tlexpr = tlentry->expr;

        Assert(IsA(tlentry, TargetEntry));

        if (IsA(tlexpr, RelabelType))
            tlexpr = ((RelabelType *)tlexpr)->arg;

        if (equal(expr, tlexpr))
            return tlentry;
	}
	return NULL;
}                               /* tlist_member_ignoring_RelabelType */


/*
 * flatten_tlist
 *	  Create a target list that only contains unique variables.
 *
 * Note that Vars with varlevelsup > 0 are not included in the output
 * tlist.  We expect that those will eventually be replaced with Params,
 * but that probably has not happened at the time this routine is called.
 *
 * 'tlist' is the current target list
 *
 * Returns the "flattened" new target list.
 *
 * The result is entirely new structure sharing no nodes with the original.
 * Copying the Var nodes is probably overkill, but be safe for now.
 */
List *
flatten_tlist(List *tlist)
{
	List	   *vlist = pull_var_clause((Node *) tlist, false);
	List	   *new_tlist;

	new_tlist = add_to_flat_tlist(NIL, vlist);
	list_free(vlist);
	return new_tlist;
}

/*
 * add_to_flat_tlist
 *		Add more vars to a flattened tlist (if they're not already in it)
 *
 * 'tlist' is the flattened tlist
 * 'vars' is a list of var nodes
 *
 * Returns the extended tlist.
 */
List *
add_to_flat_tlist(List *tlist, List *vars)
{
	int			next_resno = list_length(tlist) + 1;
	ListCell   *v;

	foreach(v, vars)
	{
		Var		   *var = (Var *) lfirst(v);

		if (!tlist_member((Node *) var, tlist))
		{
			TargetEntry *tle;

			tle = makeTargetEntry(copyObject(var),		/* copy needed?? */
								  next_resno++,
								  NULL,
								  false);
			tlist = lappend(tlist, tle);
		}
	}
	return tlist;
}

/*
 * get_sortgroupclause_tle
 *		Find the targetlist entry matching the given SortClause
 *		(or GroupClause) by ressortgroupref, and return it.
 *
 * Because GroupClause is typedef'd as SortClause, either kind of
 * node can be passed without casting.
 */
TargetEntry *
get_sortgroupclause_tle(SortClause *sortClause,
						List *targetList)
{
	Index		refnumber = sortClause->tleSortGroupRef;
	ListCell   *l;

	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->ressortgroupref == refnumber)
			return tle;
	}

	elog(ERROR, "ORDER/GROUP BY expression not found in targetlist");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_sortgroupclauses_tles
 *      Find a list of unique targetlist entries matching the given list of
 *      SortClause, GroupClause, or GroupingClauses.
 *
 * In each grouping set, targets that do not appear in a GroupingClause
 * will be put in the front of those that appear in a GroupingClauses.
 * The targets within the same clause will be appended in the order
 * of their appearance.
 */
List *
get_sortgroupclauses_tles(List *clauses, List *targetList)
{
	List *result = NIL;
	ListCell *l;

	List *sub_grouping_result = NIL;

	foreach(l, clauses)
	{
		Node *node = lfirst(l);

		if (IsA(node, GroupClause) || IsA(node, SortClause))
		{
			TargetEntry *tle =
				get_sortgroupclause_tle((SortClause*)node, targetList);

			if (!list_member(result, tle))
				result = lappend(result, tle);
		}

		else
		{
			List *sub_list;
			Assert (IsA(node, GroupingClause));

			sub_list =
				get_sortgroupclauses_tles(((GroupingClause*)node)->groupsets,
										  targetList);

			sub_grouping_result =
				list_concat_unique(sub_grouping_result, sub_list);
		}
	}

	/* Put GroupClauses before GroupingClauses. */
	result = list_concat_unique(result, sub_grouping_result);

	return result;
}

/*
 * get_sortgroupclause_expr
 *		Find the targetlist entry matching the given SortClause
 *		(or GroupClause) by ressortgroupref, and return its expression.
 *
 * Because GroupClause is typedef'd as SortClause, either kind of
 * node can be passed without casting.
 */
Node *
get_sortgroupclause_expr(SortClause *sortClause, List *targetList)
{
	TargetEntry *tle = get_sortgroupclause_tle(sortClause, targetList);

	return (Node *) tle->expr;
}

/*
 * get_sortgrouplist_exprs
 *		Given a list of SortClauses (or GroupClauses), build a list
 *		of the referenced targetlist expressions.
 */
List *
get_sortgrouplist_exprs(List *sortClauses, List *targetList)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, sortClauses)
	{
		SortClause *sortcl = (SortClause *) lfirst(l);
		Node	   *sortexpr;

		sortexpr = get_sortgroupclause_expr(sortcl, targetList);
		result = lappend(result, sortexpr);
	}
	return result;
}

/*
 * get_grouplist_exprs
 *     Find a list of referenced targetlist expressions used in a given
 *     list of GroupClauses or a GroupingClauses.
 *
 * All expressions will appear in the same order as they appear in the
 * given list of GroupClauses or a GroupingClauses.
 */
List *
get_grouplist_exprs(List *groupClauses, List *targetList)
{
	List *result = NIL;
	ListCell *l;

	foreach (l, groupClauses)
	{
		Node *groupClause = lfirst(l);

		Assert(IsA(groupClause, GroupClause) || IsA(groupClause, GroupingClause));

		if (IsA(groupClause, GroupClause))
			result = lappend(result,
							 get_sortgroupclause_expr((GroupClause*)groupClause,
													  targetList));

		else if (IsA(groupClause, GroupingClause))
			result = list_concat(result,
								 get_grouplist_exprs(((GroupingClause*)groupClause)->groupsets,
													 targetList));
	}

	return result;
}


/*
 * Does tlist have same output datatypes as listed in colTypes?
 *
 * Resjunk columns are ignored if junkOK is true; otherwise presence of
 * a resjunk column will always cause a 'false' result.
 *
 * Note: currently no callers care about comparing typmods.
 */
bool
tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK)
{
	ListCell   *l;
	ListCell   *curColType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
		{
			if (!junkOK)
				return false;
		}
		else
		{
			if (curColType == NULL)
				return false;	/* tlist longer than colTypes */
			if (exprType((Node *) tle->expr) != lfirst_oid(curColType))
				return false;
			curColType = lnext(curColType);
		}
	}
	if (curColType != NULL)
		return false;			/* tlist shorter than colTypes */
	return true;
}
