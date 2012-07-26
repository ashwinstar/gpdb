/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include <dlfcn.h>
#include "postgres.h"
#include <limits.h>
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/execHHashagg.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/planpartition.h"
#include "optimizer/transform.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "nodes/print.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "nodes/bitmapset.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbmutate.h"	/* apply_shareinput */
#include "cdb/cdbpath.h"	/* cdbpath_segments */
#include "cdb/cdbpathtoplan.h"	/* cdbpathtoplan_create_flow() */
#include "cdb/cdbgroup.h"	/* grouping_planner extensions */
#include "cdb/cdbsetop.h"	/* motion utilities */
#include "cdb/cdbsubselect.h"	/* cdbsubselect_flatten_sublinks() */
#include "utils/debugbreak.h"
#include "catalog/gp_policy.h"
#include "postmaster/optserver.h"

double          cursor_tuple_fraction = 1.0;
planner_hook_type planner_hook = 0;
ParamListInfo   PlannerBoundParamList = 0;
PlannedStmt    *
planner(Query * parse, int cursorOptions, ParamListInfo boundParams)
{
	check_expected(parse);
	check_expected(cursorOptions);
	check_expected(boundParams);
	optional_assignment(parse);
	return (PlannedStmt *) (unsigned) mock();
}

PlannedStmt    *
standard_planner(Query * parse, int cursorOptions, ParamListInfo boundParams)
{
	check_expected(parse);
	check_expected(cursorOptions);
	check_expected(boundParams);
	optional_assignment(parse);
	return (PlannedStmt *) (unsigned) mock();
}

Plan           *
subquery_planner(PlannerGlobal * glob, Query * parse, PlannerInfo * parent_root, double tuple_fraction, PlannerInfo ** subroot, PlannerConfig * config)
{
	check_expected(glob);
	check_expected(parse);
	check_expected(parent_root);
	check_expected(tuple_fraction);
	check_expected(subroot);
	check_expected(config);
	optional_assignment(glob);
	optional_assignment(parse);
	optional_assignment(parent_root);
	optional_assignment(subroot);
	optional_assignment(config);
	return (Plan *) (unsigned) mock();
}

void 
grouping_planner_output_asserts(PlannerInfo * root, Plan * plan)
{
	check_expected(root);
	check_expected(plan);
	optional_assignment(root);
	optional_assignment(plan);
	mock();
}

bool 
is_dummy_plan(Plan * plan)
{
	check_expected(plan);
	optional_assignment(plan);
	return (bool) mock();
}

bool 
choose_hashed_grouping(PlannerInfo * root, double tuple_fraction, Path * cheapest_path, Path * sorted_path, double dNumGroups, AggClauseCounts * agg_counts)
{
	check_expected(root);
	check_expected(tuple_fraction);
	check_expected(cheapest_path);
	check_expected(sorted_path);
	check_expected(dNumGroups);
	check_expected(agg_counts);
	optional_assignment(root);
	optional_assignment(cheapest_path);
	optional_assignment(sorted_path);
	optional_assignment(agg_counts);
	return (bool) mock();
}

List           *
register_ordered_aggs(List * tlist, Node * havingqual, List * sub_tlist)
{
	check_expected(tlist);
	check_expected(havingqual);
	check_expected(sub_tlist);
	optional_assignment(tlist);
	optional_assignment(havingqual);
	optional_assignment(sub_tlist);
	return (List *) (unsigned) mock();
}
