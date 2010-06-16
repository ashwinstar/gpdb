/*
 *  cdbfilerepservice.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <assert.h>

#include "miscadmin.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepprimaryrecovery.h"
#include "cdb/cdbfilerepmirror.h"
#include "cdb/cdbfilerepmirrorack.h"
#include "cdb/cdbfilerepresyncmanager.h"
#include "cdb/cdbfilerepresyncworker.h"
#include "cdb/cdbvars.h"
#include "libpq/pqsignal.h"
#include "postmaster/postmaster.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"

/*
 * Initialize external variables.
 */
FileRepProcessType_e fileRepProcessType = FileRepProcessTypeNotInitialized;

static FileRepState_e fileRepState = FileRepStateNotInitialized;
/* state of FileRep process */


/* 
 * Parameters set by signal handlers for later service n the main loop 
 */
static volatile sig_atomic_t reloadConfigFile = false;

static volatile sig_atomic_t shutdownRequested = false;
/* graceful shutdown informed by SIGUSR2 signal from postmaster */

/* state change informed by SIGUSR1 signal from postmaster...when state change request comes in
 *  this counter is incremented.
 */
static volatile sig_atomic_t stateChangeRequestCounter = 0;

/**
 * This value increases when we process state change requests.  There is no pending state change request if
 *   lastChangeRequestProcessCounterValue == stateChangeRequestCounter
 *
 * Note that this value is not actually updated in the signal handlers, but must match or exceed the size of
 *    stateChangeRequestCounter so we use its type
 */
static volatile sig_atomic_t lastChangeRequestProcessCounterValue = 0;

/* it is set to TRUE in order to read configuration information at start */

static void FileRepSubProcess_SigHupHandler(SIGNAL_ARGS);
static void FileRepSubProcess_ImmediateShutdownHandler(SIGNAL_ARGS);
static void FileRepSubProcess_ShutdownHandler(SIGNAL_ARGS);
static void FileRepSubProcess_FileRepStateHandler(SIGNAL_ARGS);
static void FileRepSubProcess_HandleCrash(SIGNAL_ARGS);

static void FileRepSubProcess_ConfigureSignals(void);

extern bool FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace);

/*
 *  SIGHUP signal from main file rep process
 *  It re-loads configuration file at next convenient time.
 */
static void 
FileRepSubProcess_SigHupHandler(SIGNAL_ARGS)
{
	reloadConfigFile = true;
	elog(DEBUG1, "FileRepSubProcess_SigHupHandler()");
	
}

/* 
 *  SIGQUIT signal from main file rep process
 */
static void 
FileRepSubProcess_ImmediateShutdownHandler(SIGNAL_ARGS)
{
	elog(DEBUG1, "FileRepSubProcess_ImmediateShutdownHandler()");
    quickdie(PASS_SIGNAL_ARGS);
}

/*
 *  SIGUSR2 signal from main file rep process
 */
static void 
FileRepSubProcess_ShutdownHandler(SIGNAL_ARGS)
{
	bool	isInTransition = FALSE;
	DataState_e dataStateTransition;

	shutdownRequested = true;

	if ( FileRepIsBackendSubProcess(fileRepProcessType))
	{
		if (FileRepPrimary_IsResyncWorker())
		{
			getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, &isInTransition, &dataStateTransition);
			
			if (isInTransition == TRUE &&
				dataStateTransition == DataStateInChangeTracking)
			{
				/*
				 * Resync worker may be waiting on lock that is acquired by backend process that is
				 * suspended during transition to Change Tracking and so FileRep backend shutdown may 
				 * never be completed.
				 */
				quickdie(PASS_SIGNAL_ARGS);
				return;
			}
			
		}
			
		/* call the normal postgres die so that it requests query cancel/procdie */
		die(PASS_SIGNAL_ARGS);
    }

	elog(DEBUG1, "FileRepSubProcess_ShutdownHandler()");
}

/*
 *  SIGUSR1 signal from main file rep process
 *  It signals about data and/or segment state change.
 */
static void 
FileRepSubProcess_FileRepStateHandler(SIGNAL_ARGS)
{
	++stateChangeRequestCounter;
	elog(DEBUG1, "FileRepSubProcess_FileRepStateHandler()");	
}

/*
 *  FileRepSubProcess_ProcessSignals()
 * 
 */
bool
FileRepSubProcess_ProcessSignals()
{
	bool processExit = false;

	if (reloadConfigFile) {
		reloadConfigFile = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
	
	if (shutdownRequested) {
	    SegmentState_e segmentState;
		getPrimaryMirrorStatusCodes(NULL, &segmentState, NULL);

		shutdownRequested = false;

		if ( segmentState == FileRepStateShutdownBackends )
		{
            processExit = FileRepIsBackendSubProcess(fileRepProcessType);
            FileRepSubProcess_SetState(FileRepStateShutdownBackends);
		}
		else
		{
    		processExit = true;
    		FileRepSubProcess_SetState(FileRepStateShutdown);
		}
	}
	
	/*
	 * Immediate shutdown if postmaster is not alive to avoid
	 * manual cleanup.
	 */
#if 0
	if (! PostmasterIsAlive(true)) {
#endif
	if (!PostmasterIsAlive(false)) {
		quickdie_impl();
	}
	
	for ( ;; )
	{
	    /* check to see if change required */
	    sig_atomic_t curStateChangeRequestCounter = stateChangeRequestCounter;
	    if ( curStateChangeRequestCounter == lastChangeRequestProcessCounterValue )
	        break;
        lastChangeRequestProcessCounterValue = curStateChangeRequestCounter;

        /* do the change in local memory */
        getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);
        switch (segmentState) {

            case SegmentStateNotInitialized:
                FileRepSubProcess_SetState(FileRepStateNotInitialized);
                break;

            case SegmentStateInitialization:
                FileRepSubProcess_SetState(FileRepStateInitialization);
                break;
				
            case SegmentStateInResyncTransition:
                FileRepSubProcess_SetState(FileRepStateInitialization);
                break;

			case SegmentStateInChangeTrackingTransition:
            case SegmentStateInSyncTransition:
                // fileRepState remains Ready
                break;

            case SegmentStateChangeTrackingDisabled:
            case SegmentStateReady:
                FileRepSubProcess_SetState(FileRepStateReady);
                break;

            case SegmentStateFault:
                FileRep_ReportLog();
                FileRepSubProcess_SetState(FileRepStateFault);
                break;

            case SegmentStateShutdownFilerepBackends:
                if (fileRepRole == FileRepPrimaryRole)
                {
                    FileRepSubProcess_SetState(FileRepStateShutdownBackends);
                }
                else
                {
                    processExit = true;
                    FileRepSubProcess_SetState(FileRepStateShutdown);
                }
                break;

            case SegmentStateShutdown:
                processExit = true;
                FileRepSubProcess_SetState(FileRepStateShutdown);
                break;

            default:
                Assert(0);
                break;
        } // switch()
    }

	return(processExit);
}

bool
FileRepSubProcess_IsStateTransitionRequested(void)
{
	/* NOTE remove when Shutdown Message will be moved to FileRep backend process */
	if (fileRepProcessType == FileRepProcessTypeMain)
	{
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);
		if (segmentState == SegmentStateImmediateShutdown)
			return TRUE;
		else
			return FALSE;
	}
	
	if (fileRepProcessType != FileRepProcessTypeNotInitialized)
	{
		FileRepSubProcess_ProcessSignals();
		
		switch (FileRepSubProcess_GetState())
		{
			case FileRepStateFault:
			case FileRepStateShutdown:
				ereport(LOG,
						(errmsg("LOG(filerep): filerep state transition requested "),
						 FileRep_errcontext()));
						 
				return TRUE;
				
			default:
				break;
		}
	}

	if (fileRepProcessType == FileRepProcessTypeNotInitialized)
	{
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState, NULL, NULL);

		switch (segmentState)
		{
			case SegmentStateFault:
			case SegmentStateImmediateShutdown:
			case SegmentStateShutdown:
				ereport(LOG,
						(errmsg("LOG(filerep): backend filerep state transition requested "),
						 FileRep_errcontext()));
				
				return TRUE;
				
			case SegmentStateShutdownFilerepBackends:
				
				if (fileRepShmemArray[0]->state == FileRepStateFault)
					ereport(ERROR,
						(errmsg("failure is detected in segment mirroring during backend shutdown, abort requested (%d)",
						 getpid())));

			default:
				break;
		}		
		
	}
	
	return FALSE;
}	
	
/*
 *  FileRepSubProcess_GetState()
 *  Return state of FileRep sub-process
 */
FileRepState_e 
FileRepSubProcess_GetState(void)
{
    Assert(fileRepState != FileRepStateShutdownBackends );
	return fileRepState;
}	

/*
 *  Set state in FileRep process and sent signal to postmaster
 */
void 
FileRepSubProcess_SetState(FileRepState_e fileRepStateLocal)
{
    bool doAssignment = true;
    if ( fileRepStateLocal == FileRepStateShutdownBackends )
    {
        if ( FileRepIsBackendSubProcess(fileRepProcessType))
        {
            /* the current process must shutdown! */
            fileRepStateLocal = FileRepStateShutdown;
        }
        else
        {
            /* the current process doesn't care about shutdown backends -- leave it as shutdown */
            doAssignment = false;
        }
    }

    if ( ! doAssignment )
    {
        return;
    }

    switch (fileRepState) {
        case FileRepStateNotInitialized:
			
			fileRepState = fileRepStateLocal;
            break;

        case FileRepStateInitialization:

            switch (fileRepStateLocal) 
			{
                case FileRepStateNotInitialized:
                    Assert(0);
					ereport(LOG,
							(errmsg("unexpected filerep state transition from '%s' to '%s' ",
									FileRepStateToString[fileRepState], FileRepStateToString[fileRepStateLocal])));
					fileRepState = FileRepStateFault;							 
					break;
					
				default:
					fileRepState = fileRepStateLocal;
					break;
			}
			break;
			
        case FileRepStateReady:

            switch (fileRepStateLocal) {
                case FileRepStateFault:
                case FileRepStateShutdown:
                    fileRepState = fileRepStateLocal;
                    break;
                case FileRepStateNotInitialized:
                    Assert(0);
					ereport(LOG,
							(errmsg("unexpected filerep state transition from '%s' to '%s' ",
							 FileRepStateToString[fileRepState], FileRepStateToString[fileRepStateLocal])));
					fileRepState = FileRepStateFault;
                    break;
                case FileRepStateInitialization:
                    /* don't do assignment -- this can happen when going from segmentState Ready to InSyncTransition */
                    doAssignment = false;
                    break;
                case FileRepStateReady:
                    break;
                default:
                    Assert(0);
                    break;
            }
            break;
        case FileRepStateFault:

            switch (fileRepStateLocal) {
                case FileRepStateFault:
                case FileRepStateShutdown:
                    fileRepState = fileRepStateLocal;
                    break;
                case FileRepStateNotInitialized:
                case FileRepStateInitialization:
                case FileRepStateReady:
                    Assert(0);
					ereport(LOG,
							(errmsg("unexpected filerep state transition from '%s' to '%s' ",
							FileRepStateToString[fileRepState], FileRepStateToString[fileRepStateLocal])));
					fileRepState = FileRepStateFault;
									  
                    break;
                default:
                    Assert(0);
                    break;
            }

            break;
        case FileRepStateShutdownBackends:
            Assert(!"process filerep state should never be in ShutdownBackends");
            break;
        case FileRepStateShutdown:

            switch (fileRepStateLocal) {
                case FileRepStateShutdown:
                    fileRepState = fileRepStateLocal;
                    break;
                case FileRepStateNotInitialized:
                case FileRepStateInitialization:
                case FileRepStateReady:
                    Assert(0);
					ereport(LOG,
						 (errmsg("unexpected filerep state transition from '%s' to '%s' ",
					 	 FileRepStateToString[fileRepState], FileRepStateToString[fileRepStateLocal])));
					fileRepState = FileRepStateFault;
									  
                case FileRepStateFault:
                    break;
                default:
                    Assert(0);
                    break;
            }

            break;
        default:
            Assert(0);
            break;
    }

    /* check doAssignment again -- may have changed value in the switch above */
    if ( ! doAssignment )
    {
        return;
    }

    /* now update in shared memory if needed */
	switch (fileRepState) {
		case FileRepStateReady:
			FileRep_SetSegmentState(SegmentStateReady);
			break;
			
		case FileRepStateFault:
			/* update shared memory configuration 
			 bool updateSegmentState(FAULT);
			 return TRUE if state was updated;
			 return FALSE if state was already set to FAULT
			 change signal to PMSIGNAL_FILEREP_SEGMENT_STATE_CHANGE
			 */
			FileRep_SetSegmentState(SegmentStateFault);
			break;
		
		case FileRepStateInitialization:
		case FileRepStateShutdown:
		case FileRepStateNotInitialized:
			/* No operation */
			break;
		case FileRepStateShutdownBackends:
		    Assert(0);
            break;
        default:
            Assert(0);
            break;
	}

	/* report the change */
	if (fileRepState != FileRepStateShutdown)
	{
		elog(LOG, "FileRepSubProcess_SetState() state:'%s' MyProcPid:'%d'", 
			FileRepStateToString[FileRepSubProcess_GetState()],
			getpid());
	}
	
}
	
static void
FileRepSubProcess_InitializeResyncManagerProcess(void)
{
	char	*fullpath;
	char	*knownDatabase = "postgres";
	
	SetProcessingMode(InitProcessing);
	
	/*
	 * Create a resource owner to keep track of our resources 
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, 
								FileRepProcessTypeToString[fileRepProcessType]);	
	
	
	InitXLOGAccess();
	
	SetProcessingMode(NormalProcessing);
		
	/*
	 * In order to access the catalog, we need a database, and a
	 * tablespace; our access to the heap is going to be slightly
	 * limited, so we'll just use some defaults.
	 */
	MyDatabaseId = TemplateDbOid;
	MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
	
	if (!FindMyDatabase(knownDatabase, &MyDatabaseId, &MyDatabaseTableSpace))
		ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE),
						errmsg("database 'postgres' does not exist")));
	
	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
	
	SetDatabasePath(fullpath);
	
	InitBufferPoolAccess();
	/*
	 * Finish filling in the PGPROC struct, and add it to the ProcArray. (We
	 * need to know MyDatabaseId before we can do this, since it's entered
	 * into the PGPROC struct.)
	 *
	 * Once I have done this, I am visible to other backends!
	 */
	InitProcessPhase2();
	
	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.
	 *
	 * Sets up MyBackendId, a unique backend identifier.
	 */
	MyBackendId = InvalidBackendId;
	
	InitBackendSharedInvalidationState();
	
	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend id: %d", MyBackendId);
	
	/*
	 * bufmgr needs another initialization call too
	 */
	InitBufferPoolBackend();
	
	/* heap access requires the rel-cache */
	RelationCacheInitialize();
	InitCatalogCache();
	
	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Load relcache entries for the system catalogs.  This must create at
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase2();	
	
	/* No need to StartupXLOG_Pass2(); since we're not writing any data to disk */
}

static void
FileRepSubProcess_HandleCrash(SIGNAL_ARGS)
{
    StandardHandlerForSigillSigsegvSigbus_OnMainThread("a file replication subprocess", PASS_SIGNAL_ARGS);
}

/*
 *
 */
static void
FileRepSubProcess_ConfigureSignals(void)					 
{
	/* Accept Signals */
	/* emergency shutdown */
	pqsignal(SIGQUIT, FileRepSubProcess_ImmediateShutdownHandler);
	
	/* graceful shutdown */
	pqsignal(SIGUSR2, FileRepSubProcess_ShutdownHandler);
	
	/* reload configuration file */
	pqsignal(SIGHUP, FileRepSubProcess_SigHupHandler);
	
	/* data or segment state changed */
	pqsignal(SIGUSR1, FileRepSubProcess_FileRepStateHandler);
	
	/* Ignore Signals */
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
		
	/* Use default action */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGINT, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

#ifdef SIGSEGV
	pqsignal(SIGSEGV, FileRepSubProcess_HandleCrash);
#endif
	
#ifdef SIGILL
	pqsignal(SIGILL, FileRepSubProcess_HandleCrash);
#endif	
	
#ifdef SIGBUS
    pqsignal(SIGBUS, FileRepSubProcess_HandleCrash);
#endif
	
}

/*
 *
 */
void
FileRepSubProcess_Main()
{
	const char *statmsg;
	
	MemoryContext	fileRepSubProcessMemoryContext;
	
	sigjmp_buf		local_sigjmp_buf;

	MyProcPid = getpid();
	
	MyStartTime = time(NULL);
	
	/*
	 * Create a PGPROC so we can use LWLocks in FileRep sub-processes.  
	 * The routine also register clean up at process exit
	 */
	InitAuxiliaryProcess();	
	
	InitBufferPoolBackend();
	
	FileRepSubProcess_ConfigureSignals();
	
	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();
		
		/* Report the error to the server log */
		EmitErrorReport();
		
		/*
		 * We can now go away.	Note that because we'll call InitProcess, a
		 * callback will be registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}
	
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;
	
	PG_SETMASK(&UnBlockSig);	
	
	/*
	 * Identify myself via ps
	 */
		
	statmsg = FileRepProcessTypeToString[fileRepProcessType];
		
	init_ps_display(statmsg, "", "", "");
	
	/* Create the memory context where cross-transaction state is stored */
	fileRepSubProcessMemoryContext = AllocSetContextCreate(TopMemoryContext,
										 "FileRep SubProcess memory context",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
	
	MemoryContextSwitchTo(fileRepSubProcessMemoryContext);
	
	elog(LOG, "FileRepSubProcess_Main() MyProcPid:%d (%s) MyProc->pid:%d", 
		 getpid(), 
 		 FileRepProcessTypeToString[fileRepProcessType], 
		 MyProc->pid);
	
	stateChangeRequestCounter++;
	
	FileRepSubProcess_ProcessSignals();
	
	switch (fileRepProcessType) {
	
		case FileRepProcessTypePrimarySender:
			FileRepPrimary_StartSender();
			break;
	
		case FileRepProcessTypeMirrorReceiver:
			FileRepMirror_StartReceiver();
			break;	

		case FileRepProcessTypeMirrorConsumer:
		case FileRepProcessTypeMirrorConsumerWriter:
		case FileRepProcessTypeMirrorConsumerAppendOnly1:
		case FileRepProcessTypeMirrorConsumerAppendOnly2:
			FileRepMirror_StartConsumer();
			break;	

		case FileRepProcessTypeMirrorSenderAck:
			FileRepAckMirror_StartSender();
			break;
	
		case FileRepProcessTypePrimaryReceiverAck:
			FileRepAckPrimary_StartReceiver();
			break;	

		case FileRepProcessTypePrimaryConsumerAck:
			FileRepAckPrimary_StartConsumer();
			break;
			
		case FileRepProcessTypePrimaryRecovery:
			FileRepSubProcess_InitializeResyncManagerProcess();
			FileRepPrimary_StartRecovery();
			
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_BEFORE_LOCKS,
								 false, true);							
			break;

		case FileRepProcessTypeResyncManager:
			FileRepSubProcess_InitializeResyncManagerProcess();
			FileRepPrimary_StartResyncManager();
			
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_BEFORE_LOCKS,
								 false, true);				
			break;
			
		case FileRepProcessTypeResyncWorker1:
		case FileRepProcessTypeResyncWorker2:
		case FileRepProcessTypeResyncWorker3:
		case FileRepProcessTypeResyncWorker4:

			FileRepSubProcess_InitializeResyncManagerProcess();
			FileRepPrimary_StartResyncWorker();
			
			ResourceOwnerRelease(CurrentResourceOwner,
								 RESOURCE_RELEASE_BEFORE_LOCKS,
								 false, true);							
			break;
			
		default:
			elog(PANIC, "unrecognized process type: %s(%d)", 
				 statmsg, fileRepProcessType);
			break;
	}

	switch (FileRepSubProcess_GetState()) {
		case FileRepStateShutdown:
		case FileRepStateReady:
			proc_exit(0);
			break;
			
		default:
			proc_exit(2);
			break;
	}
}





