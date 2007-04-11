/*-------------------------------------------------------------------------
 *
 * execdesc.h
 *	  plan and query descriptor accessor macros used by the executor
 *	  and related modules.
 *
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/execdesc.h,v 1.32 2006/07/11 16:35:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDESC_H
#define EXECDESC_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "tcop/dest.h"

struct CdbExplain_ShowStatCtx;  /* private, in "cdb/cdbexplain.c" */
struct EState;                  /* #include "nodes/execnodes.h" */
struct PlanState;               /* #include "nodes/execnodes.h" */


/* ----------------
 *		query descriptor:
 *
 *	a QueryDesc encapsulates everything that the executor
 *	needs to execute the query
 * ---------------------
 */
typedef struct QueryDesc
{
	/* These fields are provided by CreateQueryDesc */
	CmdType		operation;		/* CMD_SELECT, CMD_UPDATE, etc. */
	Query	   *parsetree;		/* rewritten parsetree */
	Plan	   *plantree;		/* planner's output */
	Snapshot	snapshot;		/* snapshot to use for query */
	Snapshot	crosscheck_snapshot;	/* crosscheck for RI update/delete */
	DestReceiver *dest;			/* the destination for tuple output */
	ParamListInfo params;		/* param values being passed in */
	bool		doInstrument;	/* TRUE requests runtime instrumentation */

	/* These fields are set by ExecutorStart */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	struct EState      *estate;			/* executor's query-wide state */
	struct PlanState   *planstate;		/* tree of per-plan-node state */
	
	/* This field is set by ExecutorEnd after collecting cdbdisp results */
	uint32		es_processed;	/* # of tuples processed */
	Oid			es_lastoid;		/* oid of row inserted */
	bool		cursor_query;   /* does this query service a cursor? */
	char		*portal_name;	/* NULL for unnamed portal */
	
    /* CDB: EXPLAIN ANALYZE statistics */
    struct CdbExplain_ShowStatCtx  *showstatctx;

} QueryDesc;

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(Query *parsetree, Plan *plantree,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				bool doInstrument);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif   /* EXECDESC_H  */
