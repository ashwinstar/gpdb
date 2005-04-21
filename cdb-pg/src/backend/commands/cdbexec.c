/*-------------------------------------------------------------------------
 *
 * cdbexec.c
 *	  The CDB command CDBEXEC (modelled on PostgreSQL EXECUTE)
 *
 * Copyright (c) 2002-2004, PostgreSQL Global Development Group
 * Copyright (c) 2005, Metapa Inc.
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


/*
 * Implements the 'CDBEXEC' utility statement.
 */
void
CdbExecQuery(CdbExecStmt *stmt, DestReceiver *dest, char *completionTag)
{
	Query *query = NULL;
	Plan *plan = NULL;
	ParamListInfo paramLI = NULL;
	MemoryContext oldctx = NULL, qctx;
	int i, n;
	ListCell *pc;
	
	/* tcop shouldn't call unless the role is right! */
	Assert(Cdb_role == CDB_ROLE_EXECUTE );
	
	/* TODO - Something smart with the memory context. */
	qctx = CurrentMemoryContext; /* Should prpbably be some portal context? */
	oldctx = MemoryContextSwitchTo(qctx);
	
	/* Deserialize the strings from the CDBEXEC statement. */
	Assert(stmt->ser_query != NULL );
	Assert(IsA(stmt->ser_query, String));
	char *pszQuery = strVal(stmt->ser_query);
	Assert(pszQuery != NULL );
	query = (Query *)deserializeNode(pszQuery);
	pfree(pszQuery);

	if (query->commandType != CMD_UTILITY)
	{
		Assert(stmt->ser_plan != NULL );
		Assert(IsA(stmt->ser_plan, String));
		char *pszPlan = strVal(stmt->ser_plan);
		Assert(pszPlan != NULL );
		plan = (Plan *)deserializeNode(pszPlan);
		pfree(pszPlan);

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
		Assert( query->commandType != CMD_UTILITY );
	}
	else
	{
		plan = NULL;
		elog(NOTICE, "Received a utility Command\n");
		/*ProcessUtility(query->utilityStmt,
			   NULL,
			   dest,
			   completionTag); 
		return */
	}

	EState	   *estate = NULL;
	/*Assert( query->commandType != CMD_UTILITY );*/

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
	 * Run the portal to completion.
	 */
	PortalStart(portal, paramLI, ActiveSnapshot);

	(void) PortalRun(portal, FETCH_ALL, dest, dest, completionTag);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	 /* Restore state prior to return. */
	if ( paramLI != NULL )
		pfree(paramLI); /* Maybe should let context free it. */

	 if ( oldctx )
		MemoryContextSwitchTo(oldctx);
	 
	 return;
}

