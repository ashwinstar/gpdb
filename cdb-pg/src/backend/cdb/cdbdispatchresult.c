/*-------------------------------------------------------------------------
 *
 * cdbdispatchresult.c
 *    Functions to dispatch commands to QExecutors
 *
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <pthread.h>

#include "libpq-fe.h"               /* prerequisite for libpq-int.h */
#include "libpq-int.h"              /* PQExpBufferData */

#include "lib/stringinfo.h"         /* StringInfoData */
#include "utils/guc.h"              /* log_min_messages */

#include "cdb/cdbconn.h"            /* SegmentDatabaseDescriptor */
#include "cdb/cdbdispatchresult.h"  /* me */


/* This mutex serializes writes by dispatcher threads to the
 * iFirstError and errcode fields of CdbDispatchResults objects.
 */
static pthread_mutex_t  setErrcodeMutex = PTHREAD_MUTEX_INITIALIZER;


/*--------------------------------------------------------------------*/

/* Make sure buffer has no trailing newline, whitespace, etc. */
static void
noTrailingNewline(StringInfo buf)
{
    while (buf->len > 0 &&
           buf->data[buf->len-1] <= ' ' &&
           buf->data[buf->len-1] > '\0')
        buf->data[--buf->len] = '\0';
}                               /* noTrailingNewline */

static void
noTrailingNewlinePQ(PQExpBuffer buf)
{
    while (buf->len > 0 &&
           buf->data[buf->len-1] <= ' ' &&
           buf->data[buf->len-1] > '\0')
        buf->data[--buf->len] = '\0';
}                               /* noTrailingNewlinePQ */


/* If buffer is nonempty, make sure it has exactly one trailing newline. */
static void
oneTrailingNewline(StringInfo buf)
{
    noTrailingNewline(buf);
    if (buf->len > 0)
        appendStringInfoChar(buf, '\n');
}                               /* oneTrailingNewline */

static void
oneTrailingNewlinePQ(PQExpBuffer buf)
{
    noTrailingNewlinePQ(buf);
    if (buf->len > 0)
        appendPQExpBufferChar(buf, '\n');
}                               /* oneTrailingNewlinePQ */


/*--------------------------------------------------------------------*/

/* Create a CdbDispatchResult object, appending it to the
 * resultArray of a given CdbDispatchResults object.
 */
CdbDispatchResult *
cdbdisp_makeResult(struct CdbDispatchResults           *meleeResults,
                   struct SegmentDatabaseDescriptor    *segdbDesc,
                   int                                  sliceIndex)
{
    CdbDispatchResult  *dispatchResult;
    int                 meleeIndex;

    Assert(meleeResults &&
           meleeResults->resultArray &&
           meleeResults->resultCount < meleeResults->resultCapacity);

    /* Allocate a slot for the new CdbDispatchResult object. */
    meleeIndex = meleeResults->resultCount++;
    dispatchResult = &meleeResults->resultArray[meleeIndex];

    /* Initialize CdbDispatchResult. */
    dispatchResult->meleeResults = meleeResults;
    dispatchResult->meleeIndex = meleeIndex;
    dispatchResult->segdbDesc = segdbDesc;
    dispatchResult->resultbuf = createPQExpBuffer();
    dispatchResult->error_message = NULL;

    /* Reset summary indicators. */
    cdbdisp_resetResult(dispatchResult);

    /* Update slice map entry. */
    if (sliceIndex >= 0 &&
        sliceIndex < meleeResults->sliceCapacity)
    {
        CdbDispatchResults_SliceInfo *si = &meleeResults->sliceMap[sliceIndex];
        if (si->resultBegin == si->resultEnd)
        {
            si->resultBegin = meleeIndex;
            si->resultEnd = meleeIndex + 1;
        }
        else
        {
            if (si->resultBegin > meleeIndex)
                si->resultBegin = meleeIndex;
            if (si->resultEnd <= meleeIndex)
                si->resultEnd = meleeIndex + 1;
        }
    }

    /* Insert the slice index in error messages related to this QE. */
    if (segdbDesc)
        cdbconn_setSliceIndex(segdbDesc, sliceIndex);

    return dispatchResult;
}                               /* cdbdisp_initResult */


/* Destroy a CdbDispatchResult object. */
void
cdbdisp_termResult(CdbDispatchResult  *dispatchResult)
{
    dispatchResult->segdbDesc = NULL;

    /* Free the PGresult objects. */
    cdbdisp_resetResult(dispatchResult);

    /* Free the error message buffer and result buffer. */
    destroyPQExpBuffer(dispatchResult->resultbuf);
    destroyPQExpBuffer(dispatchResult->error_message);
}                               /* cdbdisp_termResult */


/* Reset a CdbDispatchResult object for possible reuse. */
void
cdbdisp_resetResult(CdbDispatchResult  *dispatchResult)
{
    PQExpBuffer buf = dispatchResult->resultbuf;
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
    resetPQExpBuffer(dispatchResult->resultbuf);
    resetPQExpBuffer(dispatchResult->error_message);
}                               /* cdbdisp_resetResult */


/* Take note of an error.
 * 'errcode' is the ERRCODE_xxx value for setting the client's SQLSTATE.
 * NB: This can be called from a dispatcher thread, so it must not use
 * palloc/pfree or elog/ereport because they are not thread safe.
 */
void
cdbdisp_seterrcode(int                  errcode,        /* ERRCODE_xxx or 0 */
                   int                  resultIndex,    /* -1 if no PGresult */
                   CdbDispatchResult   *dispatchResult)
{
    CdbDispatchResults *meleeResults = dispatchResult->meleeResults;

    /* We must ensure a nonzero errcode. */
    if (!errcode)
        errcode = ERRCODE_INTERNAL_ERROR;

    /* Was the command canceled? */
    if (errcode == ERRCODE_GP_OPERATION_CANCELED ||
        errcode == ERRCODE_QUERY_CANCELED)
        dispatchResult->wasCanceled = true;

    /* If this is the first error from this QE, save the error code
     * and the index of the PGresult buffer entry.  We assume the
     * caller has not yet added the item to the PGresult buffer.
     */
    if (!dispatchResult->errcode)
    {
        dispatchResult->errcode = errcode;
        if (resultIndex >= 0)
            dispatchResult->errindex = resultIndex;
    }

    if (!meleeResults)
        return;

    /*
     * Remember which QE reported an error first among the gangs,
     * but keep quiet about cancellation done at our request.
     *
     * Interconnection errors are given lower precedence because often
     * they are secondary to an earlier and more interesting error.
     */
    if (errcode == ERRCODE_GP_OPERATION_CANCELED &&
        dispatchResult->requestedCancel)
    {}                          /* nop */

    else if (meleeResults->errcode == 0
            || (  meleeResults->errcode == ERRCODE_GP_INTERCONNECTION_ERROR
               && errcode != ERRCODE_GP_INTERCONNECTION_ERROR ) )
    {
        pthread_mutex_lock(&setErrcodeMutex);
        if (meleeResults->errcode == 0
           || (  meleeResults->errcode == ERRCODE_GP_INTERCONNECTION_ERROR
              && errcode != ERRCODE_GP_INTERCONNECTION_ERROR ) )
        {
            meleeResults->errcode = errcode;
            meleeResults->iFirstError = dispatchResult->meleeIndex;
        }
        pthread_mutex_unlock(&setErrcodeMutex);
    }
}                               /* cdbdisp_seterrcode */


/* Transfer connection error messages to dispatchResult from segdbDesc. */
bool                            /* returns true if segdbDesc had err info */
cdbdisp_mergeConnectionErrors(CdbDispatchResult                *dispatchResult,
                              struct SegmentDatabaseDescriptor *segdbDesc)
{
    if (!segdbDesc)
        return false;
    if (segdbDesc->errcode == 0 &&
        segdbDesc->error_message.len == 0)
        return false;

    /* Error code should always be accompanied by text and vice-versa. */
    Assert(segdbDesc->errcode != 0 && segdbDesc->error_message.len > 0);

    /* Append error message text and save error code. */
    cdbdisp_appendMessage(dispatchResult, 0, segdbDesc->errcode, "%s",
                          segdbDesc->error_message.data);

    /* Reset connection object's error info. */
    segdbDesc->errcode = 0;
    segdbDesc->error_message.len = 0;
    segdbDesc->error_message.data[0] = '\0';

    return true;
}                               /* cdbdisp_mergeConnectionErrors */


/* Format a message, printf-style, and append to the error_message buffer.
 * Also write it to stderr if logging is enabled for messages of the
 * given severity level 'elevel' (for example, DEBUG1; or 0 to suppress).
 * 'errcode' is the ERRCODE_xxx value for setting the client's SQLSTATE.
 * NB: This can be called from a dispatcher thread, so it must not use
 * palloc/pfree or elog/ereport because they are not thread safe.
 */
void
cdbdisp_appendMessage(CdbDispatchResult    *dispatchResult,
                      int                   elevel,
                      int                   errcode,
                      const char           *fmt,
                      ...)
{
    va_list args;
    int     msgoff;

    /* Remember first error. */
    cdbdisp_seterrcode(errcode, -1, dispatchResult);

    /* Allocate buffer if first message.
     * Insert newline between previous message and new one.
     */
    if (!dispatchResult->error_message)
        dispatchResult->error_message = createPQExpBuffer();
    else
        oneTrailingNewlinePQ(dispatchResult->error_message);

    msgoff = dispatchResult->error_message->len;

    /* Format the message and append it to the buffer. */
    va_start(args, fmt);
    appendPQExpBufferVA(dispatchResult->error_message, fmt, args);
    va_end(args);

    /* Display the message on stderr for debugging, if requested.
     * This helps to clarify the actual timing of threaded events.
     */
    if (elevel >= log_min_messages)
    {
        oneTrailingNewlinePQ(dispatchResult->error_message);
        write_stderr("%s", dispatchResult->error_message->data + msgoff);
    }

    /* In case the caller wants to hand the buffer to ereport(),
     * follow the ereport() convention of not ending with a newline.
     */
    noTrailingNewlinePQ(dispatchResult->error_message);

}                               /* cdbdisp_appendMessage */


/* Store a PGresult object ptr in the result buffer.
 * NB: Caller must not PQclear() the PGresult object.
 */
void
cdbdisp_appendResult(CdbDispatchResult *dispatchResult,
                     struct pg_result  *res)
{
    Assert(dispatchResult && res);

    /* Attach the QE identification string to the PGresult */
    if (dispatchResult->segdbDesc &&
        dispatchResult->segdbDesc->whoami)
        pqSaveMessageField(res, PG_DIAG_GP_PROCESS_TAG,
                           dispatchResult->segdbDesc->whoami);

    appendBinaryPQExpBuffer(dispatchResult->resultbuf, (char *)&res, sizeof(res));
}                               /* cdbdisp_appendResult */


/* Return the i'th PGresult object ptr (if i >= 0), or
 * the n+i'th one (if i < 0), or NULL (if i out of bounds).
 * NB: Caller must not PQclear() the PGresult object.
 */
struct pg_result *
cdbdisp_getPGresult(CdbDispatchResult *dispatchResult, int i)
{
    if (dispatchResult)
    {
        PQExpBuffer buf = dispatchResult->resultbuf;
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
    return dispatchResult ? dispatchResult->resultbuf->len / sizeof(PGresult *)
                          : 0;
}                               /* cdbdisp_numPGresult */


/* Remove all of the PGresult ptrs from a CdbDispatchResult object
 * and place them into an array provided by the caller.  The caller
 * becomes responsible for PQclear()ing them.  Returns the number of
 * PGresult ptrs placed in the array.
 */
int
cdbdisp_snatchPGresults(CdbDispatchResult  *dispatchResult,
                        struct pg_result  **pgresultptrs,
                        int                 maxresults)
{
    PQExpBuffer     buf = dispatchResult->resultbuf;
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
    char        esqlstate[8];
    int         ires;
    int         nres;

    /* Skip if user has messages turned off. */
    if (elevel_error < log_min_messages &&
        elevel_success < log_min_messages)
        return;

    /* PGresult messages */
    nres = cdbdisp_numPGresult(dispatchResult);
    for (ires = 0; ires < nres; ++ires)
    {                           /* for each PGresult */
        PGresult       *pgresult = cdbdisp_getPGresult(dispatchResult, ires);
        ExecStatusType  resultStatus = PQresultStatus(pgresult);
        char   *whoami = PQresultErrorField(pgresult, PG_DIAG_GP_PROCESS_TAG);

        if (!whoami)
            whoami = "no process id";

        /* QE success */
        if (resultStatus == PGRES_COMMAND_OK ||
            resultStatus == PGRES_TUPLES_OK)
        {
            char *cmdStatus = PQcmdStatus(pgresult);

            elog(elevel_success, "DispatchResult: ok %s (%s)",
                 cmdStatus ? cmdStatus : "(no cmdStatus)",
                 whoami);
        }

        /* QE error or libpq error */
        else
        {
            char   *sqlstate = PQresultErrorField(pgresult, PG_DIAG_SQLSTATE);
            char   *pri = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY);
            char   *dtl = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL);
            int     lenpri = strlen(pri);

            if (!sqlstate)
                sqlstate = "no SQLSTATE";

            while (lenpri > 0 &&
                   pri[lenpri-1] <= ' ' &&
                   pri[lenpri-1] > '\0')
                lenpri--;

            ereport(elevel_error, (
                 errmsg("DispatchResult: (%s) %s %.*s (%s)",
                        sqlstate,
                        PQresStatus(PQresultStatus(pgresult)),
                        lenpri,
                        pri,
                        whoami),
                 errdetail("(%s:%s) %s",
                           PQresultErrorField(pgresult, PG_DIAG_SOURCE_FILE),
                           PQresultErrorField(pgresult, PG_DIAG_SOURCE_LINE),
                           dtl ? dtl : "") ));
        }
    }                           /* for each PGresult */

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message &&
        dispatchResult->error_message->len > 0)
    {
        cdbdisp_errcode_to_sqlstate(dispatchResult->errcode, esqlstate);
        elog(elevel_error, "DispatchResult: (%s) %s",
             esqlstate,
             dispatchResult->error_message->data);
    }

    /* Connection error? */
    if (dispatchResult->segdbDesc &&
        dispatchResult->segdbDesc->error_message.len > 0)
	{
        cdbdisp_errcode_to_sqlstate(dispatchResult->segdbDesc->errcode, esqlstate);
        elog(elevel_error, "DispatchResult: (%s) %s",
             esqlstate,
             dispatchResult->segdbDesc->error_message.data);
	}

    /* Should have either an error code or an ok result. */
    if (!dispatchResult->errcode &&
        dispatchResult->okindex < 0)
    {
        elog(elevel_error, "DispatchResult: No ending status from %s",
             dispatchResult->segdbDesc ? dispatchResult->segdbDesc->whoami
                                       : "?");
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
    int     ires;
    int     nres;

    if (!dispatchResult ||
        !buf)
        return;

    /* Format PGresult messages */
    nres = cdbdisp_numPGresult(dispatchResult);
    for (ires = 0; ires < nres; ++ires)
    {                           /* for each PGresult */
        PGresult       *pgresult = cdbdisp_getPGresult(dispatchResult, ires);
        ExecStatusType  resultStatus = PQresultStatus(pgresult);
        char   *whoami = PQresultErrorField(pgresult, PG_DIAG_GP_PROCESS_TAG);

        /* QE success */
        if (resultStatus == PGRES_COMMAND_OK ||
            resultStatus == PGRES_TUPLES_OK)
        {
            if (verbose)
            {
                char *cmdStatus = PQcmdStatus(pgresult);

                oneTrailingNewline(buf);
                appendStringInfo(buf, "ok: %s", cmdStatus ? cmdStatus : "");
                if (whoami)
                    appendStringInfo(buf," (%s)", whoami);
            }
        }

        /* QE error or libpq error */
        else
        {
            char   *pri = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY);
            char   *dtl = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL);
            char   *ctx = PQresultErrorField(pgresult, PG_DIAG_CONTEXT);

            oneTrailingNewline(buf);
            appendStringInfoString(buf, pri);

            if (whoami)
            {
                noTrailingNewline(buf);
                appendStringInfo(buf,"  (%s)", whoami);
            }

            if (dtl)
            {
                oneTrailingNewline(buf);
                appendStringInfo(buf, "\t %s", dtl);
            }
			
            if (ctx)
            {
                oneTrailingNewline(buf);
                appendStringInfo(buf, "\t %s", ctx);
            }

            if (!verbose)
                goto done;
        }
    }                           /* for each PGresult */

    /* segdbDesc error message shouldn't be possible here, but check anyway.
     *   Ordinarily dispatchResult->segdbDesc is NULL here because we are
     *   called after it has been cleared by CdbCheckDispatchResult().
     */
    if (dispatchResult->segdbDesc)
        cdbdisp_mergeConnectionErrors(dispatchResult, segdbDesc);

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message &&
        dispatchResult->error_message->len > 0)
    {
        oneTrailingNewline(buf);
        appendStringInfoString(buf, dispatchResult->error_message->data);
        if (!verbose)
            goto done;
    }

done:
    noTrailingNewline(buf);
}                               /* cdbdisp_dumpDispatchResult */


/*--------------------------------------------------------------------*/


/*
 * cdbdisp_makeDispatchResults:
 * Allocates a CdbDispatchResults object in the current memory context.
 * The caller is responsible for calling DestroyCdbDispatchResults on the returned
 * pointer when done using it.
 */
CdbDispatchResults *
cdbdisp_makeDispatchResults(int     resultCapacity,
                            int     sliceCapacity,
                            bool    cancelOnError)
{
    CdbDispatchResults *results = palloc0(sizeof(*results));
    int     nbytes = resultCapacity * sizeof(results->resultArray[0]);

    results->resultArray = palloc0(nbytes);
    results->resultCapacity = resultCapacity;
    results->resultCount = 0;
    results->iFirstError = -1;
    results->errcode = 0;
    results->cancelOnError = cancelOnError;

    results->sliceMap = NULL;
    results->sliceCapacity = sliceCapacity;
    if (sliceCapacity > 0)
    {
        nbytes = sliceCapacity * sizeof(results->sliceMap[0]);
        results->sliceMap = palloc0(nbytes);
    }

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

    if (results->sliceMap)
    {
        pfree(results->sliceMap);
        results->sliceMap = NULL;
    }

    pfree(results);
}                               /* cdbdisp_destroyDispatchResults */


/* Format a CdbDispatchResults object.
 * Appends error messages to caller's StringInfo buffer.
 * Returns ERRCODE_xxx if some error was found, or 0 if no errors.
 * Before calling this function, you must call CdbCheckDispatchResult().
 */
int
cdbdisp_dumpDispatchResults(struct CdbDispatchResults  *meleeResults,
                            struct StringInfoData      *buffer,
                            bool                        verbose)
{
    CdbDispatchResult  *dispatchResult;

    /* Quick exit if no error (not counting ERRCODE_GP_OPERATION_CANCELED). */
    if (!meleeResults ||
        !meleeResults->errcode)
        return 0;

    /* Find the CdbDispatchResult of the first QE that got an error. */
    Assert(meleeResults->iFirstError >= 0 &&
           meleeResults->iFirstError < meleeResults->resultCount);

    dispatchResult = &meleeResults->resultArray[meleeResults->iFirstError];

    Assert(dispatchResult->meleeResults == meleeResults &&
           dispatchResult->errcode != 0);

    /* Format one QE's result. */
    cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);

    /* Optionally, format results from the rest of the QEs that got errors. */
    if (verbose)
    {
        int i;
        for (i = 0; i < meleeResults->resultCount; ++i)
        {
            dispatchResult = &meleeResults->resultArray[i];
            if (i == meleeResults->iFirstError)
                continue;
            if (dispatchResult->errcode)
                cdbdisp_dumpDispatchResult(dispatchResult, verbose, buffer);
        }
    }

    return meleeResults->errcode;
}                               /* cdbdisp_dumpDispatchResults */


/* Return sum of the cmdTuples values from CdbDispatchResult
 * entries that have a successful PGresult.  If sliceIndex >= 0,
 * uses only the results belonging to the specified slice.
 */
int
cdbdisp_sumCmdTuples(CdbDispatchResults *results, int sliceIndex)
{
    CdbDispatchResult  *dispatchResult;
    CdbDispatchResult  *resultEnd = cdbdisp_resultEnd(results, sliceIndex);
    PGresult           *pgresult;
    int                 sum = 0;

    for (dispatchResult = cdbdisp_resultBegin(results, sliceIndex);
         dispatchResult < resultEnd;
         ++dispatchResult)
    {
        pgresult = cdbdisp_getPGresult(dispatchResult, dispatchResult->okindex);
        if (pgresult &&
            !dispatchResult->errcode)
        {
            char   *cmdTuples = PQcmdTuples(pgresult);
            if (cmdTuples)
                sum += atoi(cmdTuples);
        }
    }

    return sum;
}                               /* cdbdisp_sumCmdTuples */

/*
 * Find the max of the lastOid values returned from the QEs
 */
Oid
cdbdisp_maxLastOid(CdbDispatchResults *results, int sliceIndex)
{
    CdbDispatchResult  *dispatchResult;
    CdbDispatchResult  *resultEnd = cdbdisp_resultEnd(results, sliceIndex);
    PGresult           *pgresult;
    Oid                 oid = InvalidOid;

    for (dispatchResult = cdbdisp_resultBegin(results, sliceIndex);
         dispatchResult < resultEnd;
         ++dispatchResult)
    {
        pgresult = cdbdisp_getPGresult(dispatchResult, dispatchResult->okindex);
        if (pgresult &&
            !dispatchResult->errcode)
        {
            Oid tmpoid = PQoidValue(pgresult);
            if (tmpoid >oid)
                oid = tmpoid;
        }
    }

    return oid;
}                               /* cdbdisp_maxLastOid */

/* Return ptr to first resultArray entry for a given sliceIndex. */
CdbDispatchResult *
cdbdisp_resultBegin(CdbDispatchResults *results, int sliceIndex)
{
    CdbDispatchResults_SliceInfo   *si;

    if (!results)
        return NULL;

    if (sliceIndex < 0)
        return &results->resultArray[0];

    si = &results->sliceMap[sliceIndex];

    Assert(sliceIndex < results->sliceCapacity &&
           si->resultBegin >= 0 &&
           si->resultBegin <= si->resultEnd &&
           si->resultEnd <= results->resultCount);

    return &results->resultArray[si->resultBegin];
}                               /* cdbdisp_resultBegin */


/* Return ptr to last+1 resultArray entry for a given sliceIndex. */
CdbDispatchResult *
cdbdisp_resultEnd(CdbDispatchResults *results, int sliceIndex)
{
    CdbDispatchResults_SliceInfo   *si;

    if (!results)
        return NULL;

    if (sliceIndex < 0)
        return &results->resultArray[results->resultCount];

    si = &results->sliceMap[sliceIndex];

    return &results->resultArray[si->resultEnd];
}                               /* cdbdisp_resultEnd */


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


#include "libpq/pqformat.h"
#include "access/printtup.h"
#include "cdb/cdbtm.h"
#include "utils/tqual.h"
/*
 * If we are a dispatch agent, we need to relay any results from the QEs back
 * up to the QD.
 */
char *
cdbdisp_relayresults(CdbDispatchResults *pPrimaryResults,
					 CdbDispatchResults *pMirrorResults)
{
	
	int i;
	int nfields = 0;
	TupleDesc typeinfo;
	CdbDispatchResults *gangResults;
	CdbDispatchResult *dispatchResult;
	struct pg_result * p;
	char * completionTag = NULL;
	StringInfoData buf;
	long sum = 0;
	Oid lastOid = InvalidOid;
	
	StringInfoData errmsgbuf;
	initStringInfo(&errmsgbuf);

	/*
	 * Let's hope that everybody has the same completion tag, or we are in trouble.
	 * 
	 * Grab the first result from the first gang member of the primary gang,
	 * get it's tag, and use that to send back to the QD.
	 * 
	 */
	
	elog(DEBUG1,"relayResults is starting");

	dispatchResult = &pPrimaryResults->resultArray[0];
	Assert(dispatchResult);
	p = ((struct pg_result **)dispatchResult->resultbuf->data)[0];
	Assert(p);
	completionTag = palloc0(COMPLETION_TAG_BUFSIZE);
	strcpy(completionTag,p->cmdStatus);
	 
	/*
	 * Not only the results need to get relayed.  Also the DTX info
	 */

	if (p->QEWriter_HaveInfo)
	{
		elog(DEBUG1,"First result: distributedXid %d, curcid %d, is dirty %d",p->QEWriter_DistributedTransactionId,
				p->QEWriter_CommandId,p->QEWriter_Dirty);
		
		/*
		 * This is a hack.  Copy the QEs DTX info into our DTX context, so that when
		 * we send our completion tag (via EndCommand), the information will go back to the QD.
		 * 
		 * Hopefully, the DTX code won't take notice of us doing this, as we don't want the
		 * TM doing anything with the dispatch agents.
		 */
		
		QEDtxContextInfo.distributedXid = p->QEWriter_DistributedTransactionId;
		QEDtxContextInfo.distributedSnapshotData.curcid = p->QEWriter_CommandId;
		if (p->QEWriter_Dirty)
		{
			MyXactMadeXLogEntry = true;   // marks our transaction as dirty
		}
	}
	
	/* Collect results from primary gang. */
	
	/*
	 * In normal queries, the results are flowing back to the end-user, so we don't get them here, but
	 * for internal queries, we do get results, and those results need to go back to the QD.
	 */
	gangResults = pPrimaryResults;
	if (gangResults)
	{
		/*
		 * if there are tuples to be sent back, they need to be preceeded with 
		 * a row description message.
		 */
		for (i = 0; i < gangResults->resultCount; ++i)
		{
			dispatchResult = &gangResults->resultArray[i];
			p = ((struct pg_result **)dispatchResult->resultbuf->data)[0];

			// Find first one with tuples (if one has tuples, they all have either tuples or errors)
			if (PQresultStatus(p) == PGRES_TUPLES_OK)
			{
				int j;
				nfields = PQnfields(p);
				typeinfo = CreateTemplateTupleDesc(nfields,false);

				// Get description of the row.  We make the assumtion that all the QEs must send
				// data in the same format
				for (j = 0; j<nfields; j++)
				{
					TupleDescInitEntry(typeinfo, (AttrNumber) (j+1), PQfname(p,j), PQftype(p, j), PQfmod(p,j), 0);
				}
				// Send row descriptor results back to our QD
				SendRowDescriptionMessage(typeinfo,
						NULL, /*((Query *) linitial(parseTrees))->targetList,*/
						NULL);
				break;
			}
		}

		for (i = 0; i < gangResults->resultCount; ++i)
		{
			int nr;
			int j;
			dispatchResult = &gangResults->resultArray[i];

			/* Append error messages. */
			cdbdisp_dumpDispatchResult(dispatchResult, false, &errmsgbuf);
			
			/*
			 * Now this is a problem.  We might have multiple result sets from this QE
			 * (presumably, if one QE has multiple result sets, they all do).
			 * 
			 * But we aren't really equiped yet to do the right thing.
			 * 
			 * Lets just hope we get one result set back for now.
			 */
			nr = dispatchResult->resultbuf->len/sizeof(struct pg_result *);
			
			if (nr > 1)
				elog(WARNING,"multiple result sets can't be relayed properly yet");
			
			for (j = 0; j<nr; j++)
			{
				
				
				p = ((struct pg_result **)dispatchResult->resultbuf->data)[j];
				
				/*
				 * if the command completed OK, we want to collect the number of tuples and lastOid
				 * information to relay back to the QD.
				 */
				if (PQresultStatus(p) == PGRES_TUPLES_OK || PQresultStatus(p) == PGRES_COMMAND_OK)
				{
					char   *cmdTuples;
					Oid    cmdLastOid;
					cmdTuples = PQcmdTuples(p);
		            if (cmdTuples)
		                sum += atol(cmdTuples);
		            
		            cmdLastOid = PQoidValue(p);
		            if (cmdLastOid)
		            	if (lastOid < cmdLastOid)
		            		lastOid = cmdLastOid;
				}
				
				/*
				 * If there are any tuples, we need to send them back to the QD
				 */
				if (PQresultStatus(p) == PGRES_TUPLES_OK)
				{
					int j,k;
					int nRows = PQntuples(p);
					for (k=0; k<nRows; k++)
					{
						initStringInfo(&buf);
						/*
						 * Prepare a DataRow message
						 */
						pq_beginmessage(&buf, 'D');

						pq_sendint(&buf, nfields, 2);

						for (j=0; j<nfields; j++)
						{
							char * v = PQgetvalue(p, k, j);
							elog(DEBUG2," result %s",v);
							pq_sendcountedtext(&buf, v, strlen(v), false);
						}
						
						pq_endmessage(&buf);
					}

				}
 
				/*
				 * the DTX info that was send with the EndCommand from the QE needs to be relayed
				 * back to the QD.
				 * 
				 * Apparently, we only get this info IF the QE has done some work.
				 */
				if (p->QEWriter_HaveInfo)
				{
					if (QEDtxContextInfo.distributedXid != p->QEWriter_DistributedTransactionId ||
							QEDtxContextInfo.distributedSnapshotData.curcid != p->QEWriter_CommandId)
					{
						elog(DEBUG1,"other result: distributedXid %d, curcid %d, is dirty %d",p->QEWriter_DistributedTransactionId,
								p->QEWriter_CommandId,p->QEWriter_Dirty);
						QEDtxContextInfo.distributedXid = p->QEWriter_DistributedTransactionId;
						QEDtxContextInfo.distributedSnapshotData.curcid = p->QEWriter_CommandId;
						
					}
					if (p->QEWriter_Dirty)
					{
						MyXactMadeXLogEntry = true;   // marks our transaction as dirty
					}
				}
				
			}

		}
		cdbdisp_destroyDispatchResults(gangResults);
	}

	/* Collect results from mirror gang. */
	gangResults = pMirrorResults;
	if (gangResults)
	{
		for (i = 0; i < gangResults->resultCount; ++i)
		{
			int nr;
			int j;
			dispatchResult = &gangResults->resultArray[i];

			/* Append error messages. */
			cdbdisp_dumpDispatchResult(dispatchResult, false, &errmsgbuf);
			
			nr = dispatchResult->resultbuf->len/sizeof(struct pg_result *);
			
			for (j = 0; j<nr; j++)
			{
				p = ((struct pg_result **)dispatchResult->resultbuf->data)[j];
				/*
				 * if the command was sent to the mirrors, we need to relay any
				 * result tuples back from the mirror gangs as well
				 */
				if (PQresultStatus(p) == PGRES_TUPLES_OK)
				{
					int j,k;
					int nRows = PQntuples(p);
					for (k=0; k<nRows; k++)
					{
						initStringInfo(&buf);
						/*
						 * Prepare a DataRow message
						 */
						pq_beginmessage(&buf, 'D');

						pq_sendint(&buf, nfields, 2);

						for (j=0; j<nfields; j++)
						{
							char * v = PQgetvalue(p, k, j);
							elog(DEBUG2," result %s",v);
							pq_sendcountedtext(&buf, v, strlen(v), false);
						}
						
						pq_endmessage(&buf);
					}

				}
				if (p->QEWriter_HaveInfo)
				{
					if (QEDtxContextInfo.distributedXid != p->QEWriter_DistributedTransactionId ||
							QEDtxContextInfo.distributedSnapshotData.curcid != p->QEWriter_CommandId)
					{
						elog(DEBUG2,"mirror result: distributedXid %d, curcid %d, is dirty %d",p->QEWriter_DistributedTransactionId,
								p->QEWriter_CommandId,p->QEWriter_Dirty);
						QEDtxContextInfo.distributedXid = p->QEWriter_DistributedTransactionId;
						QEDtxContextInfo.distributedSnapshotData.curcid = p->QEWriter_CommandId;
						
					}
					if (p->QEWriter_Dirty)
					{
						MyXactMadeXLogEntry = true;   // marks our transaction as dirty
					}
				}
				
			}

		}
		cdbdisp_destroyDispatchResults(gangResults);
	}
	
	/*
	 * If any of the QEs say they modified tuples, we need to relay that
	 * back to the QD as part of the completionTag.
	 */
	if (sum > 0)
	{
		if (strncmp(completionTag,"INSERT ",7)==0)
		{
			completionTag[0] = '\0';
			snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "INSERT %u %ld",lastOid,sum);
		}
		else if (strncmp(completionTag,"UPDATE ",7)==0)
		{
			completionTag[0] = '\0';
			snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "UPDATE %ld",sum);
		}
		else if (strncmp(completionTag,"DELETE ",7)==0)
		{
			completionTag[0] = '\0';
			snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "DELETE %ld",sum);
		}
	}
	
	elog(DEBUG1,"relayresults is done. completionTag %s  errmsgbuf.len=%d",completionTag, errmsgbuf.len);
	
	if (errmsgbuf.len > 0)
		ereport(ERROR, (errmsg("replay results dispatch error"),
						errdetail("%s", errmsgbuf.data)));

	
	return completionTag;
}



/*--------------------------------------------------------------------*/


