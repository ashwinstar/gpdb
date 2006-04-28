/*-------------------------------------------------------------------------
 *
 * cdbexplain.c
 *    Functions supporting the MPP EXPLAIN command
 *
 * Copyright (c) 2006, greenplum inc
 *
 * IDENTIFICATION
 *    $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbconn.h"                /* SegmentDatabaseDescriptor */
#include "cdb/cdbdispatchresult.h"      /* CdbDispatchResults */
#include "cdb/cdbexplain.h"             /* me */
#include "cdb/cdbvars.h"                /* Mpp_segment */
#include "executor/executor.h"          /* ExecStateTreeWalker */
#include "executor/instrument.h"        /* Instrumentation */
#include "lib/stringinfo.h"             /* StringInfo */
#include "libpq-fe.h"                   /* PGresult; prereq for libpq-int.h */
#include "libpq-int.h"                  /* pg_result */
#include "libpq/pqformat.h"             /* pq_beginmessage() etc. */


/* EXPLAIN ANALYZE statistics for one plan node of a slice */
typedef struct CdbExplain_StatInst
{
    NodeTag     pstype;         /* PlanState node type */
    double      startup;        /* Total startup time (in seconds) */
    double      total;          /* Total total time (in seconds) */
    double      ntuples;        /* Total tuples produced */
    double      nloops;         /* # of run cycles for this node */
} CdbExplain_StatInst;


/* Header of EXPLAIN ANALYZE statistics message sent from qExec to qDisp */
typedef struct CdbExplain_StatHdr
{
    int         segindex;       /* segment id */
    int         nInst;          /* num of StatInst entries following StatHdr */

    /* Array [0..ninst-1] of StatInst entries is appended starting here */
    CdbExplain_StatInst inst[1];
} CdbExplain_StatHdr;


/* State for cdbexplain_sendStatWalker */
typedef struct CdbExplain_SendStatCtx
{
    StringInfoData      buf;
    CdbExplain_StatHdr  hdr;
} CdbExplain_SendStatCtx;


/* State for cdbexplain_recvStatWalker */
typedef struct CdbExplain_RecvStatCtx
{
    int                     iStatInst;
    int                     nStatInst;
    int                     nmsgptr;
    CdbExplain_StatHdr    **msgptrs;
} CdbExplain_RecvStatCtx;


static CdbVisitOpt
cdbexplain_sendStatWalker(PlanState *planstate, void *context);
static CdbVisitOpt
cdbexplain_recvStatWalker(PlanState *planstate, void *context);


/*
 * cdbexplain_sendExecutionStatistics
 *    Called by qExec process to send EXPLAIN ANALYZE statistics to qDisp.
 *    On the qDisp, libpq will recognize our special message type ('Y') and
 *    attach the message to the current command's PGresult object.
 */
void
cdbexplain_sendExecutionStatistics(QueryDesc *queryDesc)
{
    EState                 *estate;
    PlanState              *planstate;
    CdbExplain_SendStatCtx   ctx;
    int                     hoff;

    if (!queryDesc ||
        !queryDesc->planstate ||
        !queryDesc->estate ||
        !queryDesc->estate->es_slice_map)
        return;

    /* If executing a root slice (UPD/DEL/INS), start at top of plan tree. */
    estate = queryDesc->estate;
    if (estate->es_cur_slice_idx == estate->es_cur_root_idx)
        planstate = queryDesc->planstate;

    /* Non-root slice: Start at child of our sending Motion node. */
    else
    {
        planstate = &estate->es_slice_map[estate->es_cur_slice_idx]->ps;
        Assert(planstate &&
               IsA(planstate, MotionState) &&
               planstate->lefttree);
        planstate = planstate->lefttree;
    }

    /* Start building the message header in our context area. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.hdr.segindex = Mpp_segment;
    ctx.hdr.nInst = 0;

    /* Reserve buffer space for the message header (excluding 'inst' array). */
    pq_beginmessage(&ctx.buf, 'Y');
    hoff = ctx.buf.len;
    appendBinaryStringInfo(&ctx.buf, (char *)&ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

    /* Append statistics from each PlanState node in this slice. */
    ExecStateTreeWalker(planstate, cdbexplain_sendStatWalker, NULL, &ctx);

    /* Move the message header into the buffer. */
    memcpy(ctx.buf.data+hoff, (char *)&ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

    /* Send message to qDisp process. */
    pq_endmessage(&ctx.buf);
}                               /* cdbexplain_sendExecutionStatistics */


/*
 * cdbexplain_recvExecutionStatistics
 *    Called by qDisp process to transfer a slice's EXPLAIN ANALYZE statistics
 *    from the CdbDispatchResults structures to the PlanState tree.
 */
void
cdbexplain_recvExecutionStatistics(struct QueryDesc    *queryDesc,
                                   struct PlanState    *planstate,
                                   int                  sliceIndex)
{
    CdbDispatchResults     *primaryResults = queryDesc->estate->primaryResults;
    CdbDispatchResult      *dispatchResultBeg;
    CdbDispatchResult      *dispatchResultEnd;
    CdbExplain_RecvStatCtx  ctx;
    int                     mppSegmentCount = getMPPSegmentCount();
    int                     iDispatch;
    int                     nDispatch;

    Assert(planstate &&
           IsA(planstate, MotionState) &&
           planstate->lefttree &&
           planstate->lefttree->instrument);

    /* Descend to child of this slice's sending Motion node. */
    planstate = planstate->lefttree;

    /* Initialize treewalk context. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.iStatInst = 0;
    ctx.nStatInst = 0;

    /* Find the slice's CdbDispatchResult objects. */
    dispatchResultBeg = cdbdisp_resultBegin(primaryResults, sliceIndex);
    dispatchResultEnd = cdbdisp_resultEnd(primaryResults, sliceIndex);
    nDispatch = dispatchResultEnd - dispatchResultBeg;

    if (nDispatch == 0)
        return;

    /* Find and validate the statistics returned from each qExec. */
    ctx.msgptrs = (CdbExplain_StatHdr **)palloc0(nDispatch * sizeof(ctx.msgptrs[0]));
    ctx.nmsgptr = 0;
    for (iDispatch = 0; iDispatch < nDispatch; iDispatch++)
    {
        CdbDispatchResult  *dispatchResult = &dispatchResultBeg[iDispatch];
        PGresult           *pgresult;
        CdbExplain_StatHdr *hdr;

        /* Find this qExec's last PGresult. */
        pgresult = cdbdisp_getPGresult(dispatchResult, -1);

        /* Skip to next qExec if no statistics. */
        if (!pgresult ||
            !pgresult->cdbStatBuf)
            continue;

        /* Validate the message header. */
        hdr = (CdbExplain_StatHdr *)pgresult->cdbStatBuf;
        if ((size_t)pgresult->cdbStatLen < sizeof(*hdr) ||
            (size_t)pgresult->cdbStatLen != sizeof(*hdr) - sizeof(hdr->inst) +
                                                hdr->nInst * sizeof(hdr->inst) ||
            hdr->segindex < 0 ||
            hdr->segindex >= mppSegmentCount)
        {
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg_internal("Invalid execution statistics "
                                            "response returned from seg%d.  "
                                            "length=%d",
                                            dispatchResult->segdbDesc->segindex,
                                            pgresult->cdbStatLen),
                            errhint("Please verify that all instances are using "
                                    "the correct %s software version.",
                                    PACKAGE_NAME)
                    ));
        }

        /* Slice should have same number of plan nodes on every qExec. */
        if (iDispatch == 0)
            ctx.nStatInst = hdr->nInst;
        else
            Insist(ctx.nStatInst == hdr->nInst);

        /* Save message ptr for easy reference. */
        ctx.msgptrs[ctx.nmsgptr] = hdr;
        ctx.nmsgptr++;
    }

    /* Stuff statistics into each PlanState node's Instrumentation node. */
    ExecStateTreeWalker(planstate, cdbexplain_recvStatWalker, NULL, &ctx);

    /* Make sure we visited the right number of PlanState nodes. */
    Insist(ctx.iStatInst == ctx.nStatInst);

    /* Clean up. */
    pfree(ctx.msgptrs);
}                               /* cdbexplain_recvExecutionStatistics */


/*
 * cdbexplain_recvStatWalker
 *    Update the given PlanState node's Instrument node with statistics
 *    received from qExecs.
 */
CdbVisitOpt
cdbexplain_recvStatWalker(PlanState *planstate, void *context)
{
    CdbExplain_RecvStatCtx *ctx = (CdbExplain_RecvStatCtx *)context;
    Instrumentation        *instr = planstate->instrument;
    CdbExplain_StatHdr     *shMax;
    CdbExplain_StatInst    *siMax;
    int                     imsgptr;
    double                  ntMax;

    Insist(instr &&
           ctx->iStatInst < ctx->nStatInst);

    /* Start with one qExec's statistics for the node. */
    shMax = ctx->msgptrs[0];
    siMax = &shMax->inst[ctx->iStatInst];
    ntMax = siMax->ntuples;

    instr->cdbGangTuples = siMax->ntuples;
    instr->cdbWorkers = (siMax->ntuples > 0) ? 1 : 0;

    Insist(siMax->pstype == planstate->type);

    /* Examine the statistics from the rest of the qExecs. */
    for (imsgptr = 1; imsgptr < ctx->nmsgptr; imsgptr++)
    {
        CdbExplain_StatHdr     *sh = ctx->msgptrs[imsgptr];
        CdbExplain_StatInst    *si = &sh->inst[ctx->iStatInst];
        double                  nt = si->ntuples;

        Insist(siMax->pstype == si->pstype);

        /* Update gang totals. */
        if (si->ntuples > 0)
        {
            instr->cdbGangTuples += si->ntuples;
            instr->cdbWorkers++;
        }

        /* Remember qExec with highest number of output tuples. */
        if (nt < ntMax)
            continue;
        if (nt == ntMax &&
            si->total <= siMax->total)
            continue;

        shMax = sh;
        siMax = si;
        ntMax = nt;
    }

    /* Copy stats from winning qExec into the Instrumentation node. */
    instr->startup = siMax->startup;
    instr->total = siMax->total;
    instr->ntuples = siMax->ntuples;
    instr->nloops = siMax->nloops;

    /* Store segment id of the winning qExec. */
    instr->cdbSegIndex = shMax->segindex;

    /* Advance to next node's entry in all of the StatInst arrays. */
    ctx->iStatInst++;

    /* Don't descend below bottom of slice. */
    if (IsA(planstate, MotionState))
        return CdbVisit_Skip;

    return CdbVisit_Walk;
}                               /* cdbexplain_recvStatWalker */


/*
 * cdbexplain_sendStatWalker
 */
CdbVisitOpt
cdbexplain_sendStatWalker(PlanState *planstate, void *context)
{
    CdbExplain_SendStatCtx *ctx = (CdbExplain_SendStatCtx *)context;
    CdbExplain_StatInst    *si = &ctx->hdr.inst[0];
    Instrumentation        *instr = planstate->instrument;

    Insist(instr);

    /* We have to finalize statistics, since ExecutorEnd hasn't been called. */
    InstrEndLoop(instr);

    /* Get this node's statistics. */
    memset(si, 0, sizeof(*si));
    si->pstype = planstate->type;
    si->startup = instr->startup;
    si->total = instr->total;
    si->ntuples = instr->ntuples;
    si->nloops = instr->nloops;

    /* Append to message. */
    appendBinaryStringInfo(&ctx->buf, (char *)si, sizeof(*si));
    ctx->hdr.nInst++;

    /* Don't descend across a slice boundary. */
    if (IsA(planstate, MotionState))
        return CdbVisit_Skip;

    return CdbVisit_Walk;
}                               /* cdbexplain_sendStatWalker */


/*
 * cdbexplain_showExecutionStatistics
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 */
void
cdbexplain_showExecutionStatistics(struct PlanState        *planstate,
                                   struct StringInfoData   *str,
                                   int                      indent)
{
    Instrumentation    *instr = planstate->instrument;
    double      startup = instr->startup * 1000.0;  /* convert secs to ms */
    double      total = instr->total * 1000.0;      /* convert secs to ms */
    char        firstbuf[32];
    char        totalbuf[32];
    int         i;

	for (i = 0; i < indent; i++)
		appendStringInfo(str, "  ");

    /* Motion node stats are taken from the receiving (upper) slice. */
    if (IsA(planstate, MotionState))
        appendStringInfoString(str, "recv:  ");

    /* Average number of result tuples per qExec */
    if (instr->cdbWorkers > 1)
        appendStringInfo(str, "Avg %.2f rows x %d workers.  Max ",
                         instr->cdbGangTuples / instr->cdbWorkers,
                         instr->cdbWorkers);
    else
        appendStringInfoString(str, "Total ");

    /* Number of tuples from the most prolific worker */
    appendStringInfo(str, "%.0f rows ", instr->ntuples);

    /* Identify the process whose stats are shown (unless it's the qDisp). */
    if (instr->cdbSegIndex >= 0)
        appendStringInfo(str, "(seg%d) ", instr->cdbSegIndex);
    appendStringInfoString(str, "with ");

    /* Number of rescans */
    if (instr->nloops > 1)
    {
        appendStringInfo(str, "%.0f loops, avg ", instr->nloops);
        startup /= instr->nloops;
        total /= instr->nloops;
    }

    /* Elapsed time (ms) per rescan to fetch first row */
    if (startup < 20 && startup != 0)
        sprintf(firstbuf, "%.3f", startup);
    else
        sprintf(firstbuf, "%.0f", startup);

    /* Elapsed time (ms) per rescan to fetch all rows */
    if (total < 20 && total != 0)
        sprintf(totalbuf, "%.3f", total);
    else
        sprintf(totalbuf, "%.0f", total);

    /* Show elapsed time just once if they are the same. */
    if (strcmp(firstbuf, totalbuf) == 0)
        appendStringInfo(str, "%s ms to end", totalbuf);
    else
        appendStringInfo(str, "%s ms to first row, %s ms to end",
                         firstbuf, totalbuf);

    appendStringInfoString(str, ".\n");
}                               /* cdbexplain_showExecutionStatistics */

