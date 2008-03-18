/*-------------------------------------------------------------------------
 *
 * execdesc.h
 *	  plan and query descriptor accessor macros used by the executor
 *	  and related modules.
 *
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/execdesc.h,v 1.37 2008/01/01 19:45:57 momjian Exp $
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
struct SliceTable;              /* #include "nodes/execnodes.h" */


/* ----------------
 *		query descriptor:
 *
 *	a QueryDesc encapsulates everything that the executor
 *	needs to execute the query.
 *
 *	For the convenience of SQL-language functions, we also support QueryDescs
 *	containing utility statements; these must not be passed to the executor
 *	however.
 * ---------------------
 */
typedef struct QueryDesc
{
	/* These fields are provided by CreateQueryDesc */
	CmdType		operation;		/* CMD_SELECT, CMD_UPDATE, etc. */
	PlannedStmt *plannedstmt;	/* planner's output, or null if utility */
	Node	   *utilitystmt;	/* utility statement, or null */
	Snapshot	snapshot;		/* snapshot to use for query */
	Snapshot	crosscheck_snapshot;	/* crosscheck for RI update/delete */
	DestReceiver *dest;			/* the destination for tuple output */
	ParamListInfo params;		/* param values being passed in */
	bool		doInstrument;	/* TRUE requests runtime instrumentation */

	/* These fields are set by ExecutorStart */
	TupleDesc	tupDesc;		/* descriptor for result tuples */
	struct EState      *estate;			/* executor's query-wide state */
	struct PlanState   *planstate;		/* tree of per-plan-node state */
    struct SliceTable  *sliceTable;     /* CDB: parallel exec params */   

	/* This field is set by ExecutorEnd after collecting cdbdisp results */
	uint64		es_processed;	/* # of tuples processed */
	Oid			es_lastoid;		/* oid of row inserted */
	bool		cursor_query;   /* does this query service a cursor? */
	char		*portal_name;	/* NULL for unnamed portal */
	
    /* CDB: EXPLAIN ANALYZE statistics */
    struct CdbExplain_ShowStatCtx  *showstatctx;
    
} QueryDesc;

static inline struct TableOidInfo *querydesc_into_oidinfo(QueryDesc *qd)
{
	return qd->plannedstmt->intoClause->intoOidInfo;
}
static inline GpPolicy *querydesc_into_policy(QueryDesc *qd)
{
	// MERGEBUG
	// workaround problem?
	if (qd == NULL) return NULL;
	if (qd->plannedstmt == NULL) return NULL;
	if (qd->plannedstmt->intoClause == NULL) return NULL;
	// end workaround
	return qd->plannedstmt->intoClause->intoPolicy;
}

/* in pquery.c */
extern QueryDesc *CreateQueryDesc(PlannedStmt *plannedstmt,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				bool doInstrument);

extern QueryDesc *CreateUtilityQueryDesc(Node *utilitystmt,
					   Snapshot snapshot,
					   DestReceiver *dest,
					   ParamListInfo params);

extern void FreeQueryDesc(QueryDesc *qdesc);

#endif   /* EXECDESC_H  */
