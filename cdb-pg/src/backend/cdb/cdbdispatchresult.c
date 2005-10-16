/*-------------------------------------------------------------------------
 *
 * cdbdispatchresult.c
 *    Functions to dispatch commands to QExecutors
 *
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/stringinfo.h"         /* StringInfoData */
#include "utils/guc.h"              /* log_min_messages */

#include "cdb/cdbdispatchresult.h"  /* me */


/*--------------------------------------------------------------------*/

/* Construct a CdbDispatchResult object. */
void
cdbdisp_initResult(CdbDispatchResult           *dispatchResult,
                   SegmentDatabaseDescriptor   *segdbDesc)
{
    dispatchResult->segdbDesc = segdbDesc;

    /* Allocate error message buffer and result buffer. */
    initPQExpBuffer(&dispatchResult->resultbuf);
    initPQExpBuffer(&dispatchResult->error_message);

    /* Reset summary indicators. */
    cdbdisp_resetResult(dispatchResult);
}                               /* cdbdisp_initResult */


/* Destroy a CdbDispatchResult object. */
void
cdbdisp_termResult(CdbDispatchResult  *dispatchResult)
{
    dispatchResult->segdbDesc = NULL;

    /* Free the PGresult objects. */
    cdbdisp_resetResult(dispatchResult);

    /* Free the error message buffer and result buffer. */
    termPQExpBuffer(&dispatchResult->resultbuf);
    termPQExpBuffer(&dispatchResult->error_message);
}                               /* cdbdisp_termResult */


/* Reset a CdbDispatchResult object for possible reuse. */
void
cdbdisp_resetResult(CdbDispatchResult  *dispatchResult)
{
    PQExpBuffer buf = &dispatchResult->resultbuf;
    PGresult  **begp = (PGresult **)buf->data;
    PGresult  **endp = (PGresult **)(buf->data + buf->len);
    PGresult  **p;

    /* Free the PGresult objects. */
    for (p = begp; p < endp; ++p)
    {
        Assert(*p != NULL);
        PQclear(*p);
    }

    /* Reset summary indicators. */
    dispatchResult->errcode = 0;
    dispatchResult->errindex = -1;
    dispatchResult->okindex = -1;

    /* Reset progress indicators. */
    dispatchResult->hasDispatched = false;
    dispatchResult->stillRunning = false;
    dispatchResult->requestedCancel = false;
    dispatchResult->wasCanceled = false;

    /* Empty (but don't free) the error message buffer and result buffer. */
    resetPQExpBuffer(&dispatchResult->resultbuf);
    resetPQExpBuffer(&dispatchResult->error_message);
}                               /* cdbdisp_resetResult */


/* Transfer connection error messages to dispatchResult from segdbDesc. */
bool                            /* returns true if segdbDesc had err info */
cdbdisp_mergeConnectionErrors(CdbDispatchResult            *dispatchResult,
                              SegmentDatabaseDescriptor    *segdbDesc)
{
    if (!segdbDesc)
        return false;
    if (segdbDesc->errcode == 0 &&
        segdbDesc->error_message.len == 0)
        return false;

    /* Error code should always be accompanied by text and vice-versa. */
    Assert(segdbDesc->errcode != 0 && segdbDesc->error_message.len > 0);

    /* Save ERRCODE_xxx if first error. */
    if (dispatchResult->errcode == 0)
        dispatchResult->errcode = segdbDesc->errcode;

    /* Append error message text. */
    appendPQExpBufferStr(&dispatchResult->error_message,
                         segdbDesc->error_message.data);

    /* Reset connection object's error info. */
    segdbDesc->errcode = 0;
    segdbDesc->error_message.len = 0;
    segdbDesc->error_message.data[0] = '\0';

    return true;
}                               /* cdbdisp_mergeConnectionErrors */


/* Store a PGresult object ptr in the result buffer.
 * NB: Caller must not PQclear() the PGresult object.
 */
void
cdbdisp_appendResult(CdbDispatchResult *dispatchResult,
                     PGresult          *res)
{
    Assert(dispatchResult && res);
    appendBinaryPQExpBuffer(&dispatchResult->resultbuf, (char *)&res, sizeof(res));
}                               /* cdbdisp_appendResult */


/* Return the i'th PGresult object ptr (if i >= 0), or
 * the n+i'th one (if i < 0), or NULL (if i out of bounds).
 * NB: Caller must not PQclear() the PGresult object.
 */
PGresult *
cdbdisp_getPGresult(CdbDispatchResult *dispatchResult, int i)
{
    if (dispatchResult)
    {
        PQExpBuffer buf = &dispatchResult->resultbuf;
        PGresult  **begp = (PGresult **)buf->data;
        PGresult  **endp = (PGresult **)(buf->data + buf->len);
        PGresult  **p = (i >= 0) ? &begp[i] : &endp[i];

        if (p >= begp && p < endp)
        {
            Assert(*p != NULL);
            return *p;
        }
    }
    return NULL;
}                               /* cdbdisp_getPGresult */


/* Return the number of PGresult objects in the result buffer. */
int
cdbdisp_numPGresult(CdbDispatchResult  *dispatchResult)
{
    return dispatchResult ? dispatchResult->resultbuf.len / sizeof(PGresult *)
                          : 0;
}                               /* cdbdisp_numPGresult */


/* Remove all of the PGresult ptrs from a CdbDispatchResult object
 * and place them into an array provided by the caller.  The caller
 * becomes responsible for PQclear()ing them.  Returns the number of
 * PGresult ptrs placed in the array.
 */
int
cdbdisp_snatchPGresults(CdbDispatchResult  *dispatchResult,
                        PGresult          **pgresultptrs,
                        int                 maxresults)
{
    PQExpBuffer     buf = &dispatchResult->resultbuf;
    PGresult      **begp = (PGresult **)buf->data;
    PGresult      **endp = (PGresult **)(buf->data + buf->len);
    PGresult      **p;
    int             nresults = 0;

    /* Snatch the PGresult objects. */
    for (p = begp; p < endp; ++p)
    {
        Assert(*p != NULL);
        Assert(nresults < maxresults);
        pgresultptrs[nresults++] = *p;
        *p = NULL;
    }

    /* Empty our PGresult array. */
    resetPQExpBuffer(buf);
    dispatchResult->errindex = -1;
    dispatchResult->okindex = -1;

    return nresults;
}                               /* cdbdisp_snatchPGresults */


/* Display a CdbDispatchResult in the log for debugging.
 * Call only from main thread, during or after cdbdisp_checkDispatchResults.
 */
void
cdbdisp_debugDispatchResult(CdbDispatchResult  *dispatchResult,
                            int                 elevel_error,
                            int                 elevel_success)
{
    SegmentDatabaseDescriptor  *segdbDesc;
    const char *whoami;
    PGresult   *pgresult;
    char        sqlstate[8];

    /* Skip if user has messages turned off. */
    if (elevel_error < log_min_messages &&
        elevel_success < log_min_messages)
        return;

    segdbDesc = dispatchResult->segdbDesc;
    whoami = segdbDesc ? segdbDesc->whoami : "not connected";

    /* Connection error? */
    if (segdbDesc &&
        segdbDesc->error_message.len > 0)
	{
        cdbdisp_errcode_to_sqlstate(segdbDesc->errcode, sqlstate);
        elog(elevel_error, "DispatchResult: %s -> (%s) %s",
             whoami,
             sqlstate,
             segdbDesc->error_message.data);
	}

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message.len > 0)
    {
        cdbdisp_errcode_to_sqlstate(dispatchResult->errcode, sqlstate);
        elog(elevel_error, "DispatchResult: %s -> (%s) %s",
             whoami,
             sqlstate,
             dispatchResult->error_message.data);
    }

    /* QE error or libpq error */
    if (dispatchResult->errindex >= 0)
    {
        pgresult = cdbdisp_getPGresult(dispatchResult, dispatchResult->errindex);
        cdbdisp_errcode_to_sqlstate(dispatchResult->errcode, sqlstate);
        ereport(elevel_error,
                (errmsg("DispatchResult: %s -> (%s) %s %s",
                        whoami,
                        sqlstate,
                        PQresStatus(PQresultStatus(pgresult)),
                        PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY)),
                 errdetail("(%s:%s) %s",
                        PQresultErrorField(pgresult, PG_DIAG_SOURCE_FILE),
                        PQresultErrorField(pgresult, PG_DIAG_SOURCE_LINE),
                        PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL)) ));
    }

    /* QE success */
    else if (dispatchResult->okindex >= 0)
    {
        pgresult = cdbdisp_getPGresult(dispatchResult, dispatchResult->okindex);
        elog(elevel_success, "DispatchResult: %s ok ->  %s",
             whoami,
             pgresult ? PQcmdStatus(pgresult) : "(no cmdStatus)");
    }

    /* Should have either an error code or an ok result. */
    if (!dispatchResult->errcode &&
        dispatchResult->okindex < 0)
    {
        elog(elevel_error, "DispatchResult: %s -> No ending status",
             whoami);
    }
}                               /* cdbdisp_debugDispatchResult */


/* Format a CdbDispatchResult into a StringInfo buffer provided by caller.
 * If verbose = true, reports all info; else reports at most one error.
 */
void
cdbdisp_dumpDispatchResult(CdbDispatchResult       *dispatchResult,
                           bool                     verbose,
                           struct StringInfoData   *buf)
{
    SegmentDatabaseDescriptor  *segdbDesc = dispatchResult->segdbDesc;
    const char     *whoami = segdbDesc ? segdbDesc->whoami : "not connected";
    int             ires, nres;

    if (!dispatchResult ||
        !buf)
        return;

    /* Connection error? */
    if (segdbDesc &&
        segdbDesc->error_message.len > 0)
	{
        if (buf->len > 0 &&
            buf->data[buf->len-1] != '\n')
            appendStringInfoChar(buf, '\n');

        appendStringInfo(buf, "(%s) %s",
                         whoami,
                         segdbDesc->error_message.data);
        if (!verbose)
            return;
	}

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message.len > 0)
    {
        if (buf->len > 0 &&
            buf->data[buf->len-1] != '\n')
            appendStringInfoChar(buf, '\n');

        appendStringInfo(buf, "(%s) %s",
                         whoami,
                         dispatchResult->error_message.data);
        if (!verbose)
            return;
    }

    nres = cdbdisp_numPGresult(dispatchResult);
    for (ires = 0; ires < nres; ++ires)
    {
        PGresult       *pgresult = cdbdisp_getPGresult(dispatchResult, ires);
        ExecStatusType  resultStatus = PQresultStatus(pgresult);
        bool            success = (resultStatus == PGRES_COMMAND_OK ||
                                   resultStatus == PGRES_TUPLES_OK);

        /* Keep quiet if QE was canceled because of a condition for
         * which another message should appear elsewhere.  For example,
         * if one QE reports an error, the QD may cancel other active QEs.
         * Only the original error should be shown, if 'verbose' is false.
         */

        /* QE error or libpq error */
        if (!success)
        {
            char   *pri = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY);
            char   *dtl = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL);

            if (buf->len > 0 &&
                buf->data[buf->len-1] != '\n')
                appendStringInfoChar(buf, '\n');

            appendStringInfo(buf, "(%s) %s", whoami, pri);
            if (dtl)
                appendStringInfo(buf, "\t %s", dtl);

            if (!verbose)
                return;
        }

        /* QE success */
        else if (verbose)
        {
            if (buf->len > 0 &&
                buf->data[buf->len-1] != '\n')
                appendStringInfoChar(buf, '\n');

            appendStringInfo(buf, "(%s) %s\n",
                             whoami,
                             pgresult ? PQcmdStatus(pgresult) : "");
        }
    }
}                               /* cdbdisp_dumpDispatchResult */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_makeDispatchResults:
 * Allocates a CdbDispatchResults object in the current memory context.
 * The caller is responsible for calling DestroyCdbDispatchResults on the returned
 * pointer when done using it.
 */
CdbDispatchResults *
cdbdisp_makeDispatchResults(int resultCount, bool cancelOnError)
{
    CdbDispatchResults *results = palloc0(sizeof(*results));

    results->resultArray = palloc0(resultCount * sizeof(results->resultArray[0]));
    results->resultCount = resultCount;
    results->iFirstError = -1;
    results->errcode = 0;
    results->cancelOnError = cancelOnError;

    return results;
}                               /* cdbdisp_makeDispatchResults */


/*
 * cdbdisp_destroyDispatchResults:
 * Frees all memory allocated in CdbDispatchResults struct.
 */
void
cdbdisp_destroyDispatchResults(CdbDispatchResults * results)
{
    if (!results)
        return;

    if (results->resultArray)
    {
        int i;
        for (i = 0; i < results->resultCount; i++)
        {
            cdbdisp_termResult(&results->resultArray[i]);
        }
        pfree(results->resultArray);
		results->resultArray = NULL;
    }

    pfree(results);
}                               /* cdbdisp_destroyDispatchResults */


/* Return sum of the cmdTuples values from CdbDispatchResult
 * entries that have a successful PGresult.
 * Used to determine the row counts for insert, update, and delete commands.
 */
int
cdbdisp_sumCmdTuples(CdbDispatchResults *results)
{
    CdbDispatchResult  *dispatchResult;
    PGresult           *pgresult;
    int                 sum = 0;
    int                 i;

    if (!results)
        return 0;

    for (i = 0; i < results->resultCount; i++)
    {
        dispatchResult = &results->resultArray[i];
        pgresult = cdbdisp_getPGresult(dispatchResult, dispatchResult->okindex);
        if (pgresult)
        {
            char   *cmdTuples = PQcmdTuples(pgresult);
            if (cmdTuples)
                sum += atoi(cmdTuples);
        }
    }

    return sum;
}                               /* cdbdisp_sumCmdTuples */


/*--------------------------------------------------------------------*/


/* Convert compact error code (ERRCODE_xxx) to 5-char SQLSTATE string,
 * and put it into a 6-char buffer provided by caller.
 */
char *                          /* returns outbuf+5 */
cdbdisp_errcode_to_sqlstate(int errcode, char outbuf[6])
{
    int i;
    for (i = 0; i < 5; ++i)
    {
        outbuf[i] = PGUNSIXBIT(errcode);
        errcode >>= 6;
    }
    outbuf[5] = '\0';
    return &outbuf[5];
}                               /* cdbdisp_errcode_to_sqlstate */


/* Convert SQLSTATE string to compact error code (ERRCODE_xxx). */
int
cdbdisp_sqlstate_to_errcode(const char *sqlstate)
{
    return MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
        sqlstate[3], sqlstate[4]);
}                               /* cdbdisp_sqlstate_to_errcode */


/*--------------------------------------------------------------------*/


