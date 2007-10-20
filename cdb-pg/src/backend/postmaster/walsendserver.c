/*-------------------------------------------------------------------------
 *
 * walsendserver.c
 *	  Process under QD postmaster that manages sending WAL to the standby.
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


#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "cdb/cdbvars.h"
#include "commands/sequence.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "postmaster/walsendserver.h"
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

// To avoid pulling in cdbfts.h
extern bool isQDMirroringEnabled(void);	
extern void disableQDMirroring(void);
extern void unexpectedErrorQDMirroring(void);

/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */


#ifdef EXEC_BACKEND
static pid_t walsendserver_forkexec(void);
#endif
NON_EXEC_STATIC void WalSendServerMain(int argc, char *argv[]);

static void WalSendServerInitContext(void);
static void WalSendServerDoRequest(WalSendRequest *walSendRequest, PROC_RESPONSE_COOKIE *responseCookie);
static void WalSendServerLoop(void);

/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

static bool am_walsendserver = false;


/*=========================================================================
 * VISIBLE FUNCTIONS
 */

/*
 * Main entry point for walsendserver controller process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
walsendserver_start(void)
{

	pid_t		WalSendServerPID;

	
#ifdef EXEC_BACKEND
	switch ((WalSendServerPID = walsendserver_forkexec()))
#else
	switch ((WalSendServerPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork walsendserver process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			WalSendServerMain(0, NULL);
			break;
#endif
		default:
			return (int) WalSendServerPID;
	}

	
	/* shouldn't get here */
	return 0;
}

/*=========================================================================
 * HELPER FUNCTIONS
 */


#ifdef EXEC_BACKEND
/*
 * walsendserver_forkexec()
 *
 * Format up the arglist for the serqserver process, then fork and exec.
 */
static pid_t
walsendserver_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkwalsendserver";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */

bool walsend_shutdown_requested=false;

static void
RequestShutdown(SIGNAL_ARGS)
{
	walsend_shutdown_requested = true;
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

	// UNDONE: Is this wise?
	ProcSetAcceptingNewRequests(PGPROC_REQUEST_WALSEND, false);
	
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
WalSendServerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	IsUnderPostmaster = true;
	am_walsendserver = true;
	
	/* reset MyProcPid */
	MyProcPid = getpid();
	
	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/* Identify myself via ps */
	init_ps_display("walsendserver process", "", "", "");

	SetProcessingMode(InitProcessing);

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
	WalSendServerLoop();

	/* One iteration done, go away */
	proc_exit(0);
}

static XLogRecPtr 	originalEndLocation;
static XLogRecPtr 	currentEndLocation;
static uint32 		periodicLen;
static uint32 		periodicReportLen;
static XLogRecPtr 	periodicLocation;

static void
WalSendServerInitContext(void)
{
	memset(&originalEndLocation, 0, sizeof(XLogRecPtr));
	memset(&currentEndLocation, 0, sizeof(XLogRecPtr));
	
	periodicLen = 0;
	periodicReportLen = 1024 * 1024;	// 1 megabyte.
	if (!Debug_print_qd_mirroring)
		periodicReportLen *= 10;		// 10 megabytes.
	
	memset(&periodicLocation, 0, sizeof(XLogRecPtr));

}

static void
WalSendServerDoRequest(WalSendRequest *walSendRequest, PROC_RESPONSE_COOKIE *responseCookie)
{
	WalSendResponse walSendResponse;
	bool successful;
	
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "request command %d = '%s' (mppLocalProcessSerial = %d, requestSerialNumber %d, responseNeeded %s)",
		 walSendRequest->command, WalSendRequestCommandToString(walSendRequest->command),
		 responseCookie->mppLocalProcessSerial,
		 responseCookie->requestSerialNumber,
		 (responseCookie->responseNeeded ? "true" : "false"));

	switch (walSendRequest->command)
	{
	case PositionToEnd:
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "PositionToEnd");

		successful = write_position_to_end(&originalEndLocation);
		if (successful)
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"end location %s",
				 XLogLocationToString(&originalEndLocation));
		else
		{
			elog(NOTICE,"disabling QD mirror (unable to connect to standby and determine log end location)");
			disableQDMirroring();
		}
		break;
		
	case Catchup:
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "Catchup");

        if (isQDMirroringCatchingUp())
    	{
			if (XLByteLT(originalEndLocation, walSendRequest->flushedLocation))
			{
				/*
				 * Standby master is behind the primary.  Send catchup WAL.
				 */
				 
				/* 
				 * Use a TRY block to catch errors from our attempt to read
				 * the primary's WAL.  Errors from sending to the standby
				 * come up as a boolean return (successful).
				 */
				PG_TRY();
				{
					successful = XLogCatchupQDMirror(&originalEndLocation, &walSendRequest->flushedLocation);
				}
				PG_CATCH();
				{
					/* 
					 * Report the error related to reading the primary's WAL
					 * to the server log 
					 */
					EmitErrorReport();
					FlushErrorState();

					elog(NOTICE,"Mirror cannot be synchronized because it is too far behind (unable to read old primary log during catchup)");
					markStandbyInvalid();
					successful = false;
				}
				PG_END_TRY();
				
				if (successful)
				{
					elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
						 "catchup send from standby end %s through primary flushed location %s",
						 XLogLocationToString(&originalEndLocation),
						 XLogLocationToString2(&walSendRequest->flushedLocation));
				}
			}
			else
			{
				elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"Mirror was already caught up");
				successful = true;
			}
			
			if (successful)
			{
				elog(LOG,"successfully copied transaction log from locations %s through %s to QD Mirror to synchronize it",
				     XLogLocationToString(&originalEndLocation),
				     XLogLocationToString2(&walSendRequest->flushedLocation));
				
				currentEndLocation = walSendRequest->flushedLocation;

				periodicLen = 0;
				periodicLocation = currentEndLocation;
			}
			else
			{
				elog(NOTICE,"disabling QD mirror (unable to synchronize at this time)");
				disableQDMirroring();
			}
		}
		else
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "QD Mirror Send: QD mirroring not catching-up");
		}
		
		break;
		
	case WriteWalPages:
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "WriteWalPages");
        if (isQDMirroringEnabled())
        {
			XLogRecPtr 	writeLoc;
			char 		cmd[MAXFNAMELEN + 50];
			char	   *from;
			Size		nbytes;

			XLogGetBuffer(walSendRequest->startidx, walSendRequest->npages,
				          &from, &nbytes);

			writeLoc.xlogid = walSendRequest->logId;
			writeLoc.xrecoff = walSendRequest->logSeg * XLogSegSize
				             + walSendRequest->logOff;
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1), 
				"QD Mirror Send: WriteWalPages startidx %d, npages %d, timeLineID %d, logId %u, logSeg %u, logOff 0x%X, write loc %X/%X, nbytes 0x%X",
				walSendRequest->startidx,
				walSendRequest->npages,
				walSendRequest->timeLineID,
				walSendRequest->logId,
				walSendRequest->logSeg,
				walSendRequest->logOff,
				writeLoc.xlogid,
				writeLoc.xrecoff,
				(int)nbytes);

			if (snprintf(cmd, sizeof(cmd),"xlog %d %d %d %d", 
						 walSendRequest->logId, walSendRequest->logSeg,
				         walSendRequest->logOff, (int)nbytes) >= sizeof(cmd))
				elog(ERROR,"could not create cmd for qd mirror logid %d seg %d", 
				     walSendRequest->logId, walSendRequest->logSeg);
			
			successful = write_qd_sync(cmd, from, nbytes);
			if (successful)
			{
				XLogRecPtr oldEndLocation;
				
				oldEndLocation = currentEndLocation;
				
				currentEndLocation.xlogid = walSendRequest->logId;
				currentEndLocation.xrecoff = walSendRequest->logSeg * XLogSegSize +
					                  walSendRequest->logOff;

				if (XLByteLT(oldEndLocation,currentEndLocation))
				{
					periodicLen += nbytes;
					if (periodicLen > periodicReportLen)
					{
						elog(LOG,
							 "periodic report: %d bytes successfully send to QD mirror for locations %s through %s",
							 periodicLen,
							 XLogLocationToString(&periodicLocation),
							 XLogLocationToString2(&currentEndLocation));

						periodicLen = 0;
						periodicLocation = currentEndLocation;
					}
				}
				else
				{
					elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
						 "Send to QD mirror successful.  New end location %s (old %s)",
						 XLogLocationToString(&currentEndLocation),
						 XLogLocationToString2(&oldEndLocation));
				}
			}
			else
			{
				elog(NOTICE,"disabling QD mirror (unable to send new log)");
				disableQDMirroring();
			}
			
			if (successful && walSendRequest->haveNewCheckpointLocation)
			{
				uint32 logid;
				uint32 seg;
				uint32 offset;
				
	        	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"New previous checkpoint location %s",
				     XLogLocationToString(&walSendRequest->newCheckpointLocation));
				XLByteToSeg(walSendRequest->newCheckpointLocation, logid, seg);
				offset = walSendRequest->newCheckpointLocation.xrecoff % XLogSegSize;
				
				if (snprintf(cmd, sizeof(cmd),"new_checkpoint_location %d %d %d", 
							 logid, seg, offset) >= sizeof(cmd))
					elog(ERROR,"could not create cmd for qd mirror logid %d seg %d offset %d", 
					     logid, seg, offset);
				
				successful = write_qd_sync(cmd, NULL, 0);
				if (successful)
				{
					elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"Send of new checkpoint location to QD mirror successful");
				}
				else
				{
					elog(NOTICE,"disabling QD mirror (unable to send new checkpoint location)");
					disableQDMirroring();
				}
			}
			
        }
		else
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "QD Mirror Send: QD mirroring not enabled");
		}
		
		break;
		
	default:
		elog(ERROR, "Unknown WalSendRequestCommand %d", walSendRequest->command);
	}


	walSendResponse.ok = true;
	
	ProcSendResponse(responseCookie, &walSendResponse, sizeof(walSendResponse));
}

void 
WalSendServerLoop(void)
{
	struct timeval timeout;
	PGPROCGetRequestResult getRequestResult;
	WalSendRequest walSendRequest;
	PROC_RESPONSE_COOKIE responseCookie;
	bool postmasterDied;
	bool drain;

	MemSet(&originalEndLocation, 0, sizeof(originalEndLocation));
	
	Assert(TopMemoryContext != NULL);
	MemoryContextSwitchTo(TopMemoryContext);
	Assert(CurrentMemoryContext == TopMemoryContext);

	/*
	 * Before we enter the main request loop, hang out here if the
	 * mirror configuration is unknown, there is no mirror, or we
	 * have disabled the QD mirror (and perhaps this server process has been
	 * restarted?).
	 */
	while (true)
	{
		if (isQDMirroringNotKnownYet())
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
				 "QD mirroring configuration not known yet");
		else if (isQDMirroringNotConfigured())
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
				 "QD mirroring not configured");
		else if (isQDMirroringUnexpectedError())
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
				 "QD mirroring unexpected error condition");
		else
			break;
		
		if (!PostmasterIsAlive(true))
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"postmaster has died.");
			return;
		}
		pg_usleep(1000000);		/* 1 sec */
	}
	
	/*
	 * Come up for air every 1 second to see if the postmaster has died, a
	 * shutdown has been requested, etc.
	 */
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	postmasterDied = false;
	drain = false;

	/* 
	 * Use a TRY block to catch unexpected errors that bubble up to this level
	 * and disable QD mirroring.
	 */
	PG_TRY();
	{
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1),
			 "entering main request loop");
		
//		ProcSetAcceptingNewRequests(PGPROC_REQUEST_WALSEND, true);

		WalSendServerInitContext();

		while(true)
		{
			getRequestResult = ProcGetRequest(
									PGPROC_REQUEST_WALSEND, 
									&walSendRequest, sizeof(walSendRequest),
									&timeout,
									drain,
									&responseCookie);
			
			if (!PostmasterIsAlive(true))
			{
				/*
				 * Gracefully drain our queue so requesting backend don't get hung
				 * even though the postmaster has croaked.
				 */
				elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"postmaster has died -- draining.");
				ProcSetAcceptingNewRequests(PGPROC_REQUEST_WALSEND, false);
				postmasterDied = true;
				drain = true;
			}

			if (getRequestResult == PGPROC_GETREQUEST_INTERRUPT)
			{
				CHECK_FOR_INTERRUPTS();
				
				if (walsend_shutdown_requested && !drain)
				{
					/*
					 * Gracefully drain our queue so requesting backends can finish
					 * on shutdown.
					 */
					elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"shutdown requested -- draining.");
					ProcSetAcceptingNewRequests(PGPROC_REQUEST_WALSEND, false);
					drain = true;
				}
				
				continue;
			}
			else if (getRequestResult == PGPROC_GETREQUEST_QUEUEEMPTY)
			{
				Assert(drain);
				break;
			}
			else
			{
				Assert(getRequestResult == PGPROC_GETREQUEST_OK);
			}

			WalSendServerDoRequest(&walSendRequest,&responseCookie);
		}

		disableQDMirroring();
	}
	PG_CATCH();
	{
		/* 
		 * Report the unexpected error.
		 */
		EmitErrorReport();
		FlushErrorState();

		elog(NOTICE,"disabling QD mirroring (unexpected error encountered)");
		unexpectedErrorQDMirroring();
	}
	PG_END_TRY();
		
	ProcSetAcceptingNewRequests(PGPROC_REQUEST_WALSEND, false);
	
	disconnectMirrorQD();
	
	return;
}

extern char* 
WalSendRequestCommandToString(WalSendRequestCommand command)
{
	switch (command)
	{
	case PositionToEnd:
		return "PositionToEnd";

	case Catchup:
		return "Catchup";

	case WriteWalPages:
		return "WriteWalPages";
		
	default:
		return "Unknown";
	}
}
