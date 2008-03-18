/*-------------------------------------------------------------------------
 *
 * cdbsubplan.c
 *	  Provides routines for preprocessing initPlan subplans
 *		and executing queries with initPlans
 *
 * Copyright (c) 2003-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/nodeSubplan.h"		/* For ExecSetParamPlan */

#include "cdb/cdbsubplan.h"
#include "cdb/cdbvars.h"                /* currentSliceId */


/*
 * Function preprocess_initplans() is called from ExecutorRun running a
 * parallel plan on the QD.  The call happens prior to dispatch of the
 * main plan, and only if there are some initplans.
 *
 * Argument queryDesc is the one passed in to ExecutorRun.
 *
 * The function loops through the estate->es_param_exec_vals array, which
 * has plan->nParamExec elements.  Each element is a ParamExecData struct,
 * and the index of the element in the array is the paramid of the Param
 * node in the Plan that corresponds to the result of the subquery.
 *
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
 * of the ParamExecData struct(s).	It finds the proper one based on the
 * setParam list in the SubPlan node.
 * In order to handle SubPlans of SubPlans, we pass in the values of the
 * estate->es_param_exec_vals as ParamListInfo structs to the ExecSetParamPlan call.
 * These are then serialized into the mppexec all as parameters.  In this manner, the
 * result of a SubPlan of a SubPlan is available.
 */
void
preprocess_initplans(QueryDesc *queryDesc)
{
    PlannedStmt    *plannedStmt = queryDesc->plannedstmt;
	ParamListInfo originalPli,
				augmentedPli;
	int			i;
	EState	   *estate = queryDesc->estate;
	int			originalRoot,
                originalSlice,
				rootIndex;

	if (plannedStmt->nParamExec == 0)
		return;

	originalPli = queryDesc->params;
	originalRoot = queryDesc->estate->es_cur_root_idx;
    originalSlice = queryDesc->estate->es_cur_slice_idx;
	Assert(originalSlice == 0 &&
           queryDesc->estate->es_sliceTable->localSlice == originalSlice);

	/*
	 * Loop through the estate->es_param_exec_vals. This array has an element
	 * for each PARAM_EXEC (internal) param, and a pointer to the SubPlanState
	 * to execute to evaluate it. It seems that they are created in the proper
	 * order, i.e. if a subplan x has a sublan y, then y will come before x in
	 * the es_param_exec_vals array.
	 */
	for (i = 0; i < plannedStmt->nParamExec; i++)
	{
		ParamExecData *prm;
		SubPlanState *sps;

		prm = &estate->es_param_exec_vals[i];
		sps = (SubPlanState *) prm->execPlan;

		/*
		 * Append all the es_param_exec_vals datum values on to the external
		 * parameter list so they can be serialized in the mppexec call to the
		 * QEs.  Do this inside the loop since later initplans may depend on
		 * the results of earlier ones.
		 *
		 * TODO Some of the work of addRemoteExecParamsToParmList could be
		 *		factored out of the loop.
		 */
		augmentedPli = addRemoteExecParamsToParamList(originalPli,
										              estate->es_param_exec_vals,
                                                      plannedStmt->paramExecTypes,
                                                      plannedStmt->nParamExec);
		if (sps != NULL)
		{
            SubPlan    *subplan = (SubPlan *)sps->xprstate.expr;

            Assert(IsA(subplan, SubPlan));

			/*
			 * Adjust for the slice to execute on the QD.
			 */
			rootIndex = sps->topPlan->qDispSliceId;

            Assert(rootIndex > estate->es_sliceTable->nMotions &&
                   rootIndex < list_length(estate->es_sliceTable->slices));

            queryDesc->estate->es_cur_root_idx = rootIndex;		/* ! */
            queryDesc->estate->es_cur_slice_idx = rootIndex;
			queryDesc->estate->es_sliceTable->localSlice = rootIndex;

			/* set our global sliceid variable for elog. */
			currentSliceId = rootIndex;

			/*
			 * This runs the SubPlan and puts the answer back into prm->value.
			 */
			queryDesc->params = augmentedPli;

			ExecSetParamPlan(sps, sps->planstate->ps_ExprContext, queryDesc);
		}

		queryDesc->params = originalPli;
		queryDesc->estate->es_cur_root_idx = originalRoot;
		queryDesc->estate->es_cur_slice_idx = originalSlice;
		queryDesc->estate->es_sliceTable->localSlice = originalSlice;
		currentSliceId = originalSlice;

		pfree(augmentedPli);
	}
}

/*
 * Function: addRemoteExecParamsToParamList()
 *
 * Creates a new ParamListInfo array from the existing one by appending
 * the array of ParamExecData (internal parameter) values as a new Param
 * type: PARAM_EXEC_REMOTE.
 *
 * When the query eventually runs (on the QD or a QE), it will have access
 * to the augmented ParamListInfo (locally or through serialization).
 *
 * Then, rather than lazily-evaluating the SubPlan to get its value (as for
 * an internal parameter), the plan will just look up the param value in the
 * ParamListInfo (as for an external parameter).
 */
ParamListInfo 
addRemoteExecParamsToParamList(ParamListInfo    extPrm, 
                               ParamExecData   *paramExecData,
                               Oid             *paramExecTypes,
                               int              nParamExec)
{
	ParamListInfo   augPrm;
	int			    i;
	int			    nParams;

	if (nParamExec == 0)
		return extPrm;
	Assert(paramExecData);		/* So there must be some internal parameters. */

	/* Count existing external parameters. */
	nParams = (extPrm == NULL ? 0 : extPrm->numParams);

	/* Allocate space for augmented array and set final sentinel. */
	augPrm = palloc0(sizeof(ParamListInfoData) + (nParams + nParamExec)* sizeof(ParamExternData));
	augPrm->numParams = nParams + nParamExec;

	/* Copy in the existing external parameter entries. */
	memcpy(augPrm->params, extPrm->params, nParams * sizeof(augPrm->params[0]));

	/*
	 * Now initialize the ParamListInfo elements corresponding to the
	 * initplans we're "parameterizing".  Use the datatype info saved
	 * by the planner.
	 */
	for (i = 0; i < nParamExec; i++)
	{
		int     j = nParams + i;

		augPrm->params[j].ptype = paramExecTypes[i];
		augPrm->params[j].isnull = paramExecData[i].isnull;
		augPrm->params[j].value = paramExecData[i].value;
	}

	return augPrm;
}
