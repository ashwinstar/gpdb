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

#include "cdb/cdbselect.h"
#include "cdb/cdbtm.h"
#include <pthread.h>

struct CdbDispatchResult;           /* #include "cdb/cdbdispatchresult.h" */
struct CdbDispatchResults;          /* #include "cdb/cdbdispatchresult.h" */
struct pg_result;                   /* #include "libpq-fe.h" */
struct Gang;                        /* #include "cdb/cdbgang.h" */
struct Node;                        /* #include "nodes/nodes.h" */
struct QueryDesc;                   /* #include "executor/execdesc.h" */
struct SegmentDatabaseDescriptor;   /* #include "cdb/cdbconn.h" */

typedef enum
{
	GP_DISPATCH_COMMAND_TYPE_QUERY,
	GP_DISPATCH_COMMAND_TYPE_DTX_PROTOCOL
} GpDispatchCommandType;

/*
 * Parameter structure for Greenplum Database Queries
 */
typedef struct DispatchCommandQueryParms
{
	/*
	 * The SQL command
	 */
	const char	*strCommand;
	int			strCommandlen;
	char		*serializedQuerytree;
	int			serializedQuerytreelen;
	char		*serializedPlantree;
	int			serializedPlantreelen;
	char		*serializedParams;
	int			serializedParamslen;
	char		*serializedSliceInfo;
	int			serializedSliceInfolen;
	
	/*
	 * serialized DTX context string
	 */
	char		*serializedDtxContextInfo;
	int			serializedDtxContextInfolen;

} DispatchCommandQueryParms;

/*
 * Parameter structure for DTX protocol commands
 */
typedef struct DispatchCommandDtxProtocolParms
{
	DtxProtocolCommand			dtxProtocolCommand;
	char*						dtxProtocolCommandLoggingStr;
	char						gid[TMGIDSIZE];
	DistributedTransactionId	gxid;
} DispatchCommandDtxProtocolParms;

/*
 * Parameter structure for the DispatchCommand threads
 */
typedef struct DispatchCommandParms
{
	GpDispatchCommandType			mppDispatchCommandType;
	DispatchCommandQueryParms		queryParms;
	DispatchCommandDtxProtocolParms	dtxProtocolParms;
	
	char		*query_text;
	int			query_text_len;
	
	int			localSlice;
	
	/*
	 * options for controlling certain behavior
	 * on the remote backend before query processing. Like
	 * explicitly opening a transaction.
	 */
	int			txnOptions;
	
	/*
	 * store the command here, so that we make sure that every
	 * thread gets the same command-id
	 */
	int			cmdID;
	
	/*
	 * db_count: The number of segdbs that this thread is responsible
	 * for dispatching the command to.
	 * Equals the count of segdbDescPtrArray below.
	 */
	int			db_count;
	
	/*
	 * dispatchResultPtrArray: Array[0..db_count-1] of CdbDispatchResult*
	 * Each CdbDispatchResult object points to a SegmentDatabaseDescriptor
	 * that this thread is responsible for dispatching the command to.
	 */
	struct CdbDispatchResult **dispatchResultPtrArray;

	/*
	 * mpp_fd_set supports more file descriptors than an ordinary fd_set
	 */
	mpp_fd_set	mask;
	
	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;
	bool		thread_valid;
	
}	DispatchCommandParms;

/*
 * Keeps state of all the dispatch command threads.
 */
typedef struct CdbDispatchCmdThreads
{
	struct DispatchCommandParms *dispatchCommandParmsAr;
	int	dispatchCommandParmsArSize;
	int	threadCount;
	
}   CdbDispatchCmdThreads;

/*--------------------------------------------------------------------*/

/* Mark the DTX as distributed and reconfigure the cluster if needed. */
void
cdbdisp_markDtxAsDistributed(char* debugCaller, char* debugDetail);

/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchToGang:
 * Send the strCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The caller must also provide a serialized Snapshot string to be used to
 * set the distributed snapshot for the dispatched statement.
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
 * When dispatchResults->cancelOnError is false, strCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
cdbdisp_dispatchToGang(struct CdbDispatchCmdThreads **dispatchThreads,
					   GpDispatchCommandType		mppDispatchCommandType,
					   void						   *commandTypeParms,
                       struct Gang                 *gp,
                       struct CdbDispatchResults   *dispatchResults,
                       int                          sliceIndex,
                       unsigned int                 maxSlices);

/*
 * CdbCheckDispatchResult:
 *
 * Waits for completion of threads launched by cdbdisp_dispatchToGang().
 *
 * If 'cancelUnfinishedWork' is true, QEs that were dispatched with
 * 'cancelOnError' true and are not yet idle will be canceled.
 */
void
CdbCheckDispatchResult(struct CdbDispatchCmdThreads *dispatchThreads, bool cancelUnfinishedWork);

void
CdbCheckDispatchResultInt(struct CdbDispatchCmdThreads  *dispatchThreads,
						  struct SegmentDatabaseDescriptor   ***failedSegDB,
                          int          *numOfFailed,
                          bool          cancelUnfinishedWork);

/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchRMCommand:
 * Sends a non-cancelable command to all segment dbs, primary (and mirrors
 * if requested).
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
cdbdisp_dispatchRMCommand(const char   *strCommand,
						  bool			withSnapshot,
						  bool			dispatchToMirrors,
                          StringInfo    errmsgbuf,
                          int			*numresults);


/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchDtxProtocolCommand:
 * Sends a non-cancelable command to all segment dbs, primary (and mirrors
 * if requested).
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
cdbdisp_dispatchDtxProtocolCommand(DtxProtocolCommand dtxProtocolCommand,
						  char*						dtxProtocolCommandLoggingStr,
						  char*						gid,
						  DistributedTransactionId	gxid,
						  bool						dispatchToMirrors,
                          StringInfo    			errmsgbuf,
                          int						*numresults);


/*--------------------------------------------------------------------*/

/*
 * cdbdisp_dispatchCommand:
 * Send ths strCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The startTransaction flag is used to express intent on whether the command to
 * be dispatched should be done inside of a global transaction or not.
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
cdbdisp_dispatchCommand(const char                 *strCommand,
						char				   	   *serializedQuerytree,
						int							serializedQuerytreelen,
                        bool                        cancelOnError,
                        bool						startTransaction,
                        bool						withSnapshot,
                        bool						dispatchToMirrors,
						bool						dispatchToPrimaries,
                        struct CdbDispatchResults **pPrimaryResults,    /* OUT */
                        struct CdbDispatchResults **pMirrorResults,		/* OUT */
						struct CdbDispatchCmdThreads **pDispatchThreads);  /* OUT */

/* Wait for all QEs to finish, then report any errors from the given
 * CdbDispatchResults objects and free them.  If not all QEs in the
 * associated gang(s) executed the command successfully, throws an
 * error and does not return.  No-op if both CdbDispatchResults ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct CdbDispatchResults    *primaryResults,
                      struct CdbDispatchResults    *mirrorResults,
					  struct CdbDispatchCmdThreads *dispatchThreads);

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
				    struct CdbDispatchResults  *mirrorResults,
					struct CdbDispatchCmdThreads *dispatchThreads);

/*
 * CdbDoCommand:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * Called by general users, this method includes global transaction control.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 *
 * startTransaction specifies whether to dispatch within a distributed 
 * transaction or not.
 */
void
CdbDoCommand(const char *strCommand, bool cancelOnError, bool startTransaction, bool dispatchToMirrors, bool dispatchToPrimaries);


/*
 * Special for sending SET commands that change GUC variables, so they go to all
 * gangs, both reader and writer
 */
void
CdbDoCommandOnAllGangs(const char *strCommand, bool cancelOnError, bool startTransaction);

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
                     bool                           planRequiresTxn,
                     bool                           cancelOnError,
                     struct CdbDispatchResults    **pPrimaryResults,  /* OUT */
                     struct CdbDispatchResults    **pMirrorResults,   /* OUT */
					 struct CdbDispatchCmdThreads **pDispatchThreads); /* OUT */

/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs.  Does not wait for
 * completion.  Does not start a global transaction.
 *
 *
 * The startTransaction flag indicates whether you want the dispatched 
 * statement to participate in a distributed transaction or not.
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
                                 bool			startTransaction,
                                 struct CdbDispatchResults    **pPrimaryResults,
                                 struct CdbDispatchResults    **pMirrorResults,
								 struct CdbDispatchCmdThreads ** pDispatchThreads);

/* Dispatch a command - already parsed and in the form of a Node
 * tree - to all primary and mirror segdbs, and wait for completion.
 * Starts a global transaction first, if not already started.
 * If not all QEs in the given gang(s) executed the command successfully,
 * throws an error and does not return.
 */
void
CdbDispatchUtilityStatement(struct Node *stmt);

/*
 * create a CdbDispatchCmdThreads object that holds the dispatch
 * threads state, and an array of dispatch command params. 
 */
CdbDispatchCmdThreads *
cdbdisp_makeDispatchThreads(int paramCount);

/*
 * free all memory allocated for a CdbDispatchCmdThreads object.
 */
void
cdbdisp_destroyDispatchThreads(CdbDispatchCmdThreads *dThreads);

/* used to take the current Transaction Snapshot and serialized a version of it
 * into the static variable serializedDtxContextInfo */
char *
qdSerializeDtxContextInfo(int * size, bool wantSnapshot, int txnOptions);


/* this is used for dire cleanup emergencies (in portalmem.c) */
void
CdbShutdownPortals(void);
/*--------------------------------------------------------------------*/

#endif   /* CDBDISP_H */
