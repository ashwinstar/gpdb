/*-------------------------------------------------------------------------
 *
 * cdbexplain.c
 *    Functions supporting the MPP EXPLAIN command
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
	instr_time	firststart;		/* Start time of first iteration of node */
	instr_time	counter;		/* Accumulated runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
    double      startup;        /* Total startup time (in seconds) */
    double      total;          /* Total total time (in seconds) */
    double      ntuples;        /* Total tuples produced */
    double      nloops;         /* # of run cycles for this node */
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

    /* Array [0..ninst-1] of StatInst entries is appended starting here */
    CdbExplain_StatInst inst[1];

    /* extra text is appended after that */
} CdbExplain_StatHdr;


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
    int                     nmsgptr;
    CdbExplain_StatHdr    **msgptrs;
    CdbDispatchResults     *dispatchResults;
} CdbExplain_RecvStatCtx;


static CdbVisitOpt
cdbexplain_sendStatWalker(PlanState *planstate, void *context);
static CdbVisitOpt
cdbexplain_recvStatWalker(PlanState *planstate, void *context);

static int
cdbexplain_diff_time(char* outbuf, instr_time starttime, instr_time endtime);


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
 * cdbexplain_recvExecStats
 *    Called by qDisp to transfer a slice's EXPLAIN ANALYZE statistics
 *    from the CdbDispatchResults structures to the PlanState tree.
 *    Recursively does the same for slices that are descendants of the
 *    one specified.
 */
void
cdbexplain_recvExecStats(struct PlanState          *planstate,
                         struct CdbDispatchResults *dispatchResults,
                         int                        sliceIndex)
{
    CdbDispatchResult      *dispatchResultBeg;
    CdbDispatchResult      *dispatchResultEnd;
    CdbExplain_RecvStatCtx  ctx;
    int                     mppSegmentCount = getMPPSegmentCount();
    int                     iDispatch;
    int                     nDispatch;

    if (!planstate ||
        !planstate->instrument)
        return;

    /* Initialize treewalk context. */
    ctx.iStatInst = 0;
    ctx.nStatInst = 0;
    ctx.nmsgptr = 0;
    ctx.msgptrs = NULL;
    ctx.dispatchResults = dispatchResults;

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

        /* Save message ptr for easy reference. */
        ctx.msgptrs[ctx.nmsgptr] = hdr;
        ctx.nmsgptr++;
    }

    /* Stuff statistics into each PlanState node's Instrumentation node. */
    ExecStateTreeWalker(planstate, cdbexplain_recvStatWalker, NULL, &ctx);

    /* Make sure we visited the right number of PlanState nodes. */
    Insist(ctx.iStatInst == ctx.nStatInst);

    /* Clean up. */
    if (ctx.msgptrs)
        pfree(ctx.msgptrs);
}                               /* cdbexplain_recvExecStats */


/*
 * cdbexplain_recvStatWalker
 *    Update the given PlanState node's Instrument node with statistics
 *    received from qExecs.  At a MotionState node, descend to child slice.
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
	CdbExplain_StatInst    *pAllStats = NULL;

    /* If slice was dispatched to qExecs, and stats came back, grab 'em. */
    if (ctx->nmsgptr > 0)
    {
        Insist(instr &&
               ctx->iStatInst < ctx->nStatInst);

        /* Start with one qExec's statistics for the node. */
        shMax = ctx->msgptrs[0];
        siMax = &shMax->inst[ctx->iStatInst];
        ntMax = siMax->ntuples;

        instr->cdbGangTuples = siMax->ntuples;
        instr->cdbWorkers = (siMax->ntuples > 0) ? 1 : 0;

        Insist(siMax->pstype == planstate->type);

		instr->numStats = ctx->nmsgptr;

		if (ctx->nmsgptr > 1)
		{
			pAllStats = (CdbExplain_StatInst*)
					palloc0(ctx->nmsgptr * sizeof(CdbExplain_StatInst));
			/* XXX XXX: need a better way to handle this!! */
			instr->pAllStats = (void *)pAllStats;

			{
				CdbExplain_StatInst    *si = siMax;

				pAllStats[0].starttime = si->starttime;
				pAllStats[0].firststart = si->firststart;
				pAllStats[0].counter = si->counter;
				pAllStats[0].firsttuple = si->firsttuple;

				pAllStats[0].startup = si->startup;
				pAllStats[0].total = si->total;
				pAllStats[0].ntuples = si->ntuples;
				pAllStats[0].nloops = si->nloops;
			}

		}

        /* Examine the statistics from the rest of the qExecs. */
        for (imsgptr = 1; imsgptr < ctx->nmsgptr; imsgptr++)
        {
            CdbExplain_StatHdr     *sh = ctx->msgptrs[imsgptr];
            CdbExplain_StatInst    *si = &sh->inst[ctx->iStatInst];
            double                  nt = si->ntuples;

            Insist(siMax->pstype == si->pstype);

			pAllStats[imsgptr].starttime = si->starttime;
			pAllStats[imsgptr].firststart = si->firststart;
			pAllStats[imsgptr].counter = si->counter;
			pAllStats[imsgptr].firsttuple = si->firsttuple;

			pAllStats[imsgptr].startup = si->startup;
			pAllStats[imsgptr].total = si->total;
			pAllStats[imsgptr].ntuples = si->ntuples;
			pAllStats[imsgptr].nloops = si->nloops;

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
                sh->segindex > shMax->segindex)
                continue;

            shMax = sh;
            siMax = si;
            ntMax = nt;
        }

        /* Transfer winner's stats into qDisp PlanState's Instrument node. */
        instr->starttime = siMax->starttime;
		instr->firststart = siMax->firststart;
		instr->counter = siMax->counter;
		instr->firsttuple = siMax->firsttuple;

        instr->startup = siMax->startup;
        instr->total = siMax->total;
        instr->ntuples = siMax->ntuples;
        instr->nloops = siMax->nloops;

        /* Compute ptr to winner's extra message text (if any). */
        instr->notes = NULL;
        if (siMax->bnotes < siMax->enotes)
        {
            instr->notes = (char *)shMax + shMax->bnotes + siMax->bnotes;
            Insist(shMax->bnotes + siMax->enotes < shMax->enotes &&
                   instr->notes[siMax->enotes-siMax->bnotes] == '\0');
        }

        /* Store segment id of the winning qExec. */
        instr->cdbSegIndex = shMax->segindex;

        /* Advance to next node's entry in all of the StatInst arrays. */
        ctx->iStatInst++;
    }

    /* Motion operator?  Descend to next slice. */
    if (IsA(planstate, MotionState))
    {
        cdbexplain_recvExecStats(planstate->lefttree,
                                 ctx->dispatchResults,
                                 ((Motion *)planstate->plan)->motionID);
        return CdbVisit_Skip;
    }

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

	si->starttime = instr->starttime;
	si->firststart = instr->firststart;
	si->counter = instr->counter;
	si->firsttuple = instr->firsttuple;

    si->startup = instr->startup;
    si->total = instr->total;
    si->ntuples = instr->ntuples;
    si->nloops = instr->nloops;

    /* Give node a chance to append its extra message text. */
    if (planstate->cdbexplainmsg)
    {
        si->bnotes = ctx->notebuf.len;
        planstate->cdbexplainmsg(planstate, &ctx->notebuf);
        if (si->bnotes < ctx->notebuf.len)
        {
            si->enotes = ctx->notebuf.len;
            appendStringInfoChar(&ctx->notebuf, '\0');
        }
    }

    /* Append to message. */
    appendBinaryStringInfo(&ctx->buf, (char *)si, sizeof(*si));
    ctx->hdr.nInst++;

    /* Don't descend across a slice boundary. */
    if (IsA(planstate, MotionState))
        return CdbVisit_Skip;

    return CdbVisit_Walk;
}                               /* cdbexplain_sendStatWalker */

/*
 * cdbexplain_format_time
 *    common format routine to convert time in msec to readable string
 */
void
cdbexplain_format_time(char* outbuf, double totalmsecs)
{
    if (totalmsecs < 20 && totalmsecs != 0)
        sprintf(outbuf, "%.3f ms", totalmsecs);
    else
        sprintf(outbuf, "%.0f ms", totalmsecs);
}

int
cdbexplain_diff_time(char* outbuf, instr_time starttime, instr_time endtime)
{
	if (INSTR_TIME_IS_ZERO(endtime))
	{
        sprintf(outbuf, "0 ms");			
		return 0;
	}

	{
		double      myoffset;
		instr_time	timediff = endtime;
		instr_time  firststart = starttime;

		/* compute time difference -- (instr->firststart - query firststart) */
#ifndef WIN32
		timediff.tv_sec  -= firststart.tv_sec;
		timediff.tv_usec -= firststart.tv_usec;

		/* Normalize after each add to avoid overflow/underflow of tv_usec */
		while (timediff.tv_usec < 0)
		{
			timediff.tv_usec += 1000000;
			timediff.tv_sec--;
		}
		while (timediff.tv_usec >= 1000000)
		{
			timediff.tv_usec -= 1000000;
			timediff.tv_sec++;
		}
#else							/* WIN32 */
		timediff.QuadPart -= firststart.QuadPart ;
#endif
		
		/* get time in seconds, convert to msec */
		myoffset = 1000.0 * INSTR_TIME_GET_DOUBLE(timediff);

		cdbexplain_format_time(outbuf, myoffset);
	}
	return 1;
}

/*
 * cdbexplain_showExecStats
 *    Called by qDisp process to format a node's EXPLAIN ANALYZE statistics.
 *
 * 'str' is the output buffer.
 * 'workbuf' is a scratch buffer for string manipulation.
 * 'slice' is the current node's slice table entry.
 */
void
cdbexplain_showExecStats(struct PlanState      *planstate,
                         struct StringInfoData *str,
                         struct StringInfoData *workbuf,
                         struct Slice          *slice,
                         int                    indent,
						 Instrumentation       *pFirst)
{
    Instrumentation    *instr = planstate->instrument;
    double      startup = instr->startup * 1000.0;  /* convert secs to ms */
    double      total = instr->total * 1000.0;      /* convert secs to ms */
    char        firstbuf[32];
    char        totalbuf[32];
    char       *notes;
    int         i;
    int         saveworkbuflen = workbuf->len;
	/* get the query's start time from pFirst */
	instr_time	firststart = pFirst->firststart;

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

    /* Elapsed time (ms) to fetch first row */
	cdbexplain_format_time(firstbuf, startup);

    /* Elapsed time (ms) to fetch all rows */
	cdbexplain_format_time(totalbuf, total);

    /* Show elapsed time just once if they are the same. */
    appendStringInfo(str, "%s ", firstbuf);
    if (strcmp(firstbuf, totalbuf) != 0)
        appendStringInfo(str, "to first row, %s ", totalbuf);
    appendStringInfoString(str, "to end");

    /* Number of rescans */
    if (instr->nloops > 1)
        appendStringInfo(str, " of %.0f scans", instr->nloops);
	
	/* if the current node has started (firststart != 0), determine
	 * when it started in relation to the start of the query
	 */
	if (!(INSTR_TIME_IS_ZERO(instr->firststart)))
	{
		double      myoffset;
		char        diffbuf[32];
		instr_time	timediff = instr->firststart;
		/* compute time difference -- (instr->firststart - query firststart) */
#ifndef WIN32
		timediff.tv_sec  -= firststart.tv_sec;
		timediff.tv_usec -= firststart.tv_usec;

		/* Normalize after each add to avoid overflow/underflow of tv_usec */
		while (timediff.tv_usec < 0)
		{
			timediff.tv_usec += 1000000;
			timediff.tv_sec--;
		}
		while (timediff.tv_usec >= 1000000)
		{
			timediff.tv_usec -= 1000000;
			timediff.tv_sec++;
		}
#else							/* WIN32 */
		timediff.QuadPart -= firststart.QuadPart ;
#endif
		
		/* get time in seconds, convert to msec */
		myoffset = 1000.0 * INSTR_TIME_GET_DOUBLE(timediff);

		cdbexplain_format_time(diffbuf, myoffset);
		appendStringInfo(str, ", start offset by %s", diffbuf);

		/* XXX XXX: why is instr->numStats = 26? */
		if (mpp_enable_explain_allstat && instr->numStats && instr->pAllStats)
		{
			int ii;
			CdbExplain_StatInst *pAllStats =
					(CdbExplain_StatInst*) instr->pAllStats;

			/* create a header for all stats: separate each individual
			 * stat by an underscore, separate the grouped stats for each
			 * node by a slash
			 */
			appendStringInfo(str, ", all stats: %s",
/*	 "starttime_firststart_counter_firsttuple_startup_total_ntuples_nloops" */
							 "firststart_ntuples"
					);
			for (ii = 0; ii < instr->numStats; ii++)
			{
				char tbuf[32];

				cdbexplain_diff_time(tbuf, firststart,
									 pAllStats[ii].firststart);
				appendStringInfo(str, "/%s_%f",
								 tbuf, pAllStats[ii].ntuples
						);
				
			}
			appendStringInfo(str, " end all stats");
		}
		
	} /* end if started */

    appendStringInfoString(str, ".\n");

    /* If slice was executed here on qDisp, get node's extra message text. */
    notes = instr->notes;
    if (planstate->cdbexplainmsg &&
        !sliceRunsOnQE(slice))
    {
        int     bnotes = workbuf->len;

        planstate->cdbexplainmsg(planstate, workbuf);

        if (bnotes < workbuf->len)
            notes = (char *)workbuf->data + bnotes;
        else
            notes = NULL;
    }

    /* Display extra message text. */
    if (notes)
    {
        char   *cp = notes;

        /* Could be more than one line... */
        for (;;)
        {
            char   *nlp = strchr(cp, '\n');
            char   *dp = nlp ? nlp : cp + strlen(cp);

            /* Strip trailing whitespace. */
            while (cp < dp &&
                   isspace(dp[-1]))
                dp--;

            /* Add to output buffer. */
            if (cp < dp)
            {
                for (i = 0; i < indent; i++)
                    appendStringInfo(str, "  ");
                if (instr->cdbWorkers > 1)
                    appendStringInfo(str, "(seg%d): ", instr->cdbSegIndex);
                appendBinaryStringInfo(str, cp, dp-cp);
                appendStringInfoChar(str, '\n');
            }

            if (!nlp)
                break;
            cp = nlp+1;
        }
    }

    /* Clean up workbuf. */
    truncateStringInfo(workbuf, saveworkbuflen);
}                               /* cdbexplain_showExecStats */

