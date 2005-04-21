/*-------------------------------------------------------------------------
 *
 * cdbsubplan.c
 *	  Provides routines for preprocessing initPlan subplans
 *		and executing queries with initPlans
 *
 * Copyright (c) 2003-2005, Metapa
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "executor/nodeSubplan.h"  /* For ExecSetParamPlan */
#include "cdb/cdbplan.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbsubplan.h"

typedef struct ParamWalkerContext
{
	List *params;
} ParamWalkerContext;

static bool param_walker(Node *node, ParamWalkerContext *context);
static Oid findParamType( List *params, int paramid );

/*
 * preprocess_initplans:
 * This function is called from ExecutorRun.
 * It loops through the estate->es_param_exec_vals array, 
 * which has count plan->nParamExec.
 * Each element of the array is a ParamExecData struct.
 * The index of the element in the array is the paramid of the
 * Param struct that is in the Plan where the subquery is used.
 * The execPlan member points to a SubPlanState struct for the
 * subquery.  The value and isnull members hold the result
 * of executing the SubPlan.
 * I think that the order of the elements in this array guarantees
 * that for a subplan X within a subplan Y, X will come before Y in the array.
 * If a subplan returns multiple columns (like a MULTIEXPR_SUBLINK), each will be
 * a separate entry in the es_param_exec_vals array, but they will all have
 * the same value for execPlan.
 * In order to evaluate a subplan, we call ExecSetParamPlan.
 * This is a postgres function, but has been modified from its original form
 * to parallelize subplans. Inside ExecSetParamPlan, the
 * datum result(s) of the subplan are stuffed into the value field
 * of the ParamExecData struct(s).  It finds the proper one based on the 
 * setParam list in the SubPlan node.
 * In order to handle SubPlans of SubPlans, we pass in the values of the
 * estate->es_param_exec_vals as ParamListInfo structs to the ExecSetParamPlan call.
 * These are then serialized into the cdbexec all as parameters.  In this manner, the
 * result of a SubPlan of a SubPlan is available.
 */
void preprocess_initplans( QueryDesc *queryDesc )
{
	int i;
	Plan *plan = queryDesc->plantree;
	EState *estate = queryDesc->estate;

	if ( plan->nParamExec == 0 )
		return;

	ParamListInfo p = queryDesc->params;
	ParamListInfo q;

	// Loop through the estate->es_param_exec_vals.
	// This array has an element for each PARAM_EXEC param,
	// and a pointer to the SubPlanState to execute to evaluate it.
	// It seems that they are created in the proper order,
	// i.e. if a subplan x has a sublan y, then y will come before x in 
	// the es_param_exec_vals array.
	for ( i=0; i<plan->nParamExec; i++ )
	{
		ParamExecData *prm;
		SubPlanState *sps;
		prm = &estate->es_param_exec_vals[i];
		sps = (SubPlanState *)prm->execPlan;

		/*
		 * Append all the es_param_exec_vals datun values on to the external parameter list
		 * so they can be serialized in the cdbexec call to the qExecs.
		 */
		q = addRemoteExecParamsToParamList( queryDesc->plantree, p, estate->es_param_exec_vals, plan->nParamExec );

		if ( sps != NULL )
		{
			/*
			 * In the case of a SubPlan of a SubPlan, 
			 */
			sps->planstate->plan->nParamExec = plan->nParamExec;

			/*
			 * TODO: Remove this when the dispatch member of the subplans is
			 * set properly.  Currenly, it may be DISPATCH_UNDETERMINED
			 */
			sps->planstate->plan->dispatch = DISPATCH_PARALLEL;

			/*
			 * This runs the SubPlan and puts the answer back into prm->value.
			 */
			ExecSetParamPlan(sps, sps->planstate->ps_ExprContext, q);
		}

		pfree(q);
	}
}

/*
 * Creates a new ParamListInfo array from the exisiting one by appending
 * the array of ParamExecData values as a new type of Param:
 * PARAM_EXEC_REMOTE.
 *
 * Getting the oid of the param type proved to be a little problematic.
 * It didn't seem to be directly available anywhere, so I created a 
 * Plan walker function that would put all Param nodes of type PARAM_EXEC
 * into a list.  Then, I created a function (findParamType) that would walk the list looking for a
 * specific paramid, and return its type.
 *
 * The query running on the qExec will have the ParamListInfo serialized to it,
 * and then will just look up the param value in the ParamListInfo
 * instead of trying to execute the SubPlan when it needs the value of the SubPlan.
 */
ParamListInfo addRemoteExecParamsToParamList( Plan *plan, ParamListInfo p, ParamExecData *prm, int nprm )
{
	int nParams = 0;
	int i;
	ParamListInfo q = p;

	if ( nprm == 0 )
		return p;

	Assert( prm != NULL );

	if ( q != NULL )
	{
		while ( q->kind != PARAM_INVALID )
		{
			nParams++;
			q++;
		}
	}

	ParamListInfo pli = palloc0( (nprm + nParams + 1) * sizeof(ParamListInfoData) );
	pli[nprm + nParams].kind	= PARAM_INVALID;

	q = p;
	nParams = 0;
	if ( q != NULL )
	{
		while ( q->kind != PARAM_INVALID )
		{
			memcpy( &pli[nParams], q, sizeof(ParamListInfoData) );
			q++;
			nParams++;
		}
	}

	// Walk the plan, looking for param nodes
	// that are kind PARAM_EXEC.
	// We need these for their paramtype field,
	// which doesn't seem to be available in either
	// the ParamExecData struct or the SubPlan struct. 
	ParamWalkerContext context;
	memset( &context, 0, sizeof(context) );
	param_walker((Node*)plan, &context);
	Assert( context.params != NIL );

	for ( i=0; i<nprm; i++ )
	{
		pli[nParams].kind	= PARAM_EXEC_REMOTE;
		pli[nParams].name	= NULL;
		pli[nParams].id		= i;
		pli[nParams].ptype	= findParamType(context.params, i);
		pli[nParams].isnull	= prm[i].isnull;
		pli[nParams].value	= prm[i].value;
		nParams++;
	}

	list_free( context.params );

	return pli;
}

/*
 * param_walker:
 * Simple plan walker that adds any Param nodes it finds to a list in the ParamWalkerContext.
 */
bool param_walker(Node *node, ParamWalkerContext *context)
{	
	if ( node == NULL )
		return false;

	if ( nodeTag(node) == T_Param )
	{
		Param *param = (Param *)node;
		if ( param->paramkind == PARAM_EXEC )
		{
			context->params = lappend( context->params, param );
			return false;
		}
	}

	return plan_tree_walker(node, param_walker, context);
}

/*
 * findParamType:
 * Iterates through a list of Param nodes, trying to match on the passed-in
 * paramid.  If it matches, it returns the paramtype.
 */
Oid findParamType( List *params, int paramid )
{
	ListCell *l;

	foreach(l, params)
	{
		Param *p = lfirst(l);

		if ( p->paramid == paramid )
			return p->paramtype;
	}

	ereport( ERROR,
				(errcode( ERRCODE_CDB_COMMAND_ERROR ),
				errmsg("Failed to locate datatype for paramid %d",
				paramid
			)));

	return InvalidOid;
}
