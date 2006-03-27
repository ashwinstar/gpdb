/*-------------------------------------------------------------------------
 *
 * cdbexec.c
 *	  The MPP command MPPEXEC (modelled on PostgreSQL EXECUTE)
 *
 * Copyright (c) 2005-2006, Greenplum Inc.
 * Copyright (c) 2002-2004, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/cdbexec.h"
#include "executor/executor.h"
#include "nodes/print.h"
#include "utils/guc.h"
#include "utils/elog.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbvars.h"

#include "cdb/cdbmotion.h"

/*
 * Implements the 'MPPEXEC' utility statement.
 */
void
CdbExecQuery(CdbExecStmt * stmt, DestReceiver *dest, char *completionTag)
{
	Query	   *query = NULL;
	Plan	   *plan = NULL;
	SliceTable *sliceTable = NULL;
    Slice      *slice = NULL;
	ParamListInfo paramLI = NULL;
	MemoryContext oldctx = NULL,
				qctx;
	int			i,
				n;
	ListCell   *pc;
	char	   *cstr = NULL;
	EState	   *estate = NULL;
	Portal		portal;

	/* tcop shouldn't call unless the role is right! */
	Assert(Mpp_role == MPP_ROLE_EXECUTE);

	/*
	 * TODO - SLICE PLAN INTEGRATION Do something smart with the memory
	 * context.
	 */
	qctx = CurrentMemoryContext;	/* Should probably be some portal context? */
	oldctx = MemoryContextSwitchTo(qctx);

	/* Deserialize the strings from the MPPEXEC statement. */
	Assert(stmt->ser_query != NULL && IsA(stmt->ser_query, String));
	cstr = strVal(stmt->ser_query);
	Assert(cstr != NULL);
	query = (Query *) deserializeNode(cstr);

    if (!query ||
        !IsA(query, Query))
        elog(ERROR, "MPPEXEC: received invalid query");
    if (query->commandType != CMD_UTILITY &&
        query->commandType != CMD_SELECT &&
        query->commandType != CMD_INSERT &&
        query->commandType != CMD_UPDATE &&
        query->commandType != CMD_DELETE)
        elog(ERROR, "MPPEXEC: received invalid query");

    pfree(cstr);

	/*
	 * Deserialize the slice table.
	 */
    if (stmt->ser_sliceinfo != NULL)
	{
		Assert(IsA(stmt->ser_sliceinfo, String));
		cstr = strVal(stmt->ser_sliceinfo);
		Assert(cstr != NULL);

        sliceTable = (SliceTable *) deserializeNode(cstr);

        if (!sliceTable ||
            !IsA(sliceTable, SliceTable) ||
            sliceTable->localSlice < 0 ||
            sliceTable->localSlice >= list_length(sliceTable->slices))
            elog(ERROR, "MPPEXEC: received invalid slice table");

        slice = (Slice *)list_nth(sliceTable->slices, sliceTable->localSlice);
        Insist(IsA(slice, Slice));

        pfree(cstr);

        /* Set global sliceid variable for elog. */
        currentSliceId = sliceTable->localSlice;

        /* Force commandType to SELECT if not a root slice. */
        if (sliceTable->localSlice != slice->rootIndex)
        {
            ListCell       *rtcell;
            RangeTblEntry  *rte;
            AclMode         removeperms = ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_SELECT_FOR_UPDATE;

            /* Just reading, so don't check INS/DEL/UPD permissions. */
            foreach(rtcell, query->rtable)
            {
                rte = (RangeTblEntry *)lfirst(rtcell);
                if (rte->rtekind == RTE_RELATION &&
                    0 != (rte->requiredPerms & removeperms))
                    rte->requiredPerms &= ~removeperms;
            }

            query->commandType = CMD_SELECT;
            query->into = NULL;
            query->resultRelation = 0;
            query->resultRelations = NIL;
            query->rowMarks = NIL;
        }
    }

    /*
     * Deserialize the query execution plan.
     */
    if (query->commandType != CMD_UTILITY)
	{
		Assert(stmt->ser_plan != NULL && IsA(stmt->ser_plan, String));
		cstr = strVal(stmt->ser_plan);
		Assert(cstr != NULL);
		plan = (Plan *) deserializeNode(cstr);
		pfree(cstr);

		/*
		 * Any parameters were serialized as RemoteParmData nodes.
		 * This is just a node wrapper around ParmListInfo.
		 */
		n = list_length(stmt->ser_params);
		if (n > 0)
		{
			paramLI = palloc((n + 1) * sizeof(ParamListInfoData));
			paramLI[n].kind = PARAM_INVALID;	/* Mark end of array */
			i = 0;
			foreach(pc, stmt->ser_params)
			{
				char	   *pszRemoteParmData;
				RemoteParamData *rpd;
				Node	   *ser_param;

				ser_param = lfirst(pc);
				Assert(IsA(ser_param, String));

				pszRemoteParmData = strVal(ser_param);

				rpd = (RemoteParamData *) deserializeNode(pszRemoteParmData);
				pfree(pszRemoteParmData);


				memcpy(&paramLI[i], &rpd->paramData, sizeof(ParamListInfoData));
				i++;
			}
		}

		/*
		 * Since we're running as a QE, we need to put need to the slice
		 * table and associated values determined by the QD that called
		 * MPPEXEC into the EState before we run the executor.	We can't
		 * do it now because our EState isn't ready.  Instead, stick it
		 * into the top plan node (along with nParamExec and nMotionNodes)
		 * and let the code that eventually sets up the QueryDesc sort it
		 * out.
		 */
		Assert(plan->sliceTable == NULL);		/* Accept no imposters. */
		plan->sliceTable = (Node *) sliceTable; /* Cache for CreateQueryDesc */
	}
	else
	{
		plan = NULL;
		paramLI = NULL;
	}

	/*
	 * Create a new portal to run the query in
	 */
	portal = CreateNewPortal();

	PortalDefineQuery(portal,
					  "",
					  CreateQueryTag(query),
					  list_make1(query),
					  list_make1(plan),
					  qctx);


	/*
	 * Start the portal.  Note that ExecutorStart is called in here for
	 * SELECT, but deferred until PortalRun for other DML.
	 */
	PortalStart(portal, paramLI, ActiveSnapshot);

	/*
	 * Run the portal to completion. */
	(void) PortalRun(portal, FETCH_ALL, dest, dest, completionTag);


	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* Restore state prior to return. */
	if (paramLI != NULL)
		pfree(paramLI);		/* Maybe should let context free it. */

	if (oldctx)
		MemoryContextSwitchTo(oldctx);

	return;
}
