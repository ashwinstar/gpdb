/*-------------------------------------------------------------------------
 *
 * walredoserver.c
 *	  Process under QD postmaster that manages redo of the WAL by the standby.
 *
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "utils/elog.h"


#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "cdb/cdbvars.h"
#include "commands/sequence.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "catalog/pg_control.h"
#include "postmaster/walredoserver.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/pmsignal.h"			/* PostmasterIsAlive */
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/ps_status.h"
#include "cdb/cdbutil.h"
#include "cdb/cdblink.h"
#include "access/xlog_internal.h"
#include "utils/memutils.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "cdb/cdblogsync.h"


/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */

static bool WalRedoServerGetCheckpointLocation(XLogRecPtr *newCheckpointLocation);
static void WalRedoServerMarkError(void);

#ifdef EXEC_BACKEND
static pid_t walredoserver_forkexec(void);
#endif
NON_EXEC_STATIC void WalRedoServerMain(int argc, char *argv[]);

static void WalRedoServerLoop(void);

/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

typedef struct WalRedoServerControlBlock
{
	slock_t		spinLockWalRedoServer;	/* Locks WAL redo server variables. */
	bool		redoErrorOccurred;
	bool		updated;
	XLogRecPtr  highWaterMark;
}	WalRedoServerControlBlock;


static WalRedoServerControlBlock *walRedoServerCtl = NULL;

static bool am_walredoserver = false;


/*=========================================================================
 * VISIBLE FUNCTIONS
 */

int 
WalRedoServerShmemSize(void)
{
	
	return sizeof(WalRedoServerControlBlock);
}


void 
WalRedoServerShmemInit(void)
{
	bool		found;
	
	
	/* Create or attach to the SharedSnapshot shared structure */
	walRedoServerCtl = (WalRedoServerControlBlock *) ShmemInitStruct("WAL Redo Server", WalRedoServerShmemSize(), &found);

	if (!found)
	{
		SpinLockInit(&walRedoServerCtl->spinLockWalRedoServer);
		walRedoServerCtl->redoErrorOccurred = false;
		walRedoServerCtl->updated = false;
		memset(&walRedoServerCtl->highWaterMark, 0, sizeof(XLogRecPtr));
	}
}

bool 
WalRedoServerNewCheckpointLocation(XLogRecPtr *newCheckpointLocation)
{
	bool redoErrorOccurred;
	
	if (walRedoServerCtl != NULL)
	{
		SpinLockAcquire(&walRedoServerCtl->spinLockWalRedoServer);

		redoErrorOccurred = walRedoServerCtl->redoErrorOccurred;

		if (!redoErrorOccurred)
		{
			walRedoServerCtl->updated = true;
			walRedoServerCtl->highWaterMark = *newCheckpointLocation;
		}
		
		SpinLockRelease(&walRedoServerCtl->spinLockWalRedoServer);

		return !redoErrorOccurred;
	}

	return false;
}

static bool 
WalRedoServerGetCheckpointLocation(XLogRecPtr *newCheckpointLocation)
{
	bool updated;
	
	if (walRedoServerCtl != NULL)
	{
		SpinLockAcquire(&walRedoServerCtl->spinLockWalRedoServer);

		if (!walRedoServerCtl->redoErrorOccurred)
		{
			updated = walRedoServerCtl->updated;
			if (updated)
			{
				*newCheckpointLocation = walRedoServerCtl->highWaterMark;
				walRedoServerCtl->updated = false;
			}
		}
		else
			updated = false;
		
		SpinLockRelease(&walRedoServerCtl->spinLockWalRedoServer);

		return updated;
	}

	return false;
}

static void 
WalRedoServerMarkError(void)
{
	Assert(walRedoServerCtl != NULL);
	
	SpinLockAcquire(&walRedoServerCtl->spinLockWalRedoServer);

	walRedoServerCtl->redoErrorOccurred = true;
	
	SpinLockRelease(&walRedoServerCtl->spinLockWalRedoServer);

}



/*
 * Main entry point for walredoserver controller process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
walredoserver_start(void)
{

	pid_t		WalRedoServerPID;

	
#ifdef EXEC_BACKEND
	switch ((WalRedoServerPID = walredoserver_forkexec()))
#else
	switch ((WalRedoServerPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork walredoserver process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			WalRedoServerMain(0, NULL);
			break;
#endif
		default:
			return (int) WalRedoServerPID;
	}

	
	/* shouldn't get here */
	return 0;
}

/*=========================================================================
 * HELPER FUNCTIONS
 */


#ifdef EXEC_BACKEND
/*
 * walredoserver_forkexec()
 *
 * Format up the arglist for the serqserver process, then fork and exec.
 */
static pid_t
walredoserver_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkwalredoserver";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */

bool walredo_shutdown_requested=false;

static void
RequestShutdown(SIGNAL_ARGS)
{
	walredo_shutdown_requested = true;
}

/*
 * ss_quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
ss_quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * DO NOT proc_exit() -- we're here because shared memory may be
	 * corrupted, so we don't want to try to clean up our transaction. Just
	 * nail the windows shut and get out of town.
	 *
	 * Note we do exit(2) not exit(0).	This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.
	 */
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"ss_quickdie: calling exit(2)");
	exit(2);
}

/*
 * AutoVacMain
 */
NON_EXEC_STATIC void
WalRedoServerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	IsUnderPostmaster = true;
	am_walredoserver = true;
	
	/* reset MyProcPid */
	MyProcPid = getpid();
	
	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/* Identify myself via ps */
	init_ps_display("walredoserver process", "", "", "");

	// UNDONE: To allow us to perform recovery.
	SetProcessingMode(BootstrapProcessing);

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes that
	 * happen during a single daemon iteration, so we can ignore SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);

	/*
	 * Presently, SIGINT will lead to autovacuum shutdown, because that's how
	 * we handle ereport(ERROR).  It could be improved however.
	 */
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	pqsignal(SIGQUIT, ss_quickdie);
	pqsignal(SIGALRM, PGSemaphoreHandleSigAlarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, RequestShutdown);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	/* See InitPostgres()... */
    InitProcess();	
	InitBufferPoolBackend();
	InitXLOGAccess();

	SetProcessingMode(NormalProcessing);

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

	/* main loop */
	WalRedoServerLoop();

	/* One iteration done, go away */
	proc_exit(0);
}

void 
WalRedoServerLoop(void)
{
	bool updated;
	XLogRecPtr newCheckpointLocation;
	XLogRecPtr redoCheckPointLoc;
	CheckPoint redoCheckpoint;
	
	Assert(TopMemoryContext != NULL);
	MemoryContextSwitchTo(TopMemoryContext);
	Assert(CurrentMemoryContext == TopMemoryContext);

	memset(&redoCheckPointLoc, 0, sizeof(XLogRecPtr));
	
	while(true)
	{
		/*
		 * Come up for air every 1 seconds to see if there is work to do,
		 * if postmaster has died, a shutdown has been requested, etc.
		 */
		pg_usleep(1000000);		/* 1 sec */
		
		if (!PostmasterIsAlive(true))
		{
			/*
			 * Gracefully drain our queue so requesting backend don't get hung
			 * even though the postmaster has croaked.
			 */
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"postmaster has died -- draining.");
			break;
		}

		updated = WalRedoServerGetCheckpointLocation(&newCheckpointLocation);
		if (updated)
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"new checkpoint location %s",
				 XLogLocationToString(&newCheckpointLocation));

			PG_TRY();
			{
				cdb_perform_redo(&redoCheckPointLoc, &redoCheckpoint, &newCheckpointLocation);
			}
			PG_CATCH();
			{
				/* 
				 * Report the error related to reading the primary's WAL
				 * to the server log 
				 */
				EmitErrorReport();
				FlushErrorState();

				elog(NOTICE,"error occurred during redo");
				WalRedoServerMarkError();
			}
			PG_END_TRY();
		}
	}
}
