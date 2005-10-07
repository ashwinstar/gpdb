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


/*
 * cdbdisp_dispatchToGang:
 * Send the szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * Unless an error prevents delegating the command to the threads, on return
 * there is a CdbDispatchResults object attached to the caller's Gang
 * object.  It contains an array of CdbDispatchResult objects, one per QE,
 * paralleling the Gang's db_descriptors array.  Success or failure of each
 * QE will be noted in the corresponding CdbDispatchResult entry, but before
 * examining it, the caller must wait for execution to end by calling
 * cdbdisp_checkDispatchResults().
 *
 * On return, the caller's Gang object owns the CdbDispatchResults object.
 * After examining the CdbDispatchResults object, the caller should detach
 * it from the Gang and call cdbdisp_destroyDispatchResults() to free it.
 */
void
cdbdisp_dispatchToGang(const char          *szCommand,
                       struct Gang         *gp,
                       int                  sliceIndex,
                       bool                 cancelFromBelow,
                       struct pg_result  ***pgresultsets);

/*
 * CdbCheckDispatchResult:
 * Waits for threads that were launched in CdbDispatchCommand
 * to finish.  Returns true if they all succeeded.
 */
bool                            /* returns true if success on all QEs */
CdbCheckDispatchResult(void);

bool                            /* returns true if success on all QEs */
CdbCheckDispatchResultInt(struct SegmentDatabaseDescriptor   ***failedSegDB,
                          int                                  *numOfFailed);

/* Format and free a Gang's CdbDispatchResults object.
 * Appends error messages to caller's StringInfo buffer.
 * If verbose = true, formats all errors; else formats at most one error.
 * Returns false if some error was found, or true if no errors.
 * Before calling this function, you must call CdbCheckDispatchResult.
 */
bool                            /* returns true if ok */
cdbdisp_dumpGangResults(struct Gang    *gang,
                        StringInfo      buffer,      /* OUT */
                        bool            verbose);

/* Free a Gang's CdbDispatchResults object. */
void
cdbdisp_destroyGangResults(struct Gang *gang);

/* Get sum of cmdTuples returned by the successful QEs of a Gang.
 * Used to determine the row counts for insert, update, and delete.
 */
int
cdbdisp_sumGangCmdTuples(struct Gang *gang);


/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchCommand:
 * Send ths szCommand SQL statement to all segdbs in the cluster
 * cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable mpp_connections_per_thread.
 *
 * The Gang objects allocated for the command can optionally be returned
 * in *pPrimaryGang and *pMirrorGang.  The caller, after calling
 * CdbCheckDispatchResult, can examine the CdbDispatchResults object
 * attached to each Gang.  The Gang objects are owned by the QE Factory.
 * However, the caller may optionally detach the CdbDispatchResults objects,
 * keep them as long as needed (perhaps longer than the gangs exist),
 * and ultimately free them with cdbdisp_destroyDispatchResults().
 */
void
cdbdisp_dispatchCommand(const char         *szCommand,
                        bool                cancelFromBelow,
                        struct pg_result ***resultSets,         /* OUT */
                        struct Gang       **pPrimaryGang,       /* OUT */
                        struct Gang       **pMirrorGang);       /* OUT */

extern void CdbDispatchCommand(const char *szCommand, bool cancelFromBelow);

/* Wait for all QEs to finish, then report any errors from the given gangs.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.  No-op if both gang ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct Gang  *primaryGang,
                      struct Gang  *mirrorGang);

/*
 * CdbDoCommand:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * Called by general users, this method includes global transaction control.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommand(const char *szCommand, bool cancelFromBelow);

/*
 * CdbDoCommandInt:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 */
void
CdbDoCommandInt(const char         *szCommand,
                bool                cancelFromBelow,
                struct pg_result ***resultSets);


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
 * tree - to all primary and mirror segdbs.  To wait for completion
 * and check for errors, it is suggested that the caller use
 * cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node   *stmt,
                                 bool           cancelFromBelow,
                                 struct Gang  **pPrimaryGang,  /* OUT */
                                 struct Gang  **pMirrorGang);  /* OUT */

void
CdbDispatchUtilityStatement(struct Node *stmt);


/*--------------------------------------------------------------------*/

#endif   /* CDBDISP_H */
