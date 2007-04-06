/*-------------------------------------------------------------------------
 *
 * cdbexplain.h
 *    Functions supporting the Greenplum EXPLAIN command
 *
 * Copyright (c) 2006-2007, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBEXPLAIN_H
#define CDBEXPLAIN_H

#include "executor/instrument.h"        /* instr_time */

struct CdbDispatchResults;              /* #include "cdb/cdbdispatchresult.h" */
struct PlanState;                       /* #include "nodes/execnodes.h" */
struct QueryDesc;                       /* #include "executor/execdesc.h" */
struct Slice;                           /* #include "nodes/execnodes.h" */
struct StringInfoData;                  /* #include "lib/stringinfo.h" */

struct CdbExplain_ShowStatCtx;          /* private, in "cdb/cdbexplain.c" */

/*
 * cdbexplain_sendExecStats
 *    Called by qExec process to send EXPLAIN ANALYZE statistics to qDisp.
 *    On the qDisp, libpq will attach this data to the PGresult object.
 */
void
cdbexplain_sendExecStats(struct QueryDesc *queryDesc);

/*
 * cdbexplain_recvExecStats
 *    Called by qDisp to transfer a slice's EXPLAIN ANALYZE statistics
 *    from the CdbDispatchResults structures to the PlanState tree.
 *    Recursively does the same for slices that are descendants of the
 *    one specified.
 *
 * 'extratextbuf' is a buffer which the caller must provide, to which this
 *      function may append extra message text for later output by
 *      cdbexplain_showExecStats().
 */
void
cdbexplain_recvExecStats(struct PlanState          *planstate,
                         struct CdbDispatchResults *dispatchResults,
                         int                        sliceIndex,
                         struct StringInfoData     *extratextbuf);

/*
 * cdbexplain_showExecStatsBegin
 *    Called by qDisp process to create a CdbExplain_ShowStatCtx structure
 *    in which to accumulate overall statistics for a query.
 *
 * 'querystarttime' is the timestamp of the start of the query, in a
 *      platform-dependent format.
 */
struct CdbExplain_ShowStatCtx *
cdbexplain_showExecStatsBegin(instr_time    querystarttime);

/*
 * cdbexplain_showExecStatsEnd
 *    Called by qDisp process to format the overall statistics for a query
 *    into the caller's buffer and free the CdbExplain_ShowStatCtx object.
 *
 * 'ctx' is the CdbExplain_ShowStatCtx object which was created by a call to
 *      cdbexplain_showExecStatsBegin() and contains statistics which have
 *      been accumulated over a series of calls to cdbexplain_showExecStats().
 *      Invalid on return (it is freed).
 * 'str' is the output buffer.
 */
void
cdbexplain_showExecStatsEnd(struct CdbExplain_ShowStatCtx  *ctx,
                            struct StringInfoData          *str);

/*
 * cdbexplain_showExecStats
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 *
 * 'ctx' is a CdbExplain_ShowStatCtx object which was created by a call to
 *      cdbexplain_showExecStatsBegin().
 * 'str' is the output buffer.
 * 'extratextbuf' is the buffer in which extra message text was saved by
 *      cdbexplain_recvExecStats().  Some more extra message text might be
 *      appended to this buffer.
 */
void
cdbexplain_showExecStats(struct PlanState              *planstate,
                         struct StringInfoData         *str,
                         int                            indent,
                         struct CdbExplain_ShowStatCtx *ctx,
                         struct StringInfoData         *extratextbuf);


#endif   /* CDBEXPLAIN_H */
