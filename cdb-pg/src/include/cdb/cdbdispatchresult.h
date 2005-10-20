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

struct pg_result;                   /* PGresult ... #include "libpq-fe.h" */
struct SegmentDatabaseDescriptor;   /* #include "cdb/cdbconn.h" */
struct StringInfoData;              /* #include "lib/stringinfo.h" */
struct PQExpBufferData;             /* #include "libpq-int.h" */


/*
 * CdbDispatchResult:
 * Struct for holding the result information
 * for a command dispatched by CdbCommandDispatch to
 * a single segdb.
 */
typedef struct CdbDispatchResult
{
    struct SegmentDatabaseDescriptor *segdbDesc;
                                        /* libpq connection to QE process;
                                         * reset to NULL after end of thread
                                         */

    struct CdbDispatchResults *gangResults; /* owner of this CdbDispatchResult */
    int                 gangIndex;      /* QE's position within the gang; index
                                         * to self in gangResults->resultArray
                                         */

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
    struct PQExpBufferData *resultbuf;      /* -> array of ptr to PGresult */
    struct PQExpBufferData *error_message;  /* -> string of messages; or NULL */

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
cdbdisp_initResult(CdbDispatchResult                   *dispatchResult,
                   int                                  gangIndex,
                   struct CdbDispatchResults           *gangResults,
                   struct SegmentDatabaseDescriptor    *segdbDesc);

/* Destroy a CdbDispatchResult object. */
void
cdbdisp_termResult(CdbDispatchResult  *dispatchResult);

/* Reset a CdbDispatchResult object for possible reuse. */
void
cdbdisp_resetResult(CdbDispatchResult  *dispatchResult);

/* Take note of an error.
 * 'errcode' is the ERRCODE_xxx value for setting the client's SQLSTATE.
 * NB: This can be called from a dispatcher thread, so it must not use
 * palloc/pfree or elog/ereport because they are not thread safe.
 */
void
cdbdisp_seterrcode(int                  errcode,        /* ERRCODE_xxx or 0 */
                   int                  resultIndex,    /* -1 if no PGresult */
                   CdbDispatchResult   *dispatchResult);

/* Transfer connection error messages to dispatchResult from segdbDesc. */
bool                            /* returns true if segdbDesc had err info */
cdbdisp_mergeConnectionErrors(CdbDispatchResult                *dispatchResult,
                              struct SegmentDatabaseDescriptor *segdbDesc);

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
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 4, 5)));

/* Store a PGresult object ptr in the result buffer.
 * NB: Caller must not PQclear() the PGresult object.
 */
void
cdbdisp_appendResult(CdbDispatchResult *dispatchResult,
                     struct pg_result  *res);

/* Return the i'th PGresult object ptr (if i >= 0), or
 * the n+i'th one (if i < 0), or NULL (if i out of bounds).
 * NB: Caller must not PQclear() the PGresult object.
 */
struct pg_result *
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
                        struct pg_result  **pgresultptrs,
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

/* Format a CdbDispatchResults object.
 * Appends error messages to caller's StringInfo buffer.
 * Returns ERRCODE_xxx if some error was found, or 0 if no errors.
 * Before calling this function, you must call CdbCheckDispatchResult().
 */
int
cdbdisp_dumpDispatchResults(struct CdbDispatchResults  *gangResults,
                            StringInfo                  buffer,
                            bool                        verbose);

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
