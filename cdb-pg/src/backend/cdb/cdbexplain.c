/*-------------------------------------------------------------------------
 *
 * cdbexplain.c
 *    Functions supporting the GP EXPLAIN command
 *
 * Copyright (c) 2006-2007, greenplum inc
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
	instr_time	starttime;		/* Start time of current iteration of node */
	instr_time	counter;		/* Accumulated runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
    double      startup;        /* Total startup time (in seconds) */
    double      total;          /* Total total time (in seconds) */
    double      ntuples;        /* Total tuples produced */
    double      nloops;         /* # of run cycles for this node */
    double      workmemused;    /* work_mem actually used (bytes) */
    double      workmemwanted;  /* work_mem to avoid workfile i/o (bytes) */
	instr_time	firststart;		/* Start time of first iteration of node */
    int         bnotes;         /* Offset to beginning of node's extra text */
    int         enotes;         /* Offset to end of node's extra text */
} CdbExplain_StatInst;


/* Header of EXPLAIN ANALYZE statistics message sent from qExec to qDisp */
typedef struct CdbExplain_StatHdr
{
    int         segindex;       /* segment id */
    int         nInst;          /* num of StatInst entries following StatHdr */
    int         bnotes;         /* offset to extra text area */
    int         enotes;         /* offset to end of extra text area */

    /* Array [0..nInst-1] of StatInst entries is appended starting here */
    CdbExplain_StatInst inst[1];

    /* extra text is appended after that */
} CdbExplain_StatHdr;


/* Summarization of one statistic */
typedef struct CdbExplain_Agg
{
    double      vmax;           /* maximum value of statistic */
    double      vsum;           /* sum of values */
    int         vcnt;           /* count of values > 0 */
    int         segmax;         /* segment id of qExec having maximum value */
} CdbExplain_Agg;

static inline void
cdbexplain_agg_init0(CdbExplain_Agg *agg)
{
    agg->vmax = 0;
    agg->vsum = 0;
    agg->vcnt = 0;
    agg->segmax = 0;
}

static inline void
cdbexplain_agg_init1(CdbExplain_Agg *agg, double v, int segindex)
{
    if (v > 0)
    {
        agg->vmax = v;
        agg->vsum = v;
        agg->vcnt = 1;
        agg->segmax = segindex;
    }
    else
        cdbexplain_agg_init0(agg);
}


/* One node's EXPLAIN ANALYZE statistics for all the workers of its gang */
typedef struct CdbExplain_NodeSummary
{
    int         segindex0;      /* segment id of inst[0] */
    int         nInst;          /* num of StatInst entries in inst array */

    CdbExplain_Agg  ntuples;
    CdbExplain_Agg  workmemused;
    CdbExplain_Agg  workmemwanted;

    /* Array [0..nInst-1] of StatInst entries is appended starting here */
    CdbExplain_StatInst inst[1];    /* variable size - must be last */
} CdbExplain_NodeSummary;


/* State for cdbexplain_sendStatWalker */
typedef struct CdbExplain_SendStatCtx
{
    StringInfoData      buf;
    StringInfoData      notebuf;
    CdbExplain_StatHdr  hdr;
} CdbExplain_SendStatCtx;


/* State for cdbexplain_recvStatWalker */
typedef struct CdbExplain_RecvStatCtx
{
    int                     iStatInst;
    int                     nStatInst;
    int                     segindexMin;
    int                     segindexMax;
    int                     nmsgptr;
    CdbExplain_StatHdr    **msgptrs;
    CdbDispatchResults     *dispatchResults;
    StringInfoData         *extratextbuf;
} CdbExplain_RecvStatCtx;


static CdbVisitOpt
cdbexplain_sendStatWalker(PlanState *planstate, void *context);
static CdbVisitOpt
cdbexplain_recvStatWalker(PlanState *planstate, void *context);

static int
cdbexplain_collectExtraText(PlanState *planstate, StringInfo notebuf);


/*
 * cdbexplain_sendExecStats
 *    Called by qExec process to send EXPLAIN ANALYZE statistics to qDisp.
 *    On the qDisp, libpq will recognize our special message type ('Y') and
 *    attach the message to the current command's PGresult object.
 */
void
cdbexplain_sendExecStats(QueryDesc *queryDesc)
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

    /* Allocate a separate buffer where nodes can append extra message text. */
    initStringInfo(&ctx.notebuf);

    /* Reserve buffer space for the message header (excluding 'inst' array). */
    pq_beginmessage(&ctx.buf, 'Y');
    hoff = ctx.buf.len;
    appendBinaryStringInfo(&ctx.buf, (char *)&ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

    /* Append statistics from each PlanState node in this slice. */
    ExecStateTreeWalker(planstate, cdbexplain_sendStatWalker, NULL, &ctx);

    /* Append the extra message text. */
    ctx.hdr.bnotes = ctx.buf.len - hoff;
    appendBinaryStringInfo(&ctx.buf, ctx.notebuf.data, ctx.notebuf.len);
    ctx.hdr.enotes = ctx.buf.len - hoff;
    pfree(ctx.notebuf.data);

    /* Move the message header into the buffer. */
    memcpy(ctx.buf.data+hoff, (char *)&ctx.hdr, sizeof(ctx.hdr) - sizeof(ctx.hdr.inst));

    /* Send message to qDisp process. */
    pq_endmessage(&ctx.buf);
}                               /* cdbexplain_sendExecStats */


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

    /* Initialize StatInst slot. */
    memset(si, 0, sizeof(*si));
    si->pstype = planstate->type;

    /* Add this node's extra message text to notebuf.  Store final stats. */
    si->bnotes = cdbexplain_collectExtraText(planstate, &ctx->notebuf);
    si->enotes = ctx->notebuf.len;

    /* Make sure there is a '\0' between this node's message and the next. */
    if (si->bnotes < si->enotes)
        appendStringInfoChar(&ctx->notebuf, '\0');

    /* Get this node's statistics. */
	si->starttime = instr->starttime;
	si->counter = instr->counter;
	si->firsttuple = instr->firsttuple;
    si->startup = instr->startup;
    si->total = instr->total;
    si->ntuples = instr->ntuples;
    si->nloops = instr->nloops;
    si->workmemused = instr->workmemused;
    si->workmemwanted = instr->workmemwanted;
	si->firststart = instr->firststart;

    /* Append StatInst instance to message. */
    appendBinaryStringInfo(&ctx->buf, (char *)si, sizeof(*si));
    ctx->hdr.nInst++;

    /* Don't descend across a slice boundary. */
    if (IsA(planstate, MotionState))
        return CdbVisit_Skip;

    return CdbVisit_Walk;
}                               /* cdbexplain_sendStatWalker */


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
                         struct StringInfoData     *extratextbuf)
{
    CdbDispatchResult      *dispatchResultBeg;
    CdbDispatchResult      *dispatchResultEnd;
    CdbExplain_RecvStatCtx  ctx;
    MemoryContext           oldcxt;
    int                     mppSegmentCount = getMPPSegmentCount();
    int                     iDispatch;
    int                     nDispatch;

    if (!planstate ||
        !planstate->instrument)
        return;

    /* Initialize treewalk context. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.dispatchResults = dispatchResults;
    ctx.extratextbuf = extratextbuf;

    /* Find the slice's CdbDispatchResult objects. */
    dispatchResultBeg = cdbdisp_resultBegin(dispatchResults, sliceIndex);
    dispatchResultEnd = cdbdisp_resultEnd(dispatchResults, sliceIndex);
    nDispatch = dispatchResultEnd - dispatchResultBeg;

    /* Find and validate the statistics returned from each qExec. */
    if (nDispatch > 0)
        ctx.msgptrs = (CdbExplain_StatHdr **)palloc0(nDispatch * sizeof(ctx.msgptrs[0]));
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
                                                hdr->nInst * sizeof(hdr->inst) +
                                                hdr->enotes - hdr->bnotes ||
            pgresult->cdbStatLen != hdr->enotes ||
            hdr->segindex < -1 ||
            hdr->segindex >= mppSegmentCount)
        {
            ereport(ERROR, (errcode(ERRCODE_MPP_INTERCONNECTION_ERROR),
                            errmsg_internal("Invalid execution statistics "
                                            "response returned from seg%d.  "
                                            "length=%d",
                                            hdr->segindex,
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

        /* Save lowest and highest segment id for which we have stats. */
        if (iDispatch == 0)
            ctx.segindexMin = ctx.segindexMax = hdr->segindex;
        else if (ctx.segindexMax < hdr->segindex)
            ctx.segindexMax = hdr->segindex;
        else if (ctx.segindexMin > hdr->segindex)
            ctx.segindexMin = hdr->segindex;

        /* Save message ptr for easy reference. */
        ctx.msgptrs[ctx.nmsgptr] = hdr;
        ctx.nmsgptr++;
    }

    /*
     * Note that the caller may free the CdbDispatchResults upon return, maybe
     * before EXPLAIN ANALYZE examines the PlanState tree.  Consequently we
     * must not return ptrs into the dispatch result buffers, but must copy any
     * needed information into a sufficiently long-lived memory context.
     */
    oldcxt = MemoryContextSwitchTo(planstate->state->es_query_cxt);

    /* Stuff statistics into each PlanState node's Instrumentation node. */
    ExecStateTreeWalker(planstate, cdbexplain_recvStatWalker, NULL, &ctx);

    /* Restore caller's memory context. */
    MemoryContextSwitchTo(oldcxt);

    /* Make sure we visited the right number of PlanState nodes. */
    Insist(ctx.iStatInst == ctx.nStatInst);

    /* Clean up. */
    if (ctx.msgptrs)
        pfree(ctx.msgptrs);
}                               /* cdbexplain_recvExecStats */


/*
 * CdbExplain_RecvStatAcc
 *    Segment statistic accumulator used by cdbexplain_recvStatWalker().
 */
typedef struct CdbExplain_RecvStatAcc
{
    CdbExplain_Agg          agg;            /* vmax, vsum, vcnt, segmax */
    CdbExplain_StatHdr     *rshmax;         /* max's received StatHdr */
    CdbExplain_StatInst    *rsimax;         /* max's received inst in StatHdr */
    CdbExplain_StatInst    *nsimax;         /* max's inst in NodeSummary */
} CdbExplain_RecvStatAcc;

static void
cdbexplain_recvStatAcc_init(CdbExplain_RecvStatAcc *acc)
{
    cdbexplain_agg_init0(&acc->agg);
    acc->rshmax = NULL;
    acc->rsimax = NULL;
    acc->nsimax = NULL;
}                               /* cdbexplain_recvStatAcc_init */

static inline void
cdbexplain_recvStatAcc_upd(CdbExplain_RecvStatAcc  *acc,
                           double                   v,
                           CdbExplain_StatHdr      *rsh,
                           CdbExplain_StatInst     *rsi,
                           CdbExplain_StatInst     *nsi)
{
    if (v > 0)
    {
        acc->agg.vsum += v;
        acc->agg.vcnt++;
    }
    if (acc->agg.vmax < v ||
        acc->rshmax == NULL)
    {
        acc->agg.vmax = v;
        acc->agg.segmax = rsh->segindex;
        acc->rshmax = rsh;
        acc->rsimax = rsi;
        acc->nsimax = nsi;
    }
}                               /* cdbexplain_recvStatAcc_upd */

static void
cdbexplain_recvStatAcc_saveText(CdbExplain_RecvStatAcc *acc,
                                StringInfoData         *extratextbuf)
{
    CdbExplain_StatHdr     *rsh = acc->rshmax;
    CdbExplain_StatInst    *rsi = acc->rsimax;
    CdbExplain_StatInst    *nsi = acc->nsimax;

    if (nsi->bnotes == nsi->enotes &&
        rsi->bnotes < rsi->enotes)
    {
        /* Locate extra message text in dispatch result buffer. */
        int         notelen = rsi->enotes - rsi->bnotes;
        const char *notes = (const char *)rsh + rsh->bnotes + rsi->bnotes;

        Insist(rsh->bnotes + rsi->enotes < rsh->enotes &&
               notes[notelen] == '\0');

        /* Append to extratextbuf. */
        nsi->bnotes = extratextbuf->len;
        appendBinaryStringInfo(extratextbuf, notes, notelen);
        nsi->enotes = extratextbuf->len;
    }
}                               /* cdbexplain_recvStatAcc_saveText */


/*
 * cdbexplain_recvStatWalker
 *    Update the given PlanState node's Instrument node with statistics
 *    received from qExecs.  Attach a CdbExplain_NodeSummary block to
 *    the Instrument node.  At a MotionState node, descend to child slice.
 */
CdbVisitOpt
cdbexplain_recvStatWalker(PlanState *planstate, void *context)
{
    CdbExplain_RecvStatCtx *ctx = (CdbExplain_RecvStatCtx *)context;

    /* If slice was dispatched to qExecs, and stats came back, grab 'em. */
    if (ctx->nmsgptr > 0)
    {
        Instrumentation        *instr = planstate->instrument;
        CdbExplain_StatHdr     *rsh;
        CdbExplain_StatInst    *rsi;
        CdbExplain_StatInst    *nsi;
        CdbExplain_NodeSummary *ns;
        CdbExplain_RecvStatAcc  ntuples;
        CdbExplain_RecvStatAcc  workmemused;
        CdbExplain_RecvStatAcc  workmemwanted;
        int                     imsgptr;
        int                     nInst;

        Insist(instr &&
               ctx->iStatInst < ctx->nStatInst);

        /* Allocate summary block and attach it to the Instrument node. */
        nInst = ctx->segindexMax + 1 - ctx->segindexMin;
        ns = (CdbExplain_NodeSummary *)palloc0(sizeof(*ns) - sizeof(ns->inst) +
                                               nInst*sizeof(ns->inst[0]));
        ns->segindex0 = ctx->segindexMin;
        ns->nInst = nInst;
        instr->cdbNodeSummary = ns;

        /* Initialize accumulators. */
        cdbexplain_recvStatAcc_init(&ntuples);
        cdbexplain_recvStatAcc_init(&workmemused);
        cdbexplain_recvStatAcc_init(&workmemwanted);

        /* Examine the statistics from each qExec. */
        for (imsgptr = 0; imsgptr < ctx->nmsgptr; imsgptr++)
        {
            /* Locate PlanState node's StatInst received from this qExec. */
            rsh = ctx->msgptrs[imsgptr];
            rsi = &rsh->inst[ctx->iStatInst];

            Insist(rsi->pstype == planstate->type &&
                   ns->segindex0 <= rsh->segindex &&
                   rsh->segindex < ns->segindex0 + ns->nInst);

            /* Locate this qExec's StatInst slot in node's NodeSummary block. */
            nsi = &ns->inst[rsh->segindex - ns->segindex0];

            /* Copy the StatInst to NodeSummary from dispatch result buffer. */
            *nsi = *rsi;

            /* Extra message text won't be copied unless qExec is a winner. */
            nsi->bnotes = nsi->enotes = 0;

            /* Update accumulators. */
            cdbexplain_recvStatAcc_upd(&ntuples, rsi->ntuples, rsh, rsi, nsi);
            cdbexplain_recvStatAcc_upd(&workmemused, rsi->workmemused, rsh, rsi, nsi);
            cdbexplain_recvStatAcc_upd(&workmemwanted, rsi->workmemwanted, rsh, rsi, nsi);
        }

        /* Save accumulated stats in summary block. */
        ns->ntuples = ntuples.agg;
        ns->workmemused = workmemused.agg;
        ns->workmemwanted = workmemwanted.agg;

        /* Put winner's stats into qDisp PlanState's Instrument node. */
        instr->starttime = ntuples.nsimax->starttime;
		instr->counter = ntuples.nsimax->counter;
		instr->firsttuple = ntuples.nsimax->firsttuple;
        instr->startup = ntuples.nsimax->startup;
        instr->total = ntuples.nsimax->total;
        instr->ntuples = ntuples.nsimax->ntuples;
        instr->nloops = ntuples.nsimax->nloops;
        instr->workmemused = ntuples.nsimax->workmemused;
        instr->workmemwanted = ntuples.nsimax->workmemwanted;
		instr->firststart = ntuples.nsimax->firststart;

        /* Save extra message text for the most interesting winners. */
        cdbexplain_recvStatAcc_saveText(&ntuples, ctx->extratextbuf);
        if (workmemused.agg.vmax > workmemwanted.agg.vmax)
            cdbexplain_recvStatAcc_saveText(&workmemused, ctx->extratextbuf);
        if (workmemwanted.agg.vcnt > 0)
            cdbexplain_recvStatAcc_saveText(&workmemwanted, ctx->extratextbuf);

        /* Advance to next node's entry in all of the StatInst arrays. */
        ctx->iStatInst++;
    }

    /* Motion operator?  Descend to next slice. */
    if (IsA(planstate, MotionState))
    {
        cdbexplain_recvExecStats(planstate->lefttree,
                                 ctx->dispatchResults,
                                 ((Motion *)planstate->plan)->motionID,
                                 ctx->extratextbuf);
        return CdbVisit_Skip;
    }

    return CdbVisit_Walk;
}                               /* cdbexplain_recvStatWalker */


/*
 * cdbexplain_collectExtraText
 *    Allow a node to supply additional text for its EXPLAIN ANALYZE report.
 *
 * Returns the starting offset of the extra message text from notebuf->data.
 * The caller can compute the length as notebuf->len minus the starting offset.
 * If the node did not provide any extra message text, the length will be 0.
 */
int
cdbexplain_collectExtraText(PlanState *planstate, StringInfo notebuf)
{
    int     bnotes = notebuf->len;

    /*
     * Invoke node's callback.  It may append to our notebuf and/or its own
     * cdbexplainbuf; and store final statistics in its Instrumentation node.
     */
    if (planstate->cdbexplainfun)
        planstate->cdbexplainfun(planstate, notebuf);

    /*
     * Append contents of node's extra message buffer.  This allows nodes to
     * contribute EXPLAIN ANALYZE info without having to set up a callback.
     */
    if (planstate->cdbexplainbuf &&
        planstate->cdbexplainbuf->len > 0)
    {
        /* If callback added to notebuf, make sure text ends with a newline. */
        if (bnotes < notebuf->len &&
            notebuf->data[notebuf->len-1] != '\n')
            appendStringInfoChar(notebuf, '\n');

        appendBinaryStringInfo(notebuf, planstate->cdbexplainbuf->data,
                               planstate->cdbexplainbuf->len);

        truncateStringInfo(planstate->cdbexplainbuf, 0);
    }

    return bnotes;
}                               /* cdbexplain_collectExtraText */


/*
 * cdbexplain_formatExtraText
 *    Format extra message text into the EXPLAIN output buffer.
 */
static void
cdbexplain_formatExtraText(StringInfo   str,
                           int          indent,
                           int          segindex,
                           const char  *notes,
                           int          notelen)
{
    const char *cp = notes;
    const char *ep = notes + notelen;

    /* Could be more than one line... */
    while (cp < ep)
    {
        const char *nlp = strchr(cp, '\n');
        const char *dp = nlp ? nlp : ep;

        /* Strip trailing whitespace. */
        while (cp < dp &&
               isspace(dp[-1]))
            dp--;

        /* Add to output buffer. */
        if (cp < dp)
        {
            appendStringInfoFill(str, 2*indent, ' ');
            if (segindex >= 0)
                appendStringInfo(str, "[seg%d] ", segindex);
            appendBinaryStringInfo(str, cp, dp-cp);
            appendStringInfoChar(str, '\n');
        }

        if (!nlp)
            break;
        cp = nlp+1;
    }
}                               /* cdbexplain_formatExtraText */


/*
 * cdbexplain_formatMemory
 *    Convert memory size to string from (double) bytes.
 */
static void
cdbexplain_formatMemory(char *outbuf, double bytes)
{
    sprintf(outbuf, "%.0fK bytes", floor((bytes + 1023.0)/1024.0));
}                               /* cdbexplain_formatMemory */


/*
 * cdbexplain_formatSeconds
 *    Convert time in seconds to readable string
 */
static void
cdbexplain_formatSeconds(char* outbuf, double seconds)
{
    double ms = seconds * 1000.0;

    sprintf(outbuf, "%.*f ms",
            (ms < 10.0 && ms != 0.0 && ms > -10.0) ? 3 : 0,
            ms);
}                               /* cdbexplain_formatSeconds */


/*
 * cdbexplain_formatSeg
 *    Convert segment id to string.
 */
static void
cdbexplain_formatSeg(char *outbuf, int segindex, int nInst)
{
    if (nInst > 1 &&
        segindex >= 0)
        sprintf(outbuf, " (seg%d)", segindex);
    else
        outbuf[0] = '\0';
}                               /* cdbexplain_formatSeg */


/*
 * cdbexplain_showExecStats
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 *
 * 'str' is the output buffer.
 * 'extratextbuf' is the buffer in which extra message text was saved by
 *      cdbexplain_recvExecStats().  Some more extra message text might be
 *      appended to this buffer.
 *
 * NB: Each PlanState node has an Instrumentation node.  If the operator was
 * dispatched to one or more qExecs which returned statistics, we'll find that
 * a CdbExplain_NodeSummary block has been attached to the Instrumentation
 * node by cdbexplain_recvExecStats(); and the Instrumentation node contains
 * the statistics from whichever qExec produced the most output rows.  There
 * are no CdbExplain_NodeSummary blocks for operators which were assigned to
 * the qDisp process for execution.
 */
void
cdbexplain_showExecStats(struct PlanState      *planstate,
                         struct StringInfoData *str,
                         int                    indent,
                         struct StringInfoData *extratextbuf,
                         instr_time             querystarttime)
{
    Instrumentation        *instr = planstate->instrument;
    CdbExplain_NodeSummary *ns = instr->cdbNodeSummary;
    instr_time              timediff;
    int                     i;

    CdbExplain_NodeSummary  nsqd;
    char        firstbuf[50];
    char        totalbuf[50];
    char        avgbuf[50];
    char        maxbuf[50];
    char        segbuf[50];
    char        startbuf[50];		

    /*
     * If slice was dispatched to a gang of one or more qExec processes,
     * and statistics were returned from at least one qExec, we expect
     * cdbexplain_recvExecStats() has attached a CdbExplain_NodeSummary block
     * to the Instrumentation node.  Otherwise, the slice was probably
     * executed here in the qDisp process (or not executed); fake up a
     * NodeSummary using the stats from the Instrumentation node.
     */
    if (!ns)
    {
        ns = &nsqd;
        ns->segindex0 = -1;
        ns->nInst = 1;

        /* Ask the node for its extra message text and final statistics. */
        memset(&ns->inst[0], 0, sizeof(ns->inst[0]));
        ns->inst[0].bnotes = cdbexplain_collectExtraText(planstate, extratextbuf);
        ns->inst[0].enotes = extratextbuf->len;

        /* Copy node's final statistics from Instrumentation to NodeSummary. */
        cdbexplain_agg_init1(&ns->ntuples, instr->ntuples, -1);
        cdbexplain_agg_init1(&ns->workmemused, instr->workmemused, -1);
        cdbexplain_agg_init1(&ns->workmemwanted, instr->workmemwanted, -1);
    }

    /*
     * Row counts.  Also, timings from the worker with the most output rows.
     */
    appendStringInfoFill(str, 2*indent, ' ');
    switch (planstate->type)
    {
        case T_HashState:
            appendStringInfoString(str, "Rows in:  ");
            break;
        case T_MotionState:
            /* Motion node stats are taken from the receiving (upper) slice. */
            appendStringInfoString(str, "Rows received:  ");
            break;
        default:
            appendStringInfoString(str, "Rows out:  ");
    }

    cdbexplain_formatSeg(segbuf, ns->ntuples.segmax, ns->nInst);
    if (ns->ntuples.vcnt > 1)
        appendStringInfo(str,
                         "Avg %.1f rows x %d workers.  Max %.0f rows%s",
                         ns->ntuples.vsum / ns->ntuples.vcnt,
                         ns->ntuples.vcnt,
                         ns->ntuples.vmax,
                         segbuf);
    else
        appendStringInfo(str,
                         "%.0f rows%s",
                         ns->ntuples.vmax,
                         segbuf);

    /* Time from this worker's first InstrStartNode() to its first result row */
    cdbexplain_formatSeconds(firstbuf, instr->startup);

    /* Time from this worker's first InstrStartNode() to end of its results */
    cdbexplain_formatSeconds(totalbuf, instr->total);

    /* Show elapsed time just once if they are the same. */
    if (strcmp(firstbuf, totalbuf) != 0)
        appendStringInfo(str,
                         " with %s to first row, %s to end",
                         firstbuf,
                         totalbuf);
    else
        appendStringInfo(str,
                         " with %s to end",
                         totalbuf);

    /* Number of rescans */
    if (instr->nloops > 1)
        appendStringInfo(str, " of %.0f scans", instr->nloops);
	
	/* Time from start of query on qDisp to this worker's first result row */
	if (!(INSTR_TIME_IS_ZERO(instr->firststart)))
	{
        InstrTimeSubtract(&timediff, &instr->firststart, &querystarttime);
		cdbexplain_formatSeconds(startbuf, INSTR_TIME_GET_DOUBLE(timediff));
		appendStringInfo(str, ", start offset by %s", startbuf);
	}

    appendStringInfoString(str, ".\n");

    /*
     * Actual work_mem used.
     */
    if (ns->workmemused.vcnt > 0)
    {
        appendStringInfoFill(str, 2*indent, ' ');
        cdbexplain_formatMemory(maxbuf, ns->workmemused.vmax);
        if (ns->workmemused.vcnt == 1)
            appendStringInfo(str,
                             "Work_mem used:  %s.\n",
                             maxbuf);
        else
        {
            cdbexplain_formatSeg(segbuf, ns->workmemused.segmax, ns->nInst);
            cdbexplain_formatMemory(avgbuf, ns->workmemused.vsum / ns->workmemused.vcnt);
            appendStringInfo(str,
                             "Work_mem used:  %s avg, %s max%s.\n",
                             avgbuf,
                             maxbuf,
                             segbuf);
        }
    }

    /*
     * What value of work_mem would suffice to eliminate workfile I/O?
     */
    if (ns->workmemwanted.vcnt > 0)
    {
        appendStringInfoFill(str, 2*indent, ' ');
        cdbexplain_formatMemory(maxbuf, ns->workmemwanted.vmax);
        if (ns->nInst == 1)
            appendStringInfo(str,
                "Work_mem wanted: %s would avert workfile I/O.\n",
                maxbuf);
        else
        {
            cdbexplain_formatMemory(avgbuf, ns->workmemwanted.vsum / ns->workmemwanted.vcnt);
            cdbexplain_formatSeg(segbuf, ns->workmemwanted.segmax, ns->nInst);
            appendStringInfo(str,
                             "Work_mem wanted: %s avg, %s max%s "
                             "would avert workfile I/O affecting %d workers.\n",
                             avgbuf,
                             maxbuf,
                             segbuf,
                             ns->workmemwanted.vcnt);
        }
    }

    /*
     * Extra message text.
     */
    for (i = 0; i < ns->nInst; i++)
    {
        CdbExplain_StatInst    *nsi = &ns->inst[i];

        if (nsi->bnotes < nsi->enotes)
            cdbexplain_formatExtraText(str,
                                       indent,
                                       (ns->nInst == 1) ? -1
                                                        : ns->segindex0 + i,
                                       extratextbuf->data + nsi->bnotes,
                                       nsi->enotes - nsi->bnotes);
    }

    /*
     * Dump stats for all workers.
     */
	if (mpp_enable_explain_allstat &&
        ns->segindex0 >= 0 && 
        ns->nInst > 0)
	{
		/* create a header for all stats: separate each individual
		 * stat by an underscore, separate the grouped stats for each
		 * node by a slash
		 */
        appendStringInfoFill(str, 2*indent, ' ');
		appendStringInfoString(str, 
                               "allstat: "
        /*	 "seg_starttime_firststart_counter_firsttuple_startup_total_ntuples_nloops" */
						       "seg_firststart_ntuples");

        for (i = 0; i < ns->nInst; i++)
		{
            CdbExplain_StatInst    *nsi = &ns->inst[i];

	        if (INSTR_TIME_IS_ZERO(nsi->firststart))
                continue;

	        /* Time from start of query on qDisp to worker's first result row */
            InstrTimeSubtract(&timediff, &nsi->firststart, &querystarttime);
	        cdbexplain_formatSeconds(startbuf, INSTR_TIME_GET_DOUBLE(timediff));

			appendStringInfo(str, 
                             "/%d_%s_%.0f",
                             ns->segindex0 + i,
							 startbuf, 
                             nsi->ntuples);
		}
		appendStringInfoString(str, "//end\n");
	}
}                               /* cdbexplain_showExecStats */

