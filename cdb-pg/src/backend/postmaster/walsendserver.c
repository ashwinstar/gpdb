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
#include <pthread.h>
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
#include "postmaster/service.h"
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


static void WalSendServerInitContext(void);
static void WalSendServerDoRequest(WalSendRequest *walSendRequest);

/*
 * The following are standard procedure signatures needed by
 * the Service shared library.
 */
static void WalSendServer_ServiceRequestShutdown(SIGNAL_ARGS);
static void WalSendServer_ServicePostgresInit(void);
static void WalSendServer_ServiceInit(int listenerPort);
static bool WalSendServer_ServiceShutdownRequested(void);
static bool WalSendServer_ServiceRequest(ServiceCtrl *serviceCtrl, int sockfd, uint8 *request);



/*=========================================================================
 * GLOBAL STATE VARIABLES
 */

static bool am_walsendserver = false;

static ServiceConfig WalSendServer_ServiceConfig =
	{"WALSND",
	"WAL Send Server", "WAL Send Server process", "walsendserver",
	sizeof(WalSendRequest), sizeof(WalSendResponse),
	{1,0},		// shutdownTimeout = 1 second.
	{0,250000},	// readClientTimeout = 0.25 second.
	WalSendServer_ServiceRequestShutdown,
	WalSendServer_ServicePostgresInit,
	WalSendServer_ServiceInit,
	WalSendServer_ServiceShutdownRequested,
	WalSendServer_ServiceRequest};

static ServiceCtrl WalSendServer_ServiceCtrl;


/*=========================================================================
 * VISIBLE FUNCTIONS
 */

typedef struct WalSendServerShmem
{
	int	listenerPort;
	
} WalSendServerShmem;

static WalSendServerShmem *WalSendServerShared = NULL;

int WalSendServerShmemSize(void)
{
	return MAXALIGN(sizeof(WalSendServerShmem));
}


void WalSendServerShmemInit(void)
{
	bool		found;
	
	
	/* Create or attach to the SharedSnapshot shared structure */
	WalSendServerShared = (WalSendServerShmem *) ShmemInitStruct("WAL Send Server", WalSendServerShmemSize(), &found);
	if (!WalSendServerShared)
		elog(FATAL, "could not initialize WAL Send server shared memory");
	
	if (!found)
		WalSendServerShared->listenerPort = -1;
}

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

			ServiceInit(&WalSendServer_ServiceConfig, &WalSendServer_ServiceCtrl);
			ServiceMain(&WalSendServer_ServiceCtrl);
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
WalSendServer_ServiceRequestShutdown(SIGNAL_ARGS)
{
	walsend_shutdown_requested = true;
}

static void
WalSendServer_ServicePostgresInit(void)
{
	/* See InitPostgres()... */
    InitProcess();	
	InitBufferPoolBackend();
	InitXLOGAccess();

}

static void
WalSendServer_ServiceInit(int listenerPort)
{
	elog(LOG,"listenerPort %d",listenerPort);

	if (WalSendServerShared == NULL)
		elog(FATAL,"WAL Send server shared memory not initialized");
	
	WalSendServerShared->listenerPort = listenerPort;
	
	am_walsendserver = true;

	WalSendServerInitContext();
}

static bool
WalSendServer_ServiceShutdownRequested(void)
{
	if (walsend_shutdown_requested)
		elog(LOG,"shutdown requested is true");
	return walsend_shutdown_requested;
}

/*
 * Process local static that contains the connection to the WAL send server.
 */
static ServiceClient WalSendServerClient = {NULL,-1};

bool
WalSendServerClientConnect(void)
{
	bool connected;
	
	if (WalSendServerShared == NULL)
		elog(FATAL,"WAL Send server shared memory not initialized");

	connected = ServiceClientConnect(&WalSendServer_ServiceConfig, 
									WalSendServerShared->listenerPort,
									&WalSendServerClient);

	return connected;
}


bool
WalSendServerClientSendRequest(WalSendRequest* walSendRequest)
{
	bool successful;
	
	successful = ServiceClientSendRequest(&WalSendServerClient, walSendRequest, sizeof(WalSendRequest));

	return successful;
}

bool
WalSendServerClientReceiveResponse(WalSendResponse* walSendResponse, struct timeval *timeout)
{
	bool successful;
	
	successful = ServiceClientReceiveResponse(&WalSendServerClient, walSendResponse, sizeof(WalSendResponse), timeout);

	return successful;
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
	periodicReportLen = 10 * 1024 * 1024;	// 10 megabytes.
	if (!Debug_print_qd_mirroring)
		periodicReportLen *= 10;		// 100 megabytes.
	
	memset(&periodicLocation, 0, sizeof(XLogRecPtr));

}

static void
WalSendServerDoRequest(WalSendRequest *walSendRequest)
{
	bool successful;
	
	switch (walSendRequest->command)
	{
	case PositionToEnd:
		elog((Debug_print_qd_mirroring ? LOG : DEBUG5), "PositionToEnd");

		successful = write_position_to_end(&originalEndLocation);
		if (successful)
			elog(LOG,"QD Mirror returned end location %s",
				 XLogLocationToString(&originalEndLocation));
		else
		{
			elog(NOTICE,"disabling QD mirror (unable to connect to standby and determine log end location)");
			disableQDMirroring();
		}
		break;
		
	case Catchup:
		elog((Debug_print_qd_mirroring ? LOG : DEBUG5), "Catchup");

        if (isQDMirroringCatchingUp())
    	{
    		elog(LOG,"QD Primary is flushed through location %s",
				 XLogLocationToString(&walSendRequest->flushedLocation));
			
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
					elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
						 "catchup send from standby end %s through primary flushed location %s",
						 XLogLocationToString(&originalEndLocation),
						 XLogLocationToString2(&walSendRequest->flushedLocation));
				}
			}
			else
			{
				elog((Debug_print_qd_mirroring ? LOG : DEBUG5),"Mirror was already caught up");
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
		else if (isQDMirroringDisabled())
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), "QD Mirror Send: QD mirroring not catching-up (state is disabled)");
		}
		else
		{
			elog(ERROR,"unexpected QD mirroring state %s",
				 QDMirroringStateString());
		}
		
		break;
		
	case WriteWalPages:
		if (Debug_print_qd_mirroring)
			elog(LOG, "WriteWalPages");
		
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
			if (Debug_print_qd_mirroring)
				elog(LOG,
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
					if (Debug_print_qd_mirroring)
						elog(LOG,
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
				
	        	elog((Debug_print_qd_mirroring ? LOG : DEBUG5),"New previous checkpoint location %s",
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
					elog((Debug_print_qd_mirroring ? LOG : DEBUG5),"Send of new checkpoint location to QD mirror successful");
				}
				else
				{
					elog(NOTICE,"disabling QD mirror (unable to send new checkpoint location)");
					disableQDMirroring();
				}
			}
			
        }
		else if (isQDMirroringDisabled())
		{
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), "QD Mirror Send: QD mirroring not enabled");
		}
		else
		{
			elog(ERROR,"unexpected QD mirroring state %s",
				 QDMirroringStateString());
		}
		
		break;
		
	default:
		elog(ERROR, "Unknown WalSendRequestCommand %d", walSendRequest->command);
	}

}

static bool WalSendServer_ServiceRequest(ServiceCtrl *serviceCtrl, int sockfd, uint8 *request)
{
	WalSendRequest *walSendRequest = (WalSendRequest*)request;
	WalSendResponse walSendResponse;
	bool result = false;

	//*** Missing: On normal shutdown, disable and disconnect.
	
	/* 
	 * Use a TRY block to catch unexpected errors that bubble up to this level
	 * and disable QD mirroring.
	 */
	PG_TRY();
	{
		if (Debug_print_qd_mirroring)
			elog(LOG, "request command %d = '%s'",
			     walSendRequest->command, 
			     WalSendRequestCommandToString(walSendRequest->command));

		WalSendServerDoRequest(walSendRequest);

		/*
		 * Currently, all requests need a response.
		 */
		walSendResponse.ok = true;

		result = ServiceProcessRespond(serviceCtrl, sockfd, (uint8*)&walSendResponse, sizeof(walSendResponse));
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

		result = false;
	}
	PG_END_TRY();

	return result;
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
