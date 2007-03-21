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
 * cdbexplain_showExecStats
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 *
 * 'str' is the output buffer.
 * 'extratextbuf' is the buffer in which extra message text was saved by
 *      cdbexplain_recvExecStats().  Some more extra message text might be
 *      appended to this buffer.
 * 'querystarttime' is the timestamp of the start of the query, in a
 *      platform-dependent format.
 */
void
cdbexplain_showExecStats(struct PlanState      *planstate,
                         struct StringInfoData *str,
                         int                    indent,
                         struct StringInfoData *extratextbuf,
                         instr_time             querystarttime);


#endif   /* CDBEXPLAIN_H */
