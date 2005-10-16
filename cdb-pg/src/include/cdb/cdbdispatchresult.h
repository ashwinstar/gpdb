/*-------------------------------------------------------------------------
 *
 * cdbdispatchresult.h
 * routines for dispatching commands from the dispatcher process
 * to the qExec processes.
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDISPATCHRESULT_H
#define CDBDISPATCHRESULT_H

#include "libpq-fe.h"               /* prerequisite for libpq-int.h */
#include "libpq-int.h"              /* PQExpBufferData */
#include "cdb/cdbconn.h"            /* SegmentDatabaseDescriptor */

struct StringInfoData;              /* #include "lib/stringinfo.h" */


/*
 * CdbDispatchResult:
 * Struct for holding the result information
 * for a command dispatched by CdbCommandDispatch to
 * a single segdb.
 */
typedef struct CdbDispatchResult
{
    SegmentDatabaseDescriptor  *segdbDesc;  /* libpq connection to QE process */
    int                 errcode;        /* ERRCODE_xxx (sqlstate encoded as
                                         * an int) of first error, or 0.
                                         */
    int                 errindex;       /* index of first entry in resultbuf
                                         * that represents an error; or -1.
                                         * Pass to cdbconn_getResult().
                                         */
    int                 okindex;        /* index of last entry in resultbuf
                                         * with resultStatus == PGRES_TUPLES_OK
                                         * or PGRES_COMMAND_OK (command ended
                                         * successfully); or -1.
                                         * Pass to cdbconn_getResult().
                                         */
    PQExpBufferData     resultbuf;      /* array of *PGresult */
    PQExpBufferData     error_message;  /* string of messages */

    bool                hasDispatched;  /* true => PQsendCommand done */
    bool                stillRunning;   /* true => busy in dispatch thread */
    bool                requestedCancel; /*true => called PQrequestCancel */
    bool                wasCanceled;    /* true => got any of these errors:
                                         *  ERRCODE_MPP_OPERATION_CANCELED
                                         *  ERRCODE_QUERY_CANCELED
                                         */
}   CdbDispatchResult;


/* Construct a CdbDispatchResult object. */
void
cdbdisp_initResult(CdbDispatchResult           *dispatchResult,
                   SegmentDatabaseDescriptor   *segdbDesc);

/* Destroy a CdbDispatchResult object. */
void
cdbdisp_termResult(CdbDispatchResult  *dispatchResult);

/* Reset a CdbDispatchResult object for possible reuse. */
void
cdbdisp_resetResult(CdbDispatchResult  *dispatchResult);

/* Transfer connection error messages to dispatchResult from segdbDesc. */
bool                            /* returns true if segdbDesc had err info */
cdbdisp_mergeConnectionErrors(CdbDispatchResult            *dispatchResult,
                              SegmentDatabaseDescriptor    *segdbDesc);

/* Store a PGresult object ptr in the result buffer.
 * NB: Caller must not PQclear() the PGresult object.
 */
void
cdbdisp_appendResult(CdbDispatchResult *dispatchResult,
                     PGresult          *res);

/* Return the i'th PGresult object ptr (if i >= 0), or
 * the n+i'th one (if i < 0), or NULL (if i out of bounds).
 * NB: Caller must not PQclear() the PGresult object.
 */
PGresult *
cdbdisp_getPGresult(CdbDispatchResult *dispatchResult, int i);

/* Return the number of PGresult objects in the result buffer. */
int
cdbdisp_numPGresult(CdbDispatchResult  *dispatchResult);

/* Remove all of the PGresult ptrs from a CdbDispatchResult object
 * and place them into an array provided by the caller.  The caller
 * becomes responsible for PQclear()ing them.  Returns the number of
 * PGresult ptrs placed in the array.
 */
int
cdbdisp_snatchPGresults(CdbDispatchResult  *dispatchResult,
                        PGresult          **pgresultptrs,
                        int                 maxresults);

/* Display a CdbDispatchResult in the log for debugging.
 * Call only from main thread, during or after cdbdisp_checkDispatchResults.
 */
void
cdbdisp_debugDispatchResult(CdbDispatchResult  *dispatchResult,
                            int                 elevel_error,
                            int                 elevel_success);

/* Format a CdbDispatchResult into a StringInfo buffer provided by caller.
 * If verbose = true, reports all results; else reports at most one error.
 */
void
cdbdisp_dumpDispatchResult(CdbDispatchResult       *dispatchResult,
                           bool                     verbose,
                           struct StringInfoData   *buf);

/*--------------------------------------------------------------------*/

/*
 * CdbDispatchResults:
 * Struct for holding the results information for all segdb's
 * of a Gang, for a command dispatched by CdbDispatchToGang.
 */
typedef struct CdbDispatchResults
{
    int                 resultCount;    /* num of QEs */
    CdbDispatchResult  *resultArray;    /* -> array [0..resultCount-1] of
                                         *      struct CdbDispatchResult
                                         */
    volatile int        iFirstError;    /* index of the resultArray entry for
                                         * the QE that first reported an error;
                                         * or -1.  Note that this index equals
                                         * the QE's segdbDesc->gangindex; it
                                         * isn't the segindex.
                                         */
    volatile int        errcode;        /* ERRCODE_xxx (sqlstate encoded as
                                         * an int) of the first error, or 0.
                                         */
    bool                cancelOnError;  /* true => stop remaining QEs on err */
} CdbDispatchResults;


/* Allocate a CdbDispatchResults object in the current memory context. */
CdbDispatchResults *
cdbdisp_makeDispatchResults(int resultCount, bool cancelOnError);

/* Clean up and free a CdbDispatchResults object. */
void
cdbdisp_destroyDispatchResults(CdbDispatchResults  *results);

/* Return sum of the cmdTuples values from CdbDispatchResult
 * entries that have a successful PGresult.
 */
int
cdbdisp_sumCmdTuples(CdbDispatchResults *results);

/*--------------------------------------------------------------------*/

/* Convert compact error code (ERRCODE_xxx) to 5-char SQLSTATE string,
 * and put it into a 6-char buffer provided by caller.
 */
char *                          /* returns outbuf+5 */
cdbdisp_errcode_to_sqlstate(int errcode, char outbuf[6]);

/* Convert SQLSTATE string to compact error code (ERRCODE_xxx). */
int
cdbdisp_sqlstate_to_errcode(const char *sqlstate);

/*--------------------------------------------------------------------*/

#endif   /* CDBDISPATCHRESULT_H */
