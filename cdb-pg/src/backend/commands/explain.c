/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  Explain query execution plans
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *    $Id$
 *	  $PostgreSQL: pgsql/src/backend/commands/explain.c,v 1.152 2006/10/04 00:29:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/prepare.h"
#include "commands/trigger.h"
#include "executor/instrument.h"
#include "nodes/print.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/utility.h"               /* CreateQueryTag() */
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"             /* AllocSetContextCreate() */

#include "cdb/cdbdisp.h"                /* CheckDispatchResult() */
#include "cdb/cdbexplain.h"             /* cdbexplain_recvExecStats */
#include "cdb/cdblink.h"                /* getgpsegmentCount() */
#include "cdb/cdbpullup.h"              /* cdbpullup_targetlist() */

typedef struct ExplainState
{
	List	   *rtable;			/* range table */

    /* CDB */
    struct CdbExplain_ShowStatCtx  *showstatctx;    /* EXPLAIN ANALYZE info */
    Slice          *currentSlice;   /* slice whose nodes we are visiting */
    ErrorData      *deferredError;  /* caught error to be re-thrown */
    MemoryContext   explaincxt;     /* mem pool for palloc()ing buffers etc. */
    TupOutputState *tupOutputState; /* for sending output to client */
	StringInfoData  outbuf;         /* the output buffer */
    StringInfoData  workbuf;        /* a scratch buffer */
} ExplainState;

static void ExplainOneQuery(Query *query, ExplainStmt *stmt,
				ParamListInfo params, TupOutputState *tstate);
static void
ExplainOnePlan_internal(QueryDesc      *queryDesc,
                        ExplainStmt    *stmt,
                        ExplainState   *es);
static double elapsed_time(instr_time *starttime);
static ErrorData *explain_defer_error(ExplainState *es);
static void explain_outNode(StringInfo str,
				Plan *plan, PlanState *planstate,
				Plan *outer_plan,
				int indent, ExplainState *es);
static void show_scan_qual(List *qual, const char *qlabel,
			   int scanrelid, Plan *outer_plan,
			   StringInfo str, int indent, ExplainState *es);
static void show_upper_qual(List *qual, const char *qlabel,
				const char *outer_name, int outer_varno, Plan *outer_plan,
				const char *inner_name, int inner_varno, Plan *inner_plan,
				StringInfo str, int indent, ExplainState *es);
static void show_sort_keys(Plan *sortplan, int nkeys, AttrNumber *keycols,
			   const char *qlabel,
			   StringInfo str, int indent, ExplainState *es);
static void
show_grouping_keys(Plan        *plan,
                   int          numCols,
                   AttrNumber  *subplanColIdx,
                   const char  *qlabel,
			       StringInfo str, int indent, ExplainState *es);
static void
show_motion_keys(Plan *plan, List *hashExpr, int nkeys, AttrNumber *keycols,
			     const char *qlabel,
                 StringInfo str, int indent, ExplainState *es);

/*
 * ExplainQuery -
 *	  execute an EXPLAIN command
 */
void
ExplainQuery(ExplainStmt *stmt, ParamListInfo params, DestReceiver *dest)
{
	Query	   *query = stmt->query;
	TupOutputState *tstate;
	List	   *rewritten;
	ListCell   *l;

	/*
	 * Because the planner is not cool about not scribbling on its input, we
	 * make a preliminary copy of the source querytree.  This prevents
	 * problems in the case that the EXPLAIN is in a portal or plpgsql
	 * function and is executed repeatedly.  (See also the same hack in
	 * DECLARE CURSOR and PREPARE.)  XXX the planner really shouldn't modify
	 * its input ... FIXME someday.
	 */
	query = copyObject(query);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, ExplainResultDesc(stmt));

	if (query->commandType == CMD_UTILITY)
	{
		/* Rewriter will not cope with utility statements */
		if (query->utilityStmt && IsA(query->utilityStmt, DeclareCursorStmt))
			ExplainOneQuery(query, stmt, params, tstate);
		else if (query->utilityStmt && IsA(query->utilityStmt, ExecuteStmt))
			ExplainExecuteQuery(stmt, params, tstate);
		else
			do_text_output_oneline(tstate, "Utility statements have no plan structure");
	}
	else
	{
		/*
		 * Must acquire locks in case we didn't come fresh from the parser.
		 * XXX this also scribbles on query, another reason for copyObject
		 */
		AcquireRewriteLocks(query);

		/* Rewrite through rule system */
		rewritten = QueryRewrite(query);

		if (rewritten == NIL)
		{
			/* In the case of an INSTEAD NOTHING, tell at least that */
			do_text_output_oneline(tstate, "Query rewrites to nothing");
		}
		else
		{
			/* Explain every plan */
			foreach(l, rewritten)
			{
				ExplainOneQuery(lfirst(l), stmt, params, tstate);
				/* put a blank line between plans */
				if (lnext(l) != NULL)
					do_text_output_oneline(tstate, "");
			}
		}
	}

	end_tup_output(tstate);
}

/*
 * ExplainResultDesc -
 *	  construct the result tupledesc for an EXPLAIN
 */
TupleDesc
ExplainResultDesc(ExplainStmt *stmt)
{
	TupleDesc	tupdesc;

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "QUERY PLAN",
					   TEXTOID, -1, 0);
	return tupdesc;
}

/*
 * ExplainOneQuery -
 *	  print out the execution plan for one query
 */
static void
ExplainOneQuery(Query *query, ExplainStmt *stmt, ParamListInfo params,
				TupOutputState *tstate)
{
	Plan	   *plan;
	QueryDesc  *queryDesc;
	bool		isCursor = false;
	int			cursorOptions = 0;

	/* planner will not cope with utility statements */
	if (query->commandType == CMD_UTILITY)
	{
		if (query->utilityStmt && IsA(query->utilityStmt, DeclareCursorStmt))
		{
			DeclareCursorStmt *dcstmt;
			List	   *rewritten;

			dcstmt = (DeclareCursorStmt *) query->utilityStmt;
			query = (Query *) dcstmt->query;
			isCursor = true;
			cursorOptions = dcstmt->options;
			/* Still need to rewrite cursor command */
			Assert(query->commandType == CMD_SELECT);
			/* get locks (we assume ExplainQuery already copied tree) */
			AcquireRewriteLocks(query);
			rewritten = QueryRewrite(query);
			if (list_length(rewritten) != 1)
				elog(ERROR, "unexpected rewrite result");
			query = (Query *) linitial(rewritten);
			Assert(query->commandType == CMD_SELECT);
			/* do not actually execute the underlying query! */
			stmt->analyze = false;
		}
		else if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
		{
			do_text_output_oneline(tstate, "NOTIFY");
			return;
		}
		else
		{
			do_text_output_oneline(tstate, "UTILITY");
			return;
		}
	}

	/* plan the query */
	plan = planner(query, isCursor, cursorOptions, params);
	
#ifdef USE_FORCE_PLAN
	/* if we force the plan, we will do substitution here */
	if(stmt->force)
	{
		/*
		 * First, get the string from the plan.  If we force, the plan must
		 * be SELECT 'parseTree', 'planTree'.  Sanity check 
		 */
		bool fErr = FALSE;
		char *parseStr = NULL;
		char *planStr = NULL;
		
		if(!IsA(plan,Result)
		    || plan->targetlist == NULL
		    || plan->targetlist->length != 2
		    || plan->qual
		    || plan->lefttree
		    || plan->righttree
		    || plan->initPlan
		    || plan->extParam 
		    || plan->allParam
		    || plan->nParamExec != 0
		    || plan->nMotionNodes != 0
		    || plan->nInitPlans != 0
		    || plan->sliceTable
		  )
			fErr = TRUE;

		/* The entry in the targelist must be 'parseTree', 'planTree' */
		if(fErr == FALSE)
		{
			ListCell *lc = NULL;
			void *node = NULL;
			Expr *expr = NULL;
			int i=0;
			foreach(lc, plan->targetlist)
			{
				Const *constval;
				
				node = lfirst(lc);
				Assert(IsA(node, TargetEntry));
				expr = ((TargetEntry*)node)->expr;
				Assert(expr != NULL);

				constval = (Const *)expr;
				
				if(!IsA(expr,Const))
				{
					fErr = TRUE;
					break;
				}

				if(constval->constisnull || constval->constbyval)
				{
					fErr = TRUE;
					break;
				}

				switch(i)
				{
					case 0:
						parseStr = (char*) DatumGetPointer(constval->constvalue);			
						break;
					case 1:
						planStr = (char*) DatumGetPointer(constval->constvalue);
						break;
					default:
						fErr = TRUE;
						break;
				}
				++i;
			}
		}

		if(fErr == TRUE || parseStr == NULL || planStr == NULL)
		{
			ereport(ERROR, 
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXPLAIN FORCE expects a SELECT 'parseTree', 'planTree'.")
				));
		}
		else
		{
			/* plan substitution */
			query = stringToNode(parseStr);
			plan = stringToNode(planStr);

			/* Zap the slice table.  In ExplainOnePlan, we will slice the plan.
			 * The most common case is we will try to load a plan from a previous
			 * explain verbose.  The output has a slice table, which will cause 
			 * trouble in ExplainOnePlan when we slice it again.
			 */
			pfree(plan->sliceTable);
			plan->sliceTable = NULL;
		}
	}	
#endif /* USE_FORCE_PLAN*/

	/*
	 * Update snapshot command ID to ensure this query sees results of any
	 * previously executed queries.  (It's a bit cheesy to modify
	 * ActiveSnapshot without making a copy, but for the limited ways in which
	 * EXPLAIN can be invoked, I think it's OK, because the active snapshot
	 * shouldn't be shared with anything else anyway.)
	 */
	ActiveSnapshot->curcid = GetCurrentCommandId();

	/* Create a QueryDesc requesting no output */
	queryDesc = CreateQueryDesc(query, plan,
								ActiveSnapshot, InvalidSnapshot,
								None_Receiver, params,
								stmt->analyze);

	ExplainOnePlan(queryDesc, stmt, tstate);
}

/*
 * ExplainOnePlan -
 *		given a planned query, execute it if needed, and then print
 *		EXPLAIN output
 *
 * This is exported because it's called back from prepare.c in the
 * EXPLAIN EXECUTE case
 *
 * Note: the passed-in QueryDesc is freed when we're done with it
 */
void
ExplainOnePlan(QueryDesc *queryDesc, ExplainStmt *stmt,
			   TupOutputState *tstate)
{
    MemoryContext   oldcxt = CurrentMemoryContext;
    MemoryContext   explaincxt;
	ExplainState    explainState;
    ExplainState   *es = &explainState;

    /* Create EXPLAIN memory context. */
    explaincxt = AllocSetContextCreate(CurrentMemoryContext,
                                       "EXPLAIN working storage",
                                       ALLOCSET_DEFAULT_MINSIZE,
                                       ALLOCSET_DEFAULT_INITSIZE,
                                       ALLOCSET_DEFAULT_MAXSIZE);

    /* Initialize ExplainState structure. */
    memset(es, 0, sizeof(*es));
    es->explaincxt = explaincxt;
    es->showstatctx = NULL;
    es->deferredError = NULL;
    es->tupOutputState = tstate;

    /* Allocate output buffer and a scratch buffer. */
    MemoryContextSwitchTo(explaincxt);
    initStringInfoOfSize(&es->workbuf, 1000);
	initStringInfoOfSize(&es->outbuf, 16000);
    MemoryContextSwitchTo(oldcxt);

    /* Do the EXPLAIN. */
    PG_TRY();
    {
        ExplainOnePlan_internal(queryDesc, stmt, es);
    }
    PG_CATCH();
    {
        /* Free the memory we used. */
        if (CurrentMemoryContext == explaincxt)
            MemoryContextSwitchTo(oldcxt);
        MemoryContextDelete(explaincxt);

        /* Exit to next error handler. */
        PG_RE_THROW();
    }
    PG_END_TRY();

    /* Free the memory we used. */
    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(explaincxt);
}                               /* ExplainOnePlan */

void
ExplainOnePlan_internal(QueryDesc      *queryDesc,
                        ExplainStmt    *stmt,
                        ExplainState   *es)
{
    MemoryContext   oldcxt = CurrentMemoryContext;
	instr_time	starttime;
	double		totaltime = 0;
    StringInfo  buf = &es->outbuf;
    EState     *estate = NULL;
	int			eflags;
    int         nb;

    /*
     * Start timing.
     */
    INSTR_TIME_SET_CURRENT(starttime);

	/* If analyzing, we need to cope with queued triggers */
	if (stmt->analyze)
        AfterTriggerBeginQuery();

    /* Allocate workarea for summary stats. */
    if (stmt->analyze)
    {
        es->showstatctx = cdbexplain_showExecStatsBegin(queryDesc,
                                                        es->explaincxt,
                                                        starttime);

        /* Attach workarea to QueryDesc so ExecSetParamPlan() can find it. */
        queryDesc->showstatctx = es->showstatctx;
    }

	/* Select execution options */
	if (stmt->analyze)
        eflags = 0;				/* default run-to-completion flags */
	else
		eflags = EXEC_FLAG_EXPLAIN_ONLY;

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, eflags);

    estate = queryDesc->estate;

    /* CDB: Find slice table entry for the root slice. */
    es->currentSlice = getCurrentSlice(estate, estate->es_cur_slice_idx);

    /*
     * Execute the plan for statistics if asked for.  Attempt to proceed
     * with our report even if there is an error.
     */
	if (stmt->analyze)
	{
		/* run the plan */
        PG_TRY();
        {
		    ExecutorRun(queryDesc, ForwardScanDirection, 0L);
        }
        PG_CATCH();
        {
            es->deferredError = explain_defer_error(es);
        }
        PG_END_TRY();

        /* Wait for completion of all qExec processes. */
        PG_TRY();
        {
            if (estate->primaryResults)
                CdbCheckDispatchResult(estate->dispatchThreads, false);	
        }
        PG_CATCH();
        {
            es->deferredError = explain_defer_error(es);
        }
        PG_END_TRY();

        /* Suspend timing. */
	    totaltime += elapsed_time(&starttime);

        /* Get local stats if root slice was executed here in the qDisp. */
        if (!es->currentSlice ||
            sliceRunsOnQD(es->currentSlice))
            cdbexplain_localExecStats(queryDesc->planstate, es->showstatctx);

        /* Fill in the plan's Instrumentation with stats from qExecs. */
        if (estate->primaryResults)
            cdbexplain_recvExecStats(queryDesc->planstate,
                                     estate->primaryResults,
                                     estate->es_cur_slice_idx,
                                     es->showstatctx);
	}

	es->rtable = queryDesc->parsetree->rtable;

    if (stmt->verbose)
	{
		char	   *s;
		char	   *f;

#ifdef USE_FORCE_PLAN
		// First print parse tree.
		s = nodeToString(queryDesc->parsetree);
		if(s)
		{
			if (Explain_pretty_print)
				f = pretty_format_node_dump(s);
			else
				f = format_node_dump(s);
			pfree(s);
			do_text_output_multiline(es->tupOutputState, f);
			pfree(f);
			do_text_output_oneline(es->tupOutputState, ""); /* separator line */
		}
#endif /* USE_FORCE_PLAN */

        /* Switch to EXPLAIN memory context. */
        MemoryContextSwitchTo(es->explaincxt);

		if (queryDesc->plantree && estate->es_sliceTable)
		{
        	Node   *saved_es_sliceTable;

			/* Little two-step to get EXPLAIN VERBOSE to show slice table. */
			saved_es_sliceTable = queryDesc->plantree->sliceTable;		/* probably NULL */
			queryDesc->plantree->sliceTable = (Node *) queryDesc->estate->es_sliceTable;
			s = nodeToString(queryDesc->plantree);
			queryDesc->plantree->sliceTable = saved_es_sliceTable;
		}
		else
		{
			s = nodeToString(queryDesc->plantree);
		}

		if (s)
		{
			if (Explain_pretty_print)
				f = pretty_format_node_dump(s);
			else
				f = format_node_dump(s);
			pfree(s);

            /* Restore caller's memory context. */
            MemoryContextSwitchTo(oldcxt);

			do_text_output_multiline(es->tupOutputState, f);
			pfree(f);
			do_text_output_oneline(es->tupOutputState, ""); /* separator line */
		}

        /* Restore caller's memory context. */
        MemoryContextSwitchTo(oldcxt);
	}

    /*
     * Produce the EXPLAIN report into buf.  (Sometimes we get internal errors
     * while doing this; try to proceed with a partial report anyway.)
     */
    PG_TRY();
    {
	    explain_outNode(buf, queryDesc->plantree, queryDesc->planstate,
					    NULL, 0, es);
    }
    PG_CATCH();
    {
        es->deferredError = explain_defer_error(es);

        /* Keep a NUL at the end of the output buffer. */
        buf->data[Min(buf->len, buf->maxlen-1)] = '\0';
    }
    PG_END_TRY();

	/*
	 * If we ran the command, run any AFTER triggers it queued.  (Note this
	 * will not include DEFERRED triggers; since those don't run until end of
	 * transaction, we can't measure them.)  Include into total runtime.
     * Skip triggers if there has been an error.
	 */
	if (stmt->analyze &&
        !es->deferredError)
	{
		ResultRelInfo *rInfo;
		int			numrels = queryDesc->estate->es_num_result_relations;
		int			nr;

		INSTR_TIME_SET_CURRENT(starttime);
		AfterTriggerEndQuery(queryDesc->estate);
		totaltime += elapsed_time(&starttime);

	    /* Print info about runtime of triggers */
		rInfo = queryDesc->estate->es_result_relations;
		for (nr = 0; nr < numrels; rInfo++, nr++)
		{
			int			nt;

			if (!rInfo->ri_TrigDesc || !rInfo->ri_TrigInstrument)
				continue;
			for (nt = 0; nt < rInfo->ri_TrigDesc->numtriggers; nt++)
			{
				Trigger    *trig = rInfo->ri_TrigDesc->triggers + nt;
				Instrumentation *instr = rInfo->ri_TrigInstrument + nt;
				char	   *conname;

				/* Must clean up instrumentation state */
				InstrEndLoop(instr);

				/*
				 * We ignore triggers that were never invoked; they likely
				 * aren't relevant to the current query type.
				 */
				if (instr->ntuples == 0)
					continue;

				if (trig->tgisconstraint &&
				(conname = GetConstraintNameForTrigger(trig->tgoid)) != NULL)
				{
					appendStringInfo(buf, "Trigger for constraint %s",
									 conname);
					pfree(conname);
				}
				else
					appendStringInfo(buf, "Trigger %s", trig->tgname);

				if (numrels > 1)
					appendStringInfo(buf, " on %s",
							RelationGetRelationName(rInfo->ri_RelationDesc));

				appendStringInfo(buf, ": time=%.3f calls=%.0f\n",
								 1000.0 * instr->total,
								 instr->ntuples);
			}
		}
	}

    /*
     * Display per-slice and whole-query statistics.
     */
    if (stmt->analyze)
        cdbexplain_showExecStatsEnd(es->showstatctx, buf, estate);

    /*
     * Show non-default GUC settings that might have affected the plan.
     */
    nb = gp_guc_list_show(buf, "Settings:  ", "%s=%s; ", PGC_S_DEFAULT,
                           gp_guc_list_for_explain);
    if (nb > 0)
    {
        truncateStringInfo(buf, buf->len - 2);  /* drop final "; " */
        appendStringInfoChar(buf, '\n');
    }

    /*
     * Display final elapsed time.
     */
	if (stmt->analyze)
		appendStringInfo(buf, "Total runtime: %.3f ms\n",
						 1000.0 * totaltime);

    /*
     * Send EXPLAIN report to client.  Some might have been sent already
     * by explain_outNode().
     */
    if (buf->len > 0)
        do_text_output_multiline(es->tupOutputState, buf->data);

    /*
	 * Close down the query and free resources.
     *
     * We don't have to pfree() our buffers because the caller frees them by
     * deleting explaincxt.
     *
     * For EXPLAIN ANALYZE, if a qExec failed or gave an error, ExecutorEnd()
     * will reissue the error locally at this point.  Intercept any such error
     * and reduce it to a NOTICE so it won't interfere with our output.
	 */
    PG_TRY();
    {
	    ExecutorEnd(queryDesc);
    }
    PG_CATCH();
    {
        es->deferredError = explain_defer_error(es);
    }
    PG_END_TRY();

    /*
     * If we intercepted an error, now's the time to re-throw it.
     * Although we have marked it as a NOTICE instead of an ERROR,
     * it will still get the same error handling and cleanup treatment.
     *
     * We must call EndCommand() to send a successful completion response;
     * otherwise libpq clients just discard the nice report they have received.
     * Oddly, the NOTICE will be sent *after* the success response; that
     * should be good enough for now.
     */
    if (es->deferredError)
    {
        ErrorData  *edata = es->deferredError;

        /* Tell client the command ended successfully. */
        EndCommand("EXPLAIN", es->tupOutputState->dest->mydest);

        /* Resume handling the error.  Clean up and send the NOTICE message. */
        es->deferredError = NULL;
        ReThrowError(edata);
    }

    FreeQueryDesc(queryDesc);

	/* We need a CCI just in case query expanded to multiple plans */
	if (stmt->analyze)
		CommandCounterIncrement();
}                               /* ExplainOnePlan_internal */

/* Compute elapsed time in seconds since given timestamp */
static double
elapsed_time(instr_time *starttime)
{
	instr_time	endtime;

	INSTR_TIME_SET_CURRENT(endtime);
    InstrTimeSubtract(&endtime, &endtime, starttime);
	return INSTR_TIME_GET_DOUBLE(endtime);
}


/*
 * explain_defer_error
 *    Called within PG_CATCH handler to demote and save the current error.
 *
 * We'll try to postpone the error cleanup until after we have produced
 * the EXPLAIN ANALYZE report, and then reflect the error to the client as
 * merely a NOTICE (because an ERROR causes libpq clients to discard the
 * report).
 *
 * If successful, upon return we fall thru the bottom of the PG_CATCH
 * handler and continue sequentially.  Otherwise we re-throw to the
 * next outer error handler.
 */
ErrorData *
explain_defer_error(ExplainState *es)
{
    ErrorData  *edata;

    /* Already saved an earlier error?  Rethrow it now. */
    if (es->deferredError)
        ReThrowError(es->deferredError);    /* does not return */

    /* Try to downgrade the error to a NOTICE.  Rethrow if disallowed. */
    if (!elog_demote(NOTICE))
        PG_RE_THROW();

    /* Save the error info and expunge it from the error system. */
    MemoryContextSwitchTo(es->explaincxt);
    edata = CopyErrorData();
    FlushErrorState();

    /* Caller must eventually ReThrowError() for proper cleanup. */
    return edata;
}                               /* explain_defer_error */


/*
 * explain_outNode -
 *	  converts a Plan node into ascii string and appends it to 'str'
 *
 * planstate points to the executor state node corresponding to the plan node.
 * We need this to get at the instrumentation data (if any) as well as the
 * list of subplans.
 *
 * outer_plan, if not null, references another plan node that is the outer
 * side of a join with the current node.  This is only interesting for
 * deciphering runtime keys of an inner indexscan.
 */
static void
explain_outNode(StringInfo str,
				Plan *plan, PlanState *planstate,
				Plan *outer_plan,
				int indent, ExplainState *es)
{
	char	   *pname = NULL;
    Slice      *currentSlice = es->currentSlice;    /* save */
	int			i;
	bool		skip_outer=false;

	if (plan == NULL)
	{
		appendStringInfoChar(str, '\n');
		return;
	}

	switch (nodeTag(plan))
	{
		case T_Result:
			pname = "Result";
			break;
		case T_Append:
			pname = "Append";
			break;
		case T_BitmapAnd:
			pname = "BitmapAnd";
			break;
		case T_BitmapOr:
			pname = "BitmapOr";
			break;
		case T_AdaptiveNestLoop:
			pname = "Adaptive Join Choice";
			break;
		case T_NestLoop:
			if (((NestLoop *)plan)->shared_outer)
				skip_outer = true;
			switch (((NestLoop *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Nested Loop";
					break;
				case JOIN_LEFT:
					pname = "Nested Loop Left Join";
					break;
				case JOIN_FULL:
					pname = "Nested Loop Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Nested Loop Right Join";
					break;
				case JOIN_IN:
					pname = "Nested Loop EXISTS Join";
					break;
				default:
					pname = "Nested Loop ??? Join";
					break;
			}
			break;
		case T_MergeJoin:
			switch (((MergeJoin *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Merge Join";
					break;
				case JOIN_LEFT:
					pname = "Merge Left Join";
					break;
				case JOIN_FULL:
					pname = "Merge Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Merge Right Join";
					break;
				case JOIN_IN:
					pname = "Merge EXISTS Join";
					break;
				default:
					pname = "Merge ??? Join";
					break;
			}
			break;
		case T_HashJoin:
			switch (((HashJoin *) plan)->join.jointype)
			{
				case JOIN_INNER:
					pname = "Hash Join";
					break;
				case JOIN_LEFT:
					pname = "Hash Left Join";
					break;
				case JOIN_FULL:
					pname = "Hash Full Join";
					break;
				case JOIN_RIGHT:
					pname = "Hash Right Join";
					break;
				case JOIN_IN:
					pname = "Hash EXISTS Join";
					break;
				default:
					pname = "Hash ??? Join";
					break;
			}
			break;
		case T_SeqScan:
			pname = "Seq Scan";
			break;
		case T_ExternalScan:
			pname = "External Scan";
			break;						
		case T_IndexScan:
			pname = "Index Scan";
			break;
		case T_BitmapIndexScan:
			pname = "Bitmap Index Scan";
			break;
		case T_BitmapHeapScan:
			pname = "Bitmap Heap Scan";
			break;
		case T_TidScan:
			pname = "Tid Scan";
			break;
		case T_SubqueryScan:
			pname = "Subquery Scan";
			break;
		case T_FunctionScan:
			pname = "Function Scan";
			break;
		case T_ValuesScan:
			pname = "Values Scan";
			break;
		case T_ShareInputScan:
			appendStringInfo(str, "ShareInputScan (shares %d)", plan->underlying_share_id);
			pname = "";
			break;
		case T_Material:
			pname = "Materialize";
			if(plan->shared_as_id >= 0)
			{
				appendStringInfo(str, "%s (shared as %d)", pname, plan->shared_as_id);
				pname = "";
			}
			break;
		case T_Sort:
			pname = "Sort";
			if(plan->shared_as_id >= 0)
			{
				appendStringInfo(str, "%s (shared as %d)", pname, plan->shared_as_id);
				pname = "";
			}
			break;
		case T_Group:
			pname = "Group";
			break;
		case T_Agg:
			switch (((Agg *) plan)->aggstrategy)
			{
				case AGG_PLAIN:
					pname = "Aggregate";
					break;
				case AGG_SORTED:
					pname = "GroupAggregate";
					break;
				case AGG_HASHED:
					pname = "HashAggregate";
					break;
				default:
					pname = "Aggregate ???";
					break;
			}
			break;
		case T_Window:
			pname = "Window";
			break;
		case T_Unique:
			pname = "Unique";
			break;
		case T_SetOp:
			switch (((SetOp *) plan)->cmd)
			{
				case SETOPCMD_INTERSECT:
					pname = "SetOp Intersect";
					break;
				case SETOPCMD_INTERSECT_ALL:
					pname = "SetOp Intersect All";
					break;
				case SETOPCMD_EXCEPT:
					pname = "SetOp Except";
					break;
				case SETOPCMD_EXCEPT_ALL:
					pname = "SetOp Except All";
					break;
				default:
					pname = "SetOp ???";
					break;
			}
			break;
		case T_Limit:
			pname = "Limit";
			break;
		case T_Hash:
			pname = "Hash";
			break;
		case T_Motion:
			{
				Motion	   *pMotion = (Motion *) plan;
                int         nSenders = pMotion->numInputSegs;
				int 		nSlices = list_length(pMotion->sliceEntries);

				int         nReceivers; 
				bool        senderIsEntryDb = false;

				if(nSlices == 1)
				{
					MotionSliceEntry *slice = (MotionSliceEntry *) linitial(pMotion->sliceEntries);

					switch (slice->motionType)
					{
						case MOTIONTYPE_HASH:
							nReceivers = slice->numHashSegs;
							pname = "Redistribute Motion";
							break;
						case MOTIONTYPE_FIXED:
							nReceivers = slice->numOutputSegs;
							if (nReceivers == 0)
							{
								pname = "Broadcast Motion";
								nReceivers = getgpsegmentCount();
							}
							else
								pname = "Gather Motion";
							break;
						default:
							pname = "Motion ???";
							break;
					}
				}
				else
					pname = "Multi Slice Motion";

				/*
				 * Only print x:y for single slice motion.  
				 * XXX TODO: ftian: find a better way to display multi sliced motion
				 */
				if (nSenders == 1 && nSlices == 1 && 
						plan->lefttree &&
						plan->lefttree->flow &&
						plan->lefttree->flow->flotype == FLOW_SINGLETON &&
						plan->lefttree->flow->segindex == -1)
					senderIsEntryDb = true;
				appendStringInfo(str, "%s %d:%d  (slice%d%s)", pname,
						nSenders, nReceivers, pMotion->motionID,
						senderIsEntryDb ? " on entry db" : "");
				pname = "";
			}

			break;
		default:
			pname = "???";
			break;
	}

	appendStringInfoString(str, pname);
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			if (ScanDirectionIsBackward(((IndexScan *) plan)->indexorderdir))
				appendStringInfoString(str, " Backward");
			appendStringInfo(str, " using %s",
			  quote_identifier(get_rel_name(((IndexScan *) plan)->indexid)));
			/* FALL THRU */
		case T_SeqScan:
		case T_ExternalScan:
		case T_BitmapHeapScan:
		case T_TidScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *relname;

				/* Assume it's on a real relation */
				Assert(rte->rtekind == RTE_RELATION);

				/* We only show the rel name, not schema name */
				relname = get_rel_name(rte->relid);

				appendStringInfo(str, " on %s",
								 quote_identifier(relname));
				if (strcmp(rte->eref->aliasname, relname) != 0)
					appendStringInfo(str, " %s",
									 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_BitmapIndexScan:
			appendStringInfo(str, " on %s",
							 quote_identifier(get_rel_name(((BitmapIndexScan *) plan)->indexid)));
			break;
		case T_SubqueryScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);

				appendStringInfo(str, " %s",
								 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_FunctionScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *proname;

				/* Assert it's on a RangeFunction */
				Assert(rte->rtekind == RTE_FUNCTION);

				/*
				 * If the expression is still a function call, we can get the
				 * real name of the function.  Otherwise, punt (this can
				 * happen if the optimizer simplified away the function call,
				 * for example).
				 */
				if (rte->funcexpr && IsA(rte->funcexpr, FuncExpr))
				{
					FuncExpr   *funcexpr = (FuncExpr *) rte->funcexpr;
					Oid			funcid = funcexpr->funcid;

					/* We only show the func name, not schema name */
					proname = get_func_name(funcid);
				}
				else
					proname = rte->eref->aliasname;

				appendStringInfo(str, " on %s",
								 quote_identifier(proname));
				if (strcmp(rte->eref->aliasname, proname) != 0)
					appendStringInfo(str, " %s",
									 quote_identifier(rte->eref->aliasname));
			}
			break;
		case T_ValuesScan:
			if (((Scan *) plan)->scanrelid > 0)
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid,
											  es->rtable);
				char	   *valsname;

				/* Assert it's on a values rte */
				Assert(rte->rtekind == RTE_VALUES);

				valsname = rte->eref->aliasname;

				appendStringInfo(str, " on %s",
								 quote_identifier(valsname));
			}
			break;
		case T_Material:
			if (((Material *) plan)->cdb_strict)
				appendStringInfo(str, " for deadlock safety");
			break;
		default:
			break;
	}

	appendStringInfo(str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
					 plan->startup_cost, plan->total_cost,
					 plan->plan_rows, plan->plan_width);
    appendStringInfoChar(str, '\n');

    /* quals, sort keys, etc */
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			show_scan_qual(((IndexScan *) plan)->indexqualorig,
						   "Index Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			show_scan_qual(plan->qual,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_BitmapIndexScan:
			show_scan_qual(((BitmapIndexScan *) plan)->indexqualorig,
						   "Index Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_BitmapHeapScan:
			/* XXX do we want to show this in production? */
			show_scan_qual(((BitmapHeapScan *) plan)->bitmapqualorig,
						   "Recheck Cond",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			/* FALL THRU */
		case T_SeqScan:
		case T_ExternalScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
			show_scan_qual(plan->qual,
						   "Filter",
						   ((Scan *) plan)->scanrelid,
						   outer_plan,
						   str, indent, es);
			break;
		case T_TidScan:
			{
				/*
				 * The tidquals list has OR semantics, so be sure to show it
				 * as an OR condition.
				 */
				List	   *tidquals = ((TidScan *) plan)->tidquals;

				if (list_length(tidquals) > 1)
					tidquals = list_make1(make_orclause(tidquals));
				show_scan_qual(tidquals,
							   "TID Cond",
							   ((Scan *) plan)->scanrelid,
							   outer_plan,
							   str, indent, es);
				show_scan_qual(plan->qual,
							   "Filter",
							   ((Scan *) plan)->scanrelid,
							   outer_plan,
							   str, indent, es);
			}
			break;
		case T_NestLoop:
			show_upper_qual(((NestLoop *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_MergeJoin:
			show_upper_qual(((MergeJoin *) plan)->mergeclauses,
							"Merge Cond",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(((MergeJoin *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_HashJoin:
			show_upper_qual(((HashJoin *) plan)->hashclauses,
							"Hash Cond",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(((HashJoin *) plan)->join.joinqual,
							"Join Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							"outer", OUTER, outerPlan(plan),
							"inner", INNER, innerPlan(plan),
							str, indent, es);
			break;
		case T_AdaptiveNestLoop:
	        for (i = 0; i < indent; i++)
		        appendStringInfo(str, "  ");
            appendStringInfo(str, "  Hash Join will be chosen if outer row count >= %d\n",
							 ((AdaptiveNestLoop *)plan)->nl_strategy_threshold);
			break;
		case T_Agg:
			show_upper_qual(plan->qual,
							"Filter",
							NULL, 0, outerPlan(plan),
							NULL, 0, NULL,
							str, indent, es);
			show_grouping_keys(plan,
						       ((Agg *) plan)->numCols,
						       ((Agg *) plan)->grpColIdx,
						       "Group By",
						       str, indent, es);
			break;
		case T_Group:
			show_upper_qual(plan->qual,
							"Filter",
							"subplan", 0, outerPlan(plan),
							NULL, 0, NULL,
							str, indent, es);
			show_grouping_keys(plan,
						       ((Group *) plan)->numCols,
						       ((Group *) plan)->grpColIdx,
						       "Group By",
						       str, indent, es);
			break;
		case T_Window:
			{
				Window *window = (Window *)plan;
				ListCell *cell;
				char orderKeyStr[32]; /* XXX big enough */
				int i;
				
				if ( window->numPartCols > 0 )
				{
					show_grouping_keys(outerPlan(plan),
									   window->numPartCols,
									   window->partColIdx,
									   "Partition By",
									   str, indent, es);
				}

				if (list_length(window->windowKeys) > 1)
					i = 0;
				else
					i = -1;
					
				foreach(cell, window->windowKeys)
				{
					WindowKey *key = (WindowKey *) lfirst(cell);
					
					if ( i < 0 )
						sprintf(orderKeyStr, "Order By");
					else
					{
						sprintf(orderKeyStr, "Order By (level %d)", ++i);
					}
					
					show_sort_keys(outerPlan(plan),
								   key->numSortCols,
								   key->sortColIdx,
								   orderKeyStr,
								   str, indent, es);
				}
				/* XXX don't show framing for now */
			}
			break;
		case T_Unique:
			show_motion_keys(plan,
                             NIL,
						     ((Unique *) plan)->numCols,
						     ((Unique *) plan)->uniqColIdx,
						     "Group By",
						     str, indent, es);
			break;
		case T_Sort:
		{
			bool bLimit = (((Sort *) plan)->limitCount
						   || ((Sort *) plan)->limitOffset);
			
			bool bNoDup = ((Sort *) plan)->noduplicates;

			char *SortKeystr = "Sort Key";

			if ((bLimit && bNoDup))
				SortKeystr = "Sort Key (Limit Distinct)";
			else if (bLimit)
				SortKeystr = "Sort Key (Limit)";
			else if (bNoDup)
				SortKeystr = "Sort Key (Distinct)";

			show_sort_keys(plan,
						   ((Sort *) plan)->numCols,
						   ((Sort *) plan)->sortColIdx,
						   SortKeystr,
						   str, indent, es);
		}
			break;
		case T_Result:
			show_upper_qual((List *) ((Result *) plan)->resconstantqual,
							"One-Time Filter",
							NULL, OUTER, outerPlan(plan),
							NULL, 0, NULL,
							str, indent, es);
			show_upper_qual(plan->qual,
							"Filter",
							NULL, OUTER, outerPlan(plan),
							NULL, 0, NULL,
							str, indent, es);
			break;
		case T_Motion:
			{
				Motion	   *pMotion = (Motion *) plan;
                SliceTable *sliceTable = planstate->state->es_sliceTable;

				/* 
				 * Only show merge key or hash key if this motion have one slice and one route
				 * XXX TODO: ftian: Find a better way to explain motion
				 */
				if (list_length(pMotion->sliceEntries) == 1)
				{
					MotionSliceEntry *sliceEntry = (MotionSliceEntry *) linitial(pMotion->sliceEntries);
					if (pMotion->sendSorted || sliceEntry->motionType == MOTIONTYPE_HASH)
						show_motion_keys(plan,
								sliceEntry->hashExpr,
								pMotion->numSortCols,
								pMotion->sortColIdx,
								"Merge Key",
								str, indent, es);
				}

                /* Descending into a new slice. */
                if (sliceTable)
                    es->currentSlice = (Slice *)list_nth(sliceTable->slices,
                                                         pMotion->motionID);
			}
			break;
		default:
			break;
	}

    /* CDB: Show actual row count, etc. */
	if (planstate->instrument)
        cdbexplain_showExecStats(planstate,
                                 str,
                                 indent+1,
                                 es->showstatctx);

	/* initPlan-s */
	if (plan->initPlan)
	{
        Slice      *saved_slice = es->currentSlice;
		List	   *saved_rtable = es->rtable;
		ListCell   *lst;

		foreach(lst, planstate->initPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lst);
			SubPlan    *sp = (SubPlan *) sps->xprstate.expr;
            SliceTable *sliceTable = planstate->state->es_sliceTable;

            /* Subplan might have its own root slice */
            if (sliceTable &&
                sp->qDispSliceId > 0)
                es->currentSlice = (Slice *)list_nth(sliceTable->slices,
                                                     sp->qDispSliceId);

		    for (i = 0; i < indent; i++)
			    appendStringInfo(str, "  ");
		    appendStringInfo(str, "  InitPlan  (slice%d)\n", sp->qDispSliceId);

            es->rtable = sp->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, sp->plan,
							sps->planstate,
							NULL,
							indent + 4, es);
		}
        es->currentSlice = saved_slice;
		es->rtable = saved_rtable;
	}

	/* lefttree */
	if (outerPlan(plan) && !skip_outer)
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");

		/*
		 * Ordinarily we don't pass down our own outer_plan value to our child
		 * nodes, but in bitmap scan trees we must, since the bottom
		 * BitmapIndexScan nodes may have outer references.
		 */
		explain_outNode(str, outerPlan(plan),
						outerPlanState(planstate),
						IsA(plan, BitmapHeapScan) ? outer_plan : NULL,
						indent + 3, es);
	}
    else if (skip_outer)
    {
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		appendStringInfo(str, "See first subplan of Hash Join\n");
    }

	/* righttree */
	if (innerPlan(plan))
	{
		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");
		explain_outNode(str, innerPlan(plan),
						innerPlanState(planstate),
						outerPlan(plan),
						indent + 3, es);
	}

	if (IsA(plan, Append))
	{
		Append	   *appendplan = (Append *) plan;
		AppendState *appendstate = (AppendState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, appendplan->appendplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			/*
			 * Ordinarily we don't pass down our own outer_plan value to our
			 * child nodes, but in an Append we must, since we might be
			 * looking at an appendrel indexscan with outer references from
			 * the member scans.
			 */
			explain_outNode(str, subnode,
							appendstate->appendplans[j],
							outer_plan,
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, BitmapAnd))
	{
		BitmapAnd  *bitmapandplan = (BitmapAnd *) plan;
		BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, bitmapandplan->bitmapplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			explain_outNode(str, subnode,
							bitmapandstate->bitmapplans[j],
							outer_plan, /* pass down same outer plan */
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, BitmapOr))
	{
		BitmapOr   *bitmaporplan = (BitmapOr *) plan;
		BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
		ListCell   *lst;
		int			j;

		j = 0;
		foreach(lst, bitmaporplan->bitmapplans)
		{
			Plan	   *subnode = (Plan *) lfirst(lst);

			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "  ->  ");

			explain_outNode(str, subnode,
							bitmaporstate->bitmapplans[j],
							outer_plan, /* pass down same outer plan */
							indent + 3, es);
			j++;
		}
	}

	if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subqueryscan = (SubqueryScan *) plan;
		SubqueryScanState *subquerystate = (SubqueryScanState *) planstate;
		Plan	   *subnode = subqueryscan->subplan;
		RangeTblEntry *rte = rt_fetch(subqueryscan->scan.scanrelid,
									  es->rtable);
		List	   *saved_rtable = es->rtable;

		Assert(rte->rtekind == RTE_SUBQUERY);
		es->rtable = rte->subquery->rtable;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  ->  ");

		explain_outNode(str, subnode,
						subquerystate->subplan,
						NULL,
						indent + 3, es);

		es->rtable = saved_rtable;
	}

	/* subPlan-s */
	if (planstate->subPlan)
	{
		List	   *saved_rtable = es->rtable;
		ListCell   *lst;

		for (i = 0; i < indent; i++)
			appendStringInfo(str, "  ");
		appendStringInfo(str, "  SubPlan\n");
		foreach(lst, planstate->subPlan)
		{
			SubPlanState *sps = (SubPlanState *) lfirst(lst);
			SubPlan    *sp = (SubPlan *) sps->xprstate.expr;

            Assert(!sps->cdbextratextbuf);

			es->rtable = sp->rtable;
			for (i = 0; i < indent; i++)
				appendStringInfo(str, "  ");
			appendStringInfo(str, "    ->  ");
			explain_outNode(str, sp->plan,
							sps->planstate,
							NULL,
							indent + 4, es);
		}
		es->rtable = saved_rtable;
	}

    /* CDB: Empty the output buffer if it's more than half full. */
    if (str->len*2 > str->maxlen)
    {
        do_text_output_multiline(es->tupOutputState, str->data);
        truncateStringInfo(str, 0);
    }

    es->currentSlice = currentSlice;    /* restore */
}                               /* explain_outNode */


/*
 * Show a qualifier expression for a scan plan node
 */
static void
show_scan_qual(List *qual, const char *qlabel,
			   int scanrelid, Plan *outer_plan,
			   StringInfo str, int indent, ExplainState *es)
{
	Node	   *outercontext;
	List	   *context;
	Node	   *node;
	char	   *exprstr;
	int			i;

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/*
	 * If we have an outer plan that is referenced by the qual, add it to the
	 * deparse context.  If not, don't (so that we don't force prefixes
	 * unnecessarily).
	 */
	if (outer_plan)
	{
		Relids		varnos = pull_varnos(node);

		if (bms_is_member(OUTER, varnos))
			outercontext = deparse_context_for_subplan("outer",
													   (Node *) outer_plan);
		else
			outercontext = NULL;
		bms_free(varnos);
	}
	else
		outercontext = NULL;

	context = deparse_context_for_plan(OUTER, outercontext,
									   0, NULL,
									   es->rtable);

	/* Deparse the expression */
	exprstr = deparse_expr_sweet(node, context, (outercontext != NULL), false);

	/* And add to str */
	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: %s\n", qlabel, exprstr);
}

/*
 * Show a qualifier expression for an upper-level plan node
 */
static void
show_upper_qual(List *qual, const char *qlabel,
				const char *outer_name, int outer_varno, Plan *outer_plan,
				const char *inner_name, int inner_varno, Plan *inner_plan,
				StringInfo str, int indent, ExplainState *es)
{
	List	   *context;
	Node	   *outercontext;
	Node	   *innercontext;
	Node	   *node;
	char	   *exprstr;
	int			i;
    bool		useprefix = inner_plan || list_length(es->rtable) > 1;  /*CDB*/

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Generate deparse context */
	if (outer_plan)
		outercontext = deparse_context_for_subplan(outer_name,
												   (Node *) outer_plan);
	else
		outercontext = NULL;
	if (inner_plan)
		innercontext = deparse_context_for_subplan(inner_name,
												   (Node *) inner_plan);
	else
		innercontext = NULL;
	context = deparse_context_for_plan(outer_varno, outercontext,
									   inner_varno, innercontext,
									   es->rtable);

	/* Deparse the expression */
	node = (Node *) make_ands_explicit(qual);
	exprstr = deparse_expr_sweet(node, context, useprefix, false);

	/* And add to str */
	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: %s\n", qlabel, exprstr);
}

/*
 * CDB: Show GROUP BY keys for an Agg or Group node.
 */
void
show_grouping_keys(Plan        *plan,
                   int          numCols,
                   AttrNumber  *subplanColIdx,
                   const char  *qlabel,
			       StringInfo str, int indent, ExplainState *es)
{
    Plan       *subplan = plan->lefttree;
    List	   *context;
    char	   *exprstr;
    bool		useprefix = list_length(es->rtable) > 1;
    int			keyno;
    int			i;
	int         num_null_cols = 0;
	int         rollup_gs_times = 0;
	int         rollup_level = 0;
	StringInfoData tmpstr;

	initStringInfoOfSize(&tmpstr, 1000);

    if (numCols <= 0)
        return;

    for (i = 0; i < indent; i++)
        appendStringInfoString(str, "  ");
    appendStringInfo(str, "  %s: ", qlabel);

    /* Generate deparse context */
	if (cdbpullup_exprHasSubplanRef((Expr *)subplan->targetlist))
	{
		Node	   *outerctx;
		Node	   *innerctx;

        if (subplan->righttree)
        {
            outerctx = deparse_context_for_subplan("outer",
                                                   (Node *)subplan->lefttree);
            innerctx = deparse_context_for_subplan("inner",
                                                   (Node *)subplan->righttree);
	        context = deparse_context_for_plan(OUTER, outerctx,
									           INNER, innerctx,
									           es->rtable);
            useprefix = true;
        }
        else
        {
            outerctx = deparse_context_for_subplan(NULL,
                                                   (Node *)subplan->lefttree);
	        context = deparse_context_for_plan(OUTER, outerctx,
									           0, NULL,
									           es->rtable);
        }
	}
	else
		context = deparse_context_for_plan(0, NULL,
										   0, NULL,
										   es->rtable);

	if (IsA(plan, Agg))
	{
		num_null_cols = ((Agg*)plan)->numNullCols;
		rollup_gs_times = ((Agg*)plan)->rollupGSTimes;
		rollup_level = ((Agg*)plan)->rollupLevel;
	}

	else if (IsA(plan, Group))
	{
		num_null_cols = ((Group*)plan)->numNullCols;
		rollup_gs_times = ((Group*)plan)->rollupGSTimes;
		rollup_level = ((Group*)plan)->rollupLevel;
	}

    for (keyno = 0; keyno < numCols; keyno++)
    {
	    /* find key expression in tlist */
	    AttrNumber      keyresno = subplanColIdx[keyno];
	    TargetEntry    *target = get_tle_by_resno(subplan->targetlist, keyresno);
		char grpid_str[50];
		sprintf(grpid_str, "GROUPING");

	    if (!target)
		    elog(ERROR, "no tlist entry for key %d", keyresno);

		if (IsA(target->expr, Grouping))
			/* Append "GROUPING" explicitly. */
			exprstr = grpid_str;
		else
			/* Deparse the expression, showing any top-level cast */
			exprstr = deparse_expr_sweet((Node *) target->expr, context,
										 useprefix, true);

		/* And add to str */
		if (keyno > 0)
			appendStringInfoString(str, ", ");
		appendStringInfoString(str, exprstr);

		/* Add to tmpstr for grouping sets */
		if ((num_null_cols > 0) && (numCols - num_null_cols - keyno > 0))
		{
			if (keyno > 0)
				appendStringInfoString(&tmpstr, ", ");
			if ((rollup_level == 0 && keyno == numCols - num_null_cols) ||
				(rollup_level != 0 && keyno == numCols - num_null_cols - 1))
				appendStringInfoString(&tmpstr, grpid_str);
			else
				appendStringInfoString(&tmpstr, exprstr);
		}
    }

	if (rollup_gs_times > 1)
		appendStringInfo(str, " (%d times)", rollup_gs_times);

    appendStringInfoChar(str, '\n');

	if (num_null_cols > 0)
	{
		for (i = 0; i < indent; i++)
			appendStringInfoString(str, "  ");

		appendStringInfoString(str, "  Grouping Set: (");
		appendStringInfoString(str, tmpstr.data);
		appendStringInfoString(str, ")\n");
	}

	pfree(tmpstr.data);
}                               /* show_grouping_keys */


/*
 * Show the sort keys for a Sort node.
 */
static void
show_sort_keys(Plan *sortplan, int nkeys, AttrNumber *keycols,
			   const char *qlabel,
			   StringInfo str, int indent, ExplainState *es)
{
	List	   *context;
    bool		useprefix = list_length(es->rtable) > 1;    /*CDB*/
	int			keyno;
	char	   *exprstr;
	int			i;

	if (nkeys <= 0)
		return;

	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");
	appendStringInfo(str, "  %s: ", qlabel);

	/*
	 * In this routine we expect that the plan node's tlist has not been
	 * processed by set_plan_references().	Normally, any Vars will contain
	 * valid varnos referencing the actual rtable.	But we might instead be
	 * looking at a dummy tlist generated by prepunion.c; if there are Vars
	 * with varno OUTER, use the tlist itself to determine their names.
     * (NB: CDB uses varno OUTER; in PostgreSQL it was varno 0.)
	 */
	if (cdbpullup_exprHasSubplanRef((Expr *)sortplan->targetlist))
	{
		Node	   *outercontext;

		outercontext = deparse_context_for_subplan(NULL, (Node *)sortplan);
		context = deparse_context_for_plan(OUTER, outercontext,
										   0, NULL,
										   es->rtable);
	}
	else
		context = deparse_context_for_plan(0, NULL,
										   0, NULL,
										   es->rtable);

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		/* find key expression in tlist */
		AttrNumber	keyresno = keycols[keyno];
		TargetEntry *target = get_tle_by_resno(sortplan->targetlist, keyresno);

		if (!target)
			elog(ERROR, "no tlist entry for key %d", keyresno);
		/* Deparse the expression, showing any top-level cast */
		exprstr = deparse_expr_sweet((Node *) target->expr, context,
									 useprefix, true);
		/* And add to str */
		if (keyno > 0)
			appendStringInfo(str, ", ");
		appendStringInfoString(str, exprstr);
	}

	appendStringInfo(str, "\n");
}


/*
 * CDB: Show the hash and merge keys for a Motion node.
 */
void
show_motion_keys(Plan *plan, List *hashExpr, int nkeys, AttrNumber *keycols,
			     const char *qlabel,
                 StringInfo str, int indent, ExplainState *es)
{
    List	   *context;
    char	   *exprstr;
    bool		useprefix = list_length(es->rtable) > 1;
    int			keyno;
    int			i;

    if (!nkeys &&
        !hashExpr)
        return;

    /* Generate deparse context */
	if (cdbpullup_exprHasSubplanRef((Expr *)plan->targetlist) ||
        cdbpullup_exprHasSubplanRef((Expr *)hashExpr))
	{
		Node	   *outercontext;

	    outercontext = deparse_context_for_subplan(NULL, (Node *)plan);
	    context = deparse_context_for_plan(OUTER, outercontext,
									       0, NULL,
									       es->rtable);
	}
	else
		context = deparse_context_for_plan(0, NULL,
										   0, NULL,
										   es->rtable);

    /* Merge Receive ordering key */
    if (nkeys > 0)
    {
        for (i = 0; i < indent; i++)
            appendStringInfoString(str, "  ");
        appendStringInfo(str, "  %s: ", qlabel);

	    for (keyno = 0; keyno < nkeys; keyno++)
	    {
		    /* find key expression in tlist */
		    AttrNumber	keyresno = keycols[keyno];
		    TargetEntry *target = get_tle_by_resno(plan->targetlist, keyresno);

		    /* Deparse the expression, showing any top-level cast */
		    if (target)
		        exprstr = deparse_expr_sweet((Node *) target->expr, context,
									         useprefix, true);
            else
            {
                elog(WARNING, "Gather Motion %s error: no tlist item %d",
                     qlabel, keyresno);
                exprstr = "*BOGUS*";
            }

		    /* And add to str */
		    if (keyno > 0)
			    appendStringInfoString(str, ", ");
		    appendStringInfoString(str, exprstr);
	    }

	    appendStringInfoChar(str, '\n');
    }

    /* Hashed repartitioning key */
    if (hashExpr)
    {
	    /* Deparse the expression */
	    exprstr = deparse_expr_sweet((Node *)hashExpr, context, useprefix, true);

	    /* And add to str */
	    for (i = 0; i < indent; i++)
		    appendStringInfoString(str, "  ");
	    appendStringInfo(str, "  %s: %s\n", "Hash Key", exprstr);
    }
}                               /* show_motion_keys */

