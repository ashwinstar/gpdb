/*-------------------------------------------------------------------------
 *
 * cdbexec.c
 *	  The CDB command MPPEXEC (modelled on PostgreSQL EXECUTE)
 *
 * Copyright (c) 2002-2004, PostgreSQL Global Development Group
 * Copyright (c) 2005, Greenplum Inc.
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
CdbExecQuery(CdbExecStmt *stmt, DestReceiver *dest, char *completionTag)
{
	Query *query = NULL;
	Plan *plan = NULL;
	SliceTable *sliceTable = NULL;
	ParamListInfo paramLI = NULL;
	MemoryContext oldctx = NULL, qctx;
	int i, n;
	ListCell *pc;
	char *cstr = NULL;
	EState *estate = NULL;
	
	/* tcop shouldn't call unless the role is right! */
	Assert(Mpp_role == MPP_ROLE_EXECUTE );
	
	/* TODO - SLICE PLAN INTEGRATION 
	 * Do something smart with the memory context. 
	 */
	qctx = CurrentMemoryContext; /* Should probably be some portal context? */
	oldctx = MemoryContextSwitchTo(qctx);
	
	/* Deserialize the strings from the MPPEXEC statement. */
	Assert(stmt->ser_query != NULL && IsA(stmt->ser_query, String) );
	cstr = strVal(stmt->ser_query);
	Assert(cstr != NULL );
	query = (Query *)deserializeNode(cstr);
	pfree(cstr);
	
	if ( stmt->ser_sliceinfo != NULL )
	{
		Assert( IsA(stmt->ser_sliceinfo, String) );
		cstr = strVal(stmt->ser_sliceinfo);
		Assert(cstr != NULL);
		sliceTable = (SliceTable*)deserializeNode(cstr);
		pfree(cstr);
	}

	if (query->commandType != CMD_UTILITY)
	{
		Assert(stmt->ser_plan != NULL && IsA(stmt->ser_plan, String) );
		cstr = strVal(stmt->ser_plan);
		Assert(cstr != NULL );
		plan = (Plan *)deserializeNode(cstr);
		pfree(cstr);

		/*
		 * Any parameters were serialized as RemoteParmData nodes.
		 * This is just a node wrapper around ParmListInfo.
		 */
		n = list_length(stmt->ser_params);
		if ( n > 0)
		{
			paramLI = palloc((n+1) * sizeof(ParamListInfoData));
			paramLI[n].kind = PARAM_INVALID; /* Mark end of array */
			i = 0;
			foreach (pc, stmt->ser_params)
			{
				char *pszRemoteParmData;
				RemoteParamData *rpd;
				Node *ser_param;

				ser_param = lfirst(pc);
				Assert(IsA(ser_param, String));

				pszRemoteParmData = strVal(ser_param);

				rpd = (RemoteParamData *)deserializeNode(pszRemoteParmData);
				pfree( pszRemoteParmData );

				
				memcpy( &paramLI[i], &rpd->paramData, sizeof(ParamListInfoData) );
				i++;
			}
		}
		
		/* 
		 * Since we're running as a QE, we need to put need to the slice 
		 * table and associated values determined by the QD that called 
		 * MPPEXEC into the EState before we run the executor.  We can't 
		 * do it now because our EState isn't ready.  Instead, stick it
		 * into the top plan node (along with nParamExec and nMotionNodes)
		 * and let the code that eventually sets up the QueryDesc sort it
		 * out.
		 */
		Assert ( plan->sliceTable == NULL );  /* Accept no imposters. */
		plan->sliceTable = (Node*)sliceTable; /* Cache for CreateQueryDesc */
	}
	else
	{
		plan = NULL;
		paramLI = NULL;
		elog(DEBUG2, "mppexec received a utility command\n");
	}

 	{
	/*
	 * Create a new portal to run the query in
	 */
	List *query_list = list_make1(query);
	List *plan_list = list_make1(plan);
	const char *commandTag = CreateQueryTag(query);

	Portal		portal = CreateNewPortal();

	PortalDefineQuery(portal,
					  "",
					  commandTag,
					  query_list,
					  plan_list,
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
	if ( paramLI != NULL )
		pfree(paramLI); /* Maybe should let context free it. */
 	}

	 if ( oldctx )
		MemoryContextSwitchTo(oldctx);
	 
	 return;
}

