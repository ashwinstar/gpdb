/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"

TargetEntry    *
tlist_member(Node * node, List * targetlist)
{
	check_expected(node);
	check_expected(targetlist);
	optional_assignment(node);
	optional_assignment(targetlist);
	return (TargetEntry *) (unsigned) mock();
}

TargetEntry    *
tlist_member_ignoring_RelabelType(Expr * expr, List * targetlist)
{
	check_expected(expr);
	check_expected(targetlist);
	optional_assignment(expr);
	optional_assignment(targetlist);
	return (TargetEntry *) (unsigned) mock();
}

List           *
flatten_tlist(List * tlist)
{
	check_expected(tlist);
	optional_assignment(tlist);
	return (List *) (unsigned) mock();
}

List           *
add_to_flat_tlist(List * tlist, List * exprs, bool resjunk)
{
	check_expected(tlist);
	check_expected(exprs);
	check_expected(resjunk);
	optional_assignment(tlist);
	optional_assignment(exprs);
	return (List *) (unsigned) mock();
}

TargetEntry    *
get_sortgroupclause_tle(SortClause * sortClause, List * targetList)
{
	check_expected(sortClause);
	check_expected(targetList);
	optional_assignment(sortClause);
	optional_assignment(targetList);
	return (TargetEntry *) (unsigned) mock();
}

List           *
get_sortgroupclauses_tles(List * clauses, List * targetList)
{
	check_expected(clauses);
	check_expected(targetList);
	optional_assignment(clauses);
	optional_assignment(targetList);
	return (List *) (unsigned) mock();
}

Node           *
get_sortgroupclause_expr(SortClause * sortClause, List * targetList)
{
	check_expected(sortClause);
	check_expected(targetList);
	optional_assignment(sortClause);
	optional_assignment(targetList);
	return (Node *) (unsigned) mock();
}

List           *
get_sortgrouplist_exprs(List * sortClauses, List * targetList)
{
	check_expected(sortClauses);
	check_expected(targetList);
	optional_assignment(sortClauses);
	optional_assignment(targetList);
	return (List *) (unsigned) mock();
}

AttrNumber     *
get_grouplist_colidx(List * groupClauses, List * targetList, int *numCols)
{
	check_expected(groupClauses);
	check_expected(targetList);
	check_expected(numCols);
	optional_assignment(groupClauses);
	optional_assignment(targetList);
	optional_assignment(numCols);
	return (AttrNumber *) (unsigned) mock();
}

List           *
get_grouplist_exprs(List * groupClauses, List * targetList)
{
	check_expected(groupClauses);
	check_expected(targetList);
	optional_assignment(groupClauses);
	optional_assignment(targetList);
	return (List *) (unsigned) mock();
}

bool 
tlist_same_datatypes(List * tlist, List * colTypes, bool junkOK)
{
	check_expected(tlist);
	check_expected(colTypes);
	check_expected(junkOK);
	optional_assignment(tlist);
	optional_assignment(colTypes);
	return (bool) mock();
}

Index 
maxSortGroupRef(List * targetlist, bool include_orderedagg)
{
	check_expected(targetlist);
	check_expected(include_orderedagg);
	optional_assignment(targetlist);
	return (Index) mock();
}

int 
get_row_width(List * tlist)
{
	check_expected(tlist);
	optional_assignment(tlist);
	return (int) mock();
}
