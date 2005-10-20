/*-------------------------------------------------------------------------
 *
 * cdbdisp.h
 * routines for dispatching commands from the dispatcher process
 * to the qExec processes.
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBDISP_H
#define CDBDISP_H

#include "lib/stringinfo.h"         /* StringInfo */

struct CdbDispatchResult;           /* #include "cdb/cdbdispatchresult.h" */
struct CdbDispatchResults;          /* #include "cdb/cdbdispatchresult.h" */
struct pg_result;                   /* #include "libpq-fe.h" */
struct Gang;                        /* #include "cdb/cdbgang.h" */
struct Node;                        /* #include "nodes/nodes.h" */
struct QueryDesc;                   /* #include "executor/execdesc.h" */
struct SegmentDatabaseDescriptor;   /* #include "cdb/cdbconn.h" */

/*--------------------------------------------------------------------*/

/* Start a global transaction and invoke FTS reconfiguration if needed. */
void
cdbdisp_startDtxTransaction(void);

/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchToGang:
 * Send the szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * Unless an error prevents delegating the command to the threads,
 * a CdbDispatchResults object is returned.  It contains an array of
 * CdbDispatchResult objects, one per QE, paralleling the Gang's
 * db_descriptors array.  Success or failure of each QE will be
 * noted in the corresponding CdbDispatchResult entry, but before
 * examining those, the caller must wait for execution to end by
 * calling CdbCheckDispatchResult().
 *
 * The CdbDispatchResults object is palloc'ed in the caller's context.
 * It owns some malloc'ed storage, so the caller must make certain to
 * free it by calling cdbdisp_destroyDispatchResults().
 *
 * When cancelOnError is false, szCommand is to be dispatched to
 * every connected gang member without regard for cancellation
 * requests nor for any other QE's errors, connection failures, etc.
 *
 * NB: This function should return, even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
struct CdbDispatchResults *
cdbdisp_dispatchToGang(const char  *szCommand,
                       struct Gang *gp,
                       int          sliceIndex,
                       bool         cancelOnError);

/*
 * CdbCheckDispatchResult:
 *
 * Waits for completion of threads launched by cdbdisp_dispatchToGang().
 *
 * If 'cancelUnfinishedWork' is true, QEs that were dispatched with
 * 'cancelOnError' true and are not yet idle will be canceled.
 */
void
CdbCheckDispatchResult(bool cancelUnfinishedWork);

void
CdbCheckDispatchResultInt(struct SegmentDatabaseDescriptor   ***failedSegDB,
                          int          *numOfFailed,
                          bool          cancelUnfinishedWork);

/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchRMCommand:
 * Sends a non-cancelable command to all segment dbs, primary and mirror.
 *
 * Returns a malloc'ed array containing the PGresult objects thus
 * produced; the caller must PQclear() them and free() the array.
 * A NULL entry follows the last used entry in the array.
 *
 * Any error messages - whether or not they are associated with
 * PGresult objects - are appended to a StringInfo buffer provided
 * by the caller.
 */
struct pg_result **             /* returns ptr to array of PGresult ptrs */
cdbdisp_dispatchRMCommand(const char   *szCommand,
                          StringInfo    errmsgbuf);

/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchCommand:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * The CdbDispatchResults objects allocated for the command
 * are returned in *pPrimaryResults and *pMirrorResults.
 * The caller, after calling CdbCheckDispatchResult(), can
 * examine the CdbDispatchResults objects, can keep them as
 * long as needed, and ultimately must free them with
 * cdbdisp_destroyDispatchResults() prior to deallocation
 * of the memory context from which they were allocated.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 */
void
cdbdisp_dispatchCommand(const char                 *szCommand,
                        bool                        cancelOnError,
                        struct CdbDispatchResults **pPrimaryResults,    /* OUT */
                        struct CdbDispatchResults **pMirrorResults);    /* OUT */

/* Wait for all QEs to finish, then report any errors from the given
 * CdbDispatchResults objects and free them.  If not all QEs in the
 * associated gang(s) executed the command successfully, throws an
 * error and does not return.  No-op if both CdbDispatchResults ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct CdbDispatchResults    *primaryResults,
                      struct CdbDispatchResults    *mirrorResults);

/*
 * CdbDoCommand:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * Called by general users, this method includes global transaction control.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommand(const char *szCommand, bool cancelOnError);

/*
 * CdbDoCommandNoTxn:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommandNoTxn(const char   *szCommand,
                  bool          cancelOnError);


/*--------------------------------------------------------------------*/

/* Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.
 */
void
cdbdisp_dispatchPlan(struct QueryDesc *queryDesc);

/* Free the results of all gangs in a query plan's slice table.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
void
cdbdisp_destroyPlanResults(struct QueryDesc *queryDesc);

/* Check and free the results of all gangs in a query plan's slice table.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
void
cdbdisp_dumpPlanResults(struct QueryDesc *queryDesc);

/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  Does not wait for
 * completion.  Does not start a global transaction.
 * To wait for completion and check for errors, it is suggested
 * that the caller use cdbdisp_finishCommand().
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 */
void
cdbdisp_dispatchUtilityStatement(struct Node   *stmt,
                                 bool           cancelOnError,
                                 struct CdbDispatchResults    **pPrimaryResults,
                                 struct CdbDispatchResults    **pMirrorResults);

/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs, and wait for completion.
 * Starts a global transaction first, if not already started.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 */
void
CdbDispatchUtilityStatement(struct Node *stmt);


/*--------------------------------------------------------------------*/

#endif   /* CDBDISP_H */
