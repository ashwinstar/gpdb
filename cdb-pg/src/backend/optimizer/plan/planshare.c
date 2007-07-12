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

static ShareInputScan *make_shareinputscan(Plan *inputplan) 
{
	ShareInputScan *sisc = NULL;
	Plan *oldplan = NULL;

	sisc = makeNode(ShareInputScan);

	oldplan = (Plan *) inputplan;
	Assert(IsA(inputplan, Material) || IsA(inputplan, Sort));

	sisc->plan.targetlist = copyObject(oldplan->targetlist);
	sisc->plan.lefttree = oldplan;
	sisc->plan.flow = copyObject(inputplan->flow); 

	if(IsA(inputplan, Material))
	{
		Material *ma = (Material *) inputplan;
		Assert(ma->share_type == SHARE_MATERIAL || ma->share_type == SHARE_MATERIAL_XSLICE);
		Assert(ma->share_id >= 0);
		sisc->share_type = ma->share_type;
		sisc->share_id = ma->share_id;
	}
	else 
	{
		Sort *s = (Sort *) inputplan;
		Assert(s->share_type == SHARE_SORT || s->share_type == SHARE_SORT_XSLICE);
		Assert(s->share_id >= 0);
		sisc->share_type = s->share_type;
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

List *share_plan(PlannerInfo *root, Plan *common, int numpartners)
{
	List *shared_nodes = NULL;
	ShareType stype;
	Query *qry = root->parse;
	Plan *shared = common;
	bool xslice = false;
	int i;

	if(!gp_window_shareinput)
	{
		for(i=0; i<numpartners; ++i)
			shared_nodes = lappend(shared_nodes, copyObject(common));
	}
	else
	{
		if(IsA(common, Material))
		{
			Material *m = (Material *) common;

			Assert(m->share_type == SHARE_NOTSHARED);

			stype = xslice ? SHARE_MATERIAL_XSLICE : SHARE_MATERIAL; 
			m->share_id = list_length(qry->sharedNodes);
			qry->sharedNodes = lappend(qry->sharedNodes, common);
			m->share_type = stype;
		}
		else if (IsA(common, Sort))
		{
			Sort *s = (Sort *) s;

			Assert(s->share_type == SHARE_NOTSHARED);
			stype = xslice ? SHARE_SORT_XSLICE : SHARE_SORT;
			s->share_id = list_length(qry->sharedNodes);
			qry->sharedNodes = lappend(qry->sharedNodes, common);
			s->share_type = stype;
		}
		else
		{
			Path matpath;
			Material *m = make_material(common);
			shared = (Plan *) m;

			cost_material(&matpath, common->total_cost, common->plan_rows, common->plan_width);
			shared->startup_cost = matpath.startup_cost;
			shared->total_cost = matpath.total_cost;
			shared->plan_rows = common->plan_rows;
			shared->plan_width = common->plan_width;
			shared->dispatch = common->dispatch;
			shared->flow = copyObject(common->flow); 

			stype = xslice ? SHARE_MATERIAL_XSLICE : SHARE_MATERIAL; 
			m->share_id = list_length(qry->sharedNodes);
			qry->sharedNodes = lappend(qry->sharedNodes, shared);
			m->share_type = stype;
		}

		for(i=0; i<numpartners; ++i)
		{
			Plan *p = (Plan *) make_shareinputscan(shared);
			shared_nodes = lappend(shared_nodes, p);
		}
	}

	return shared_nodes;
}
