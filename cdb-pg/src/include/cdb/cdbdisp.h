/*-------------------------------------------------------------------------
 *
 * cdbdisp.h
 * routines for dispatching commands from the dispatcher process
 * to the qExec processes.
 *
 * Copyright (c) 2005-2006, greenplum inc
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
 * The caller must also provide a serialized SnapshotInfo string to be used to
 * set the global snapshot for the dispatched statement.
 *
 * The caller must provide a CdbDispatchResults object having available
 * resultArray slots sufficient for the number of QEs to be dispatched:
 * i.e., resultCapacity - resultCount >= gp->size.  This function will
 * assign one resultArray slot per QE of the Gang, paralleling the Gang's
 * db_descriptors array.  Success or failure of each QE will be noted in
 * the QE's CdbDispatchResult entry; but before examining the results, the
 * caller must wait for execution to end by calling CdbCheckDispatchResult().
 *
 * The CdbDispatchResults object owns some malloc'ed storage, so the caller
 * must make certain to free it by calling cdbdisp_destroyDispatchResults().
 *
 * When dispatchResults->cancelOnError is false, szCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
cdbdisp_dispatchToGang(const char                  *szCommand,
					   const char				   *szSnapshotInfo,
                       struct Gang                 *gp,
                       struct CdbDispatchResults   *dispatchResults,
                       int                          sliceIndex);

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
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
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
 * cdbdisp_handleError
 *
 * When caller catches an error, the PG_CATCH handler can use this
 * function instead of cdbdisp_finishCommand to wait for all QEs
 * to finish, clean up, and report QE errors if appropriate.
 * This function should be called only from PG_CATCH handlers.
 *
 * This function destroys and frees the given CdbDispatchResults objects.
 * It is a no-op if both CdbDispatchResults ptrs are NULL.
 *
 * On return, the caller is expected to finish its own cleanup and
 * exit via PG_RE_THROW().
 */
void
cdbdisp_handleError(struct CdbDispatchResults  *primaryResults,
				    struct CdbDispatchResults  *mirrorResults);

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
 *
 * The CdbDispatchResults objects allocated for the plan are
 * returned in *pPrimaryResults and (if needed) *pMirrorResults.
 * The caller, after calling CdbCheckDispatchResult(), can
 * examine the CdbDispatchResults objects, can keep them as
 * long as needed, and ultimately must free them with
 * cdbdisp_destroyDispatchResults() prior to deallocation
 * of the caller's memory context.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchPlan(struct QueryDesc              *queryDesc,
                     bool                           cancelOnError,
                     struct CdbDispatchResults    **pPrimaryResults,  /* OUT */
                     struct CdbDispatchResults    **pMirrorResults);  /* OUT */

/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  Does not wait for
 * completion.  Does not start a global transaction.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatchResults() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
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
