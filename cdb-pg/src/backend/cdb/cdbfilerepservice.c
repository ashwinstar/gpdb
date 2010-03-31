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
#include "libpq/pqsignal.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/syscache.h"

extern void die(SIGNAL_ARGS); /* is in tcopprot.h */
extern void quickdie(SIGNAL_ARGS); /* is in tcopprot.h */

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

static volatile sig_atomic_t immediateShutdownRequested = false;
/* immediate shutdown informed by SIGQUIT signal from postmaster */

static volatile sig_atomic_t stateChangeRequested = true;
/* state change informed by SIGUSR1 signal from postmaster */
/* it is set to TRUE in order to read configuration information at start */

static void FileRepSubProcess_SigHupHandler(SIGNAL_ARGS);
static void FileRepSubProcess_ImmediateShutdownHandler(SIGNAL_ARGS);
static void FileRepSubProcess_ShutdownHandler(SIGNAL_ARGS);
static void FileRepSubProcess_FileRepStateHandler(SIGNAL_ARGS);

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
	immediateShutdownRequested = true;
	if ( FileRepIsBackendSubProcess(fileRepProcessType))
	{
	    /* call the normal postgres die so that it requests query cancel/procdie */
	    quickdie(PASS_SIGNAL_ARGS);
    }
	elog(DEBUG1, "FileRepSubProcess_ImmediateShutdownHandler()");
}

/*
 *  SIGUSR2 signal from main file rep process
 */
static void 
FileRepSubProcess_ShutdownHandler(SIGNAL_ARGS)
{
	shutdownRequested = true;

	if ( FileRepIsBackendSubProcess(fileRepProcessType))
	{
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
	stateChangeRequested = true;
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
	
	if (immediateShutdownRequested) {
		processExit = true;
		FileRepSubProcess_SetState(FileRepStateImmediateShutdown);
	}
	
	/*
	 * Immediate shutdown if postmaster is not alive to avoid
	 * manual cleanup.
	 */
#if 0
	if (! PostmasterIsAlive(true)) {
#endif
	if (!PostmasterIsAlive(false)) {
		processExit = true;
		FileRepSubProcess_SetState(FileRepStateImmediateShutdown);
	}
	
	/*
	 * Immediate shutdown if postmaster is not alive to avoid
	 * manual cleanup.
	 */
	/*
	if (! FileRepMainIsAlive(true)) {
		processExit = true;
		FileRepSubProcess_SetState(FileRepStateImmediateShutdown);
	}
	*/
	if (stateChangeRequested) {

		stateChangeRequested = FALSE;
		
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
		
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

			case SegmentStateImmediateShutdown:
				processExit = true;
				FileRepSubProcess_SetState(FileRepStateImmediateShutdown);
				break;
				
            default:
                Assert(0);
		        break;
		} // switch()
	} // if (stateChangeRequested)

	return(processExit);
}

bool
FileRepSubProcess_IsStateTransitionRequested(void)
{
	/* NOTE remove when Shutdown Message will be moved to FileRep backend process */
	if (fileRepProcessType == FileRepProcessTypeMain)
	{
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);
		if (segmentState == FileRepStateImmediateShutdown)
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
			case FileRepStateImmediateShutdown:
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
		getFileRepRoleAndState(&fileRepRole, &segmentState, &dataState);

		switch (segmentState)
		{
			case SegmentStateFault:
			case SegmentStateImmediateShutdown:
			case SegmentStateShutdown:
				ereport(LOG,
						(errmsg("LOG(filerep): backend filerep state transition requested "),
						 FileRep_errcontext()));
				
				return TRUE;
				
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

    if ( doAssignment)
    {
        switch (fileRepState) {
            case FileRepStateNotInitialized:
            case FileRepStateInitialization:

                fileRepState = fileRepStateLocal;
                break;
            case FileRepStateReady:

                switch (fileRepStateLocal) {
                    case FileRepStateFault:
                    case FileRepStateImmediateShutdown:
                    case FileRepStateShutdown:
                        fileRepState = fileRepStateLocal;
                        break;
                    case FileRepStateNotInitialized:
                    case FileRepStateInitialization:
                        Assert(0);
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
                    case FileRepStateImmediateShutdown:
                    case FileRepStateShutdown:
                        fileRepState = fileRepStateLocal;
                        break;
                    case FileRepStateNotInitialized:
                    case FileRepStateInitialization:
                    case FileRepStateReady:
                        Assert(0);
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
                    case FileRepStateImmediateShutdown:
                    case FileRepStateShutdown:
                        fileRepState = fileRepStateLocal;
                        break;
                    case FileRepStateNotInitialized:
                    case FileRepStateInitialization:
                    case FileRepStateReady:
                        Assert(0);
                    case FileRepStateFault:
                        break;
                    default:
                        Assert(0);
                        break;
                }

                break;
            case FileRepStateImmediateShutdown:

                break;
            default:
                Assert(0);
                break;
        }
    }

	switch (fileRepState) {
		case FileRepStateReady:
			if (updateSegmentState(SegmentStateReady)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}
			break;
			
		case FileRepStateFault:
			/* update shared memory configuration 
			 bool updateSegmentState(FAULT);
			 return TRUE if state was updated;
			 return FALSE if state was already set to FAULT
			 change signal to PMSIGNAL_FILEREP_SEGMENT_STATE_CHANGE
			 */
			if (updateSegmentState(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}
			break;
		
		case FileRepStateInitialization:
		case FileRepStateShutdown:
		case FileRepStateImmediateShutdown:
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
	
	if (fileRepState != FileRepStateShutdown &&
		fileRepState != FileRepStateImmediateShutdown)
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

#ifdef DEBUG_FILEREP
static void
bug_handler(SIGNAL_ARGS)
{
	pg_usleep(2000000000L);
    ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
                    errOmitLocation(true),
                    errmsg("Unexpected internal error"))); 
}
#endif

/*
 *
 */
static void
FileRepSubProcess_ConfigureSignals(void)					 
{
	
	sigjmp_buf		local_sigjmp_buf;
	
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

#ifdef DEBUG_FILEREP
#ifdef SIGSEGV
	pqsignal(SIGSEGV, bug_handler);
#endif
#ifdef SIGBUS
    pqsignal(SIGBUS, bug_handler);
#endif
#endif
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
}

/*
 *
 */
void
FileRepSubProcess_Main()
{
	const char *statmsg;
	
	MemoryContext	fileRepSubProcessMemoryContext;

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
	
	stateChangeRequested = TRUE;
	
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
			FileRepPrimary_StartRecovery();
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
			proc_exit(0);
			break;
			
		case FileRepStateImmediateShutdown:
			proc_exit(2);
			break;
			
		case FileRepStateReady:
			proc_exit(0);
			break;
			
		default:
			proc_exit(2);
			break;
	}
}





