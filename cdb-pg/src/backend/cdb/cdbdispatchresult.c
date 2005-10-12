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
    dispatchResult->requestedCancel = false;

    /* Empty (but don't free) the error message buffer and result buffer. */
    resetPQExpBuffer(&dispatchResult->resultbuf);
    resetPQExpBuffer(&dispatchResult->error_message);
}                               /* cdbdisp_resetResult */


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


/* Display a CdbDispatchResult in the log for debugging.
 * Call only from main thread, during or after cdbdisp_checkDispatchResults.
 */
void
cdbdisp_debugDispatchResult(CdbDispatchResult  *dispatchResult,
                            int                 elevel_error,
                            int                 elevel_success)
{
    SegmentDatabaseDescriptor  *segdbDesc;
    PGresult   *pgresult;
    char        sqlstate[8];

    /* Skip if user has messages turned off. */
    if (elevel_error < log_min_messages &&
        elevel_success < log_min_messages)
        return;

    segdbDesc = dispatchResult->segdbDesc;

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message.len > 0)
    {
        cdbdisp_errcode_to_sqlstate(dispatchResult->errcode, sqlstate);
        elog(elevel_error, "DispatchResult: %s -> (%s) %s",
             segdbDesc->whoami,
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
                        segdbDesc->whoami,
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
        elog(elevel_success, "DispatchResult: %s -> ok %s",
             segdbDesc->whoami,
             pgresult ? PQcmdStatus(pgresult) : "");
    }

    /* Should have either an error code or an ok result. */
    if (!dispatchResult->errcode &&
        dispatchResult->okindex < 0)
    {
        elog(elevel_error, "DispatchResult: %s -> No ending status",
             segdbDesc->whoami);
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
    int                         ires, nres;

    Assert(dispatchResult && buf);

    /* Error found on our side of the libpq interface? */
    if (dispatchResult->error_message.len > 0)
    {
        appendStringInfo(buf, "(%s) %s",
                         segdbDesc->whoami,
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

        /* QE error or libpq error */
        if (!success)
        {
            char   *pri = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY);
            char   *dtl = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL);

            appendStringInfo(buf, "(%s) %s", segdbDesc->whoami, pri);
            if (dtl)
                appendStringInfo(buf, "\t %s", dtl);

            if (!verbose)
                return;
        }

        /* QE success */
        else if (verbose)
        {
            appendStringInfo(buf, "(%s) %s\n",
                             segdbDesc->whoami,
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
cdbdisp_makeDispatchResults(int resultCount, const char* szCommand)
{
    CdbDispatchResults *results = palloc0(sizeof(*results));

    results->szCommand = szCommand ? pstrdup(szCommand) : NULL;
    results->resultArray = palloc0(resultCount * sizeof(results->resultArray[0]));
    results->resultCount = resultCount;

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

    if (results->szCommand)
	{
        pfree(results->szCommand);
		results->szCommand = NULL;
	}

    pfree(results);
}                               /* cdbdisp_destroyDispatchResults */


/* Return ptr to first resultArray entry having nonzero errcode; or NULL. */
CdbDispatchResult *
cdbdisp_firstErrorResult(CdbDispatchResults *results)
{
    if (results &&
        results->resultCount >= 0)
    {
        int i;
        for (i = 0; i < results->resultCount; ++i)
        {
            CdbDispatchResult  *dispatchResult = &results->resultArray[i];
            if (dispatchResult->errcode)
                return dispatchResult;
        }
    }
    return NULL;
}                               /* cdbdisp_firstErrorResult */


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


