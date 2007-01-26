/*-------------------------------------------------------------------------
 *
 * cdbexplain.h
 *    Functions supporting the MPP EXPLAIN command
 *
 * Copyright (c) 2006-2007, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBEXPLAIN_H
#define CDBEXPLAIN_H

struct CdbDispatchResults;              /* #include "cdb/cdbdispatchresult.h" */
struct PlanState;                       /* #include "nodes/execnodes.h" */
struct QueryDesc;                       /* #include "executor/execdesc.h" */
struct Slice;                           /* #include "nodes/execnodes.h" */
struct StringInfoData;                  /* #include "lib/stringinfo.h" */
struct Instrumentation;                 /* #include "executor/instrument.h" */

/*
 * cdbexplain_sendExecStats
 *    Called by qExec process to send EXPLAIN ANALYZE statistics to qDisp.
 *    On the qDisp, libpq will attach this data to the PGresult object.
 */
void
cdbexplain_sendExecStats(struct QueryDesc *queryDesc);

/*
 * cdbexplain_recvExecStats
 *    Called by qDisp process to transfer a slice's EXPLAIN ANALYZE statistics
 *    from the CdbDispatchResults structures to the PlanState tree.
 */
void
cdbexplain_recvExecStats(struct PlanState          *planstate,
                         struct CdbDispatchResults *dispatchResults,
                         int                        sliceIndex);

/*
 * cdbexplain_format_time
 *    common format routine to convert time in msec to readable string
 */
void
cdbexplain_format_time(char* outbuf, double totalmsecs);

/*
 * cdbexplain_showExecStats
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 */
void
cdbexplain_showExecStats(struct PlanState      *planstate,
                         struct StringInfoData *str,
                         struct StringInfoData *workbuf,
                         struct Slice          *slice,
                         int                    indent,
						 /* pass instrumentation to track first start time */
						 struct Instrumentation *pFirst);


#endif   /* CDBEXPLAIN_H */
