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
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerepprimaryrecovery.h"
#include "cdb/cdbfilerepmirror.h"
#include "cdb/cdbfilerepmirrorack.h"
#include "libpq/pqsignal.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


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
	elog(DEBUG1, "FileRepSubProcess_ImmediateShutdownHandler()");
}

/*
 *  SIGUSR2 signal from main file rep process
 */
static void 
FileRepSubProcess_ShutdownHandler(SIGNAL_ARGS)
{
	shutdownRequested = true;
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
		processExit = true;
		FileRepSubProcess_SetState(FileRepStateShutdown);
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
				
				switch (dataState) {
					case DataStateInSync:
						FileRepSubProcess_SetState(FileRepStateInitialization);
						break;
						
					case DataStateInResync:	
						if (FileRepSubProcess_GetState() == FileRepStateSuspend ||
						    FileRepSubProcess_GetState() == FileRepStateNotInitialized)
                        {
							FileRepSubProcess_SetState(FileRepStateInitialization);
						}
						break;
						
					case DataStateInChangeLogging:
						FileRepSubProcess_SetState(FileRepStateSuspend);
						break;
						
					case DataStateNotInitialized:
						Assert(0);
						break;
				}		
				break;
				
			case SegmentStateRecovery:
				/* file rep state is in Ready or in Suspend state */
				 
				 switch (dataState) {
				 case DataStateInSync:
				 case DataStateInResync:					
						 FileRepSubProcess_SetState(FileRepStateReady);
						 break;
				 
				 case DataStateInChangeLogging:
						 FileRepSubProcess_SetState(FileRepStateSuspend);
						 break;
				 
				 case DataStateNotInitialized:
						 Assert(0);
						 break;
				 }		
				 
				break;
				
			case SegmentStateReady:
				switch (dataState) {
					case DataStateInSync:
					case DataStateInResync:					
						FileRepSubProcess_SetState(FileRepStateReady);
						break;
						
					case DataStateInChangeLogging:
						FileRepSubProcess_SetState(FileRepStateSuspend);
						break;
						
					case DataStateNotInitialized:
						Assert(0);
						break;
				}		
				
				break;
				
			case SegmentStateFault:
				FileRep_ReportLog();
				FileRepSubProcess_SetState(FileRepStateFault);
				break;
				
			case SegmentStateShutdown:
				FileRepSubProcess_SetState(FileRepStateShutdown);
				break;
				
			case SegmentStateImmediateShutdown:
				FileRepSubProcess_SetState(FileRepStateImmediateShutdown);
				break;
		
		} // switch()
	} // if (stateChangeRequested)
	
	return(processExit);
}

/*
 *  FileRepSubProcess_GetState()
 *  Return state of FileRep sub-process
 */
FileRepState_e 
FileRepSubProcess_GetState(void)
{
	return fileRepState;
}	

/*
 *  Set state in FileRep process and sent signal to postmaster
 */
void 
FileRepSubProcess_SetState(FileRepState_e fileRepStateLocal)
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
				case FileRepStateSuspend:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
					Assert(0);
					break;
				case FileRepStateReady:
					break;
			}
			break;
		case FileRepStateFault:
			
			switch (fileRepStateLocal) {
				case FileRepStateFault:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
				case FileRepStateSuspend:
					fileRepState = fileRepStateLocal;
					break;
				case FileRepStateNotInitialized:
				case FileRepStateInitialization:
				case FileRepStateReady:
					Assert(0);
					break;
			}			
			
			break;
		case FileRepStateSuspend:
			
			switch (fileRepStateLocal) {
				case FileRepStateInitialization:
				case FileRepStateFault:
				case FileRepStateImmediateShutdown:
				case FileRepStateShutdown:
				case FileRepStateSuspend:
					fileRepState = fileRepStateLocal;
					break;
					
				case FileRepStateNotInitialized:
				case FileRepStateReady:
					Assert(0);
					break;
			}			
			
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
				case FileRepStateSuspend:
					Assert(0);
				case FileRepStateFault:
					break;					
			}
			
			break;
		case FileRepStateImmediateShutdown:
			
			break;
	}
	
	switch (fileRepState) {
		case FileRepStateReady:
			if (updateConfig(SegmentStateReady)) {
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
			if (updateConfig(SegmentStateFault)) {
				SendPostmasterSignal(PMSIGNAL_FILEREP_STATE_CHANGE);
			}
			break;
		
		case FileRepStateSuspend:
		case FileRepStateInitialization:
		case FileRepStateShutdown:	
		case FileRepStateImmediateShutdown:
		case FileRepStateNotInitialized:
			/* No operation */
			break;
	}
	
	elog(LOG, "FileRepSubProcess_SetState() state:'%s' MyProcPid:'%d'", 
		FileRepStateToString[FileRepSubProcess_GetState()],
		getpid());
	
}

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
	
	FileRepSubProcess_ProcessSignals();
	
	switch (fileRepProcessType) {
	
		case FileRepProcessTypePrimarySender:
			FileRepPrimary_StartSender();
			break;
	
		case FileRepProcessTypeMirrorReceiver:
			FileRepMirror_StartReceiver();
			break;	

		case FileRepProcessTypeMirrorConsumer:
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
			Assert(fileRepProcessType == FileRepProcessTypePrimaryRecovery);
			proc_exit(0);
			break;
		default:
			Assert(fileRepProcessType != FileRepProcessTypePrimaryRecovery);
			proc_exit(2);
	}
}





