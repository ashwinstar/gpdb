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


/*
 * Implements the 'CDBEXEC' utility statement.
 */
void
CdbExecQuery(CdbExecStmt *stmt)
{
	Query *query = NULL;
	Plan *plan = NULL;
	ParamListInfo paramLI = NULL;
	MemoryContext oldctx = NULL, qctx;
	int i, n;
	ListCell *pc;
	Node *ser_param;
	
	/* tcop shouldn't call unless the role is right! */
	Assert(Cdb_role == CDB_ROLE_EXECUTE );
	
	/* TODO - Something smart with the memory context. */
	qctx = CurrentMemoryContext; /* Should prpbably be some portal context? */
	oldctx = MemoryContextSwitchTo(qctx);
	
	/* Deserialize the strings from the CDBEXEC statement. */
	
	/* TODO - Deserialize query, for now just show it. */
	query = NULL;
	elog(NOTICE, "CDBEXEC ser_query: %s", nodeToString(stmt->ser_query));
	
	/* TODO - Deserialize plan, for now just show it. */
	plan = NULL;
	elog(NOTICE, "CDBEXEC ser_plan: %s", nodeToString(stmt->ser_plan));

	/* TODO - Deserialize parameters, if any */
	n = list_length(stmt->ser_params);
	if ( n > 0)
	{
		paramLI = palloc((n+1) * sizeof(ParamListInfoData));
		paramLI[n].kind = PARAM_INVALID; /* Mark end of array */
		i = 0;
		foreach (pc, stmt->ser_params)
		{
			ser_param = lfirst(pc);
			/* TODO - Deserialize one parameter, for now show and invalidate. */
			elog(NOTICE, "CDBEXEC ser_params - %d: %s", i, nodeToString(ser_param));
			paramLI[i].kind = PARAM_INVALID;
			i++;
		}
	}

	/*
	 * TODO - Run the query, for now just issue error.
	 */
	 ereport(ERROR,
	 	( errcode(ERRCODE_CDB_INTERNAL_ERROR),
	 	  errmsg("The CDBEXEC command is under construction!"),
	 	  errdetail("Caller beware!")
	 	));
	 
	 /* Restore state prior to return. */
	 pfree(paramLI); /* Maybe should let context free it. */
	 if ( oldctx )
		MemoryContextSwitchTo(oldctx);
	 
	 return;
}

