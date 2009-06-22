/*-------------------------------------------------------------------------
 *
 * service.c
 *	  Shared code module that supports Greenplum system server processes
 *    under the postmaster that communicate with other Postgres processes
 *    with TCP/IP (same GP instance or different GP instance).
 *
 *
 * Copyright (c) 2007-2008, Greenplum inc
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

// TEMP BEGIN


#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#endif


 /*
  * backlog for listen() call:
  */
#define LISTEN_BACKLOG 64
#define getInt16(pNetData)	  ntohs(*((int16 *)(pNetData)))
#define getInt32(pNetData)	  ntohl(*((int32 *)(pNetData)))
#define getOid(pNetData)


/*=========================================================================
 * FUNCTIONS PROTOTYPES
 */


// UNDONE: Network types?
typedef struct ServiceConnectionRequestMsg
{

	char    eyeCatcher[SERVER_EYE_CATCHER_LEN];

}	ServiceConnectionRequestMsg;

typedef struct ServiceConnectionResponseMsg
{

	bool    ok;

}	ServiceConnectionResponseMsg;

static bool ServiceDoConnect(ServiceConfig *serviceConfig, int listenerPort, ServiceClient *serviceClient, bool complain);
static bool ServiceClientRead(ServiceClient *serviceClient, void* response, int responseLen, struct timeval *timeout);
static bool ServiceClientPollRead(ServiceClient *serviceClient, void* response, int responseLen, bool *pollResponseReceived);
static bool ServiceClientWrite(ServiceClient *serviceClient, void* request, int requestLen);
static void ServiceQuickDie(SIGNAL_ARGS);
static void ServiceListenLoop(ServiceCtrl *serviceCtrl);
static bool ServiceProcessRequest(ServiceCtrl *serviceCtrl, int sockfd, uint8 *inputBuff, bool newConnection);
static bool ServiceNewConnectionMsg(ServiceCtrl *serviceCtrl, int sockfd, ServiceConnectionRequestMsg *newConnectionRequestMsg);
static int ServiceListenerSetup(ServiceCtrl *serviceCtrl);

void
ServiceInit(ServiceConfig *serviceConfig, ServiceCtrl *serviceCtrl)
{
	Assert(serviceCtrl != NULL);
	Assert(serviceConfig != NULL);
	serviceCtrl->serviceConfig = serviceConfig;
	serviceCtrl->listenerFd = -1;
	serviceCtrl->listenerPort = -1;
}

// -----------------------------------------------------------------------------

/*
 * Kludge to prevent our error handling by raising and
 * catching ERROR from becoming a PANIC in critical sections...
 */
#define DECLARE_SAVE_SUPPRESS_PANIC()\
	int SaveSuppressPanic = false

#define SUPPRESS_PANIC()\
	{\
		SaveSuppressPanic = SuppressPanic;\
		SuppressPanic = true;\
	}

#define RESTORE_PANIC()\
	{\
		SuppressPanic = SaveSuppressPanic;\
	}

// -----------------------------------------------------------------------------

static char ClientErrorString[200] = "";

char *ServiceGetLastClientErrorString(void)
{
	return ClientErrorString;
}

static bool
ServiceDoConnect(ServiceConfig *serviceConfig, int listenerPort, ServiceClient *serviceClient, bool complain)
{
	int  n;
	struct sockaddr_in addr;
	int saved_err;
	char        *message;
	bool		result = false;
	DECLARE_SAVE_SUPPRESS_PANIC();

	PG_TRY();
	{
		SUPPRESS_PANIC();

		for (;;)
		{
			/*
			 * Open a connection to the service.
			 */
			serviceClient->sockfd = socket(AF_INET, SOCK_STREAM, 0);

			addr.sin_family = AF_INET;
			addr.sin_port = htons(listenerPort);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if ((n = connect(serviceClient->sockfd, (struct sockaddr *)&addr, sizeof(addr))) < 0)
			{
				saved_err = errno;

				close(serviceClient->sockfd);
				serviceClient->sockfd = -1;

				if (errno == EINTR)
					continue;

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								errmsg("Could not connect to '%s': %s",
									   serviceConfig->title,
									   strerror(saved_err))));
			}
			else
			{
				//success. we're done here!
				break;
			}
		}

		/* make socket non-blocking BEFORE we connect. */
		if (!pg_set_noblock(serviceClient->sockfd))
		{
			saved_err = errno;

			close(serviceClient->sockfd);
			serviceClient->sockfd = -1;
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("Could not set '%s' socket to non-blocking mode: %s",
								   serviceConfig->title,
								   strerror(saved_err))));
		}

		result = true;
		RESTORE_PANIC();
	}
	PG_CATCH();
	{
		RESTORE_PANIC();

		/* Report the error to the server log */
	    if (!elog_demote(WARNING))
    	{
    		elog(LOG,"unable to demote error");
        	PG_RE_THROW();
    	}

		message = elog_message();
 		if (message != NULL && strlen(message) + 1 < sizeof(ClientErrorString))
			strcpy(ClientErrorString, message);
		else
			strcpy(ClientErrorString, "");

		if (complain)
			EmitErrorReport();
		FlushErrorState();

		result = false;
	}
	PG_END_TRY();

	return result;
}

void
ServiceGetClientTimeout(ServiceConfig *serviceConfig, struct timeval *timeout)
{
	Assert(serviceConfig != NULL);

	if (serviceConfig->ServiceClientTimeout == NULL)
	{
		timeout->tv_sec = 0;
		timeout->tv_usec = 250000;	// 0.25 second.
	}
	else
		serviceConfig->ServiceClientTimeout(timeout);
}

/*
 * Used by clients to connect to a service.
 */
bool
ServiceClientConnect(ServiceConfig *serviceConfig, int listenerPort, ServiceClient *serviceClient, bool complain)
{
//	ServiceConnectionRequestMsg newConnectionRequestMsg;
//	ServiceConnectionResponseMsg newConnectionResponseMsg;
	bool connected;

	Assert(serviceConfig != NULL);
	Assert(serviceClient != NULL);

	if (serviceClient->sockfd != -1)
	{
		/*
		 * We are already connected.
		 */
		return true;
	}

	serviceClient->serviceConfig = serviceConfig;

	connected = ServiceDoConnect(serviceConfig, listenerPort, serviceClient, complain);

//	ServiceClientWrite(serviceClient, &newConnectionRequestMsg, sizeof(newConnectionRequestMsg));
//	ServiceClientRead(serviceClient, &newConnectionResponseMsg, sizeof(newConnectionResponseMsg));

	// UNDONE: Check result.

	return connected;
}



/*
 * Used by clients to send a request to a service.
 */
bool
ServiceClientSendRequest(ServiceClient *serviceClient, void* request, int requestLen)
{
	ServiceConfig *serviceConfig;
	char        *message;
	bool		result = false;
	DECLARE_SAVE_SUPPRESS_PANIC();

	Assert(serviceClient != NULL);
	Assert(request != NULL);

	PG_TRY();
	{
		SUPPRESS_PANIC();

		serviceConfig = serviceClient->serviceConfig;
		if (serviceConfig == NULL)
		{
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						    errmsg("Not connected to '%s'",
						           serviceConfig->title)));
		}
		if (requestLen != serviceClient->serviceConfig->requestLen)
		{
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("Expecting request length %d and actual length is %d for '%s'",
				 					serviceClient->serviceConfig->requestLen, requestLen,
				 					serviceConfig->title)));
		}

		result = ServiceClientWrite(serviceClient, request, requestLen);
		RESTORE_PANIC();
	}
	PG_CATCH();
	{
		RESTORE_PANIC();
		/* Report the error to the server log */
	    if (!elog_demote(WARNING))
    	{
    		elog(LOG,"unable to demote error");
        	PG_RE_THROW();
    	}

		message = elog_message();
 		if (message != NULL && strlen(message) + 1 < sizeof(ClientErrorString))
			strcpy(ClientErrorString, message);
		else
			strcpy(ClientErrorString, "");

		EmitErrorReport();
		FlushErrorState();

		result = false;
	}
	PG_END_TRY();

	return result;

}

static bool
ServiceClientWrite(ServiceClient *serviceClient, void* request, int requestLen)
{
	ServiceConfig *serviceConfig;
	int		n;
	int    bytesWritten = 0;
	int			saved_err;
	char        *message;
	bool		result = false;
	fd_set		wset;
	struct timeval rundownTimeout;
		// Use local variable since select modifies
		// the timeout parameter with remaining time.
	DECLARE_SAVE_SUPPRESS_PANIC();

	Assert(serviceClient != NULL);
	serviceConfig = (ServiceConfig*)serviceClient->serviceConfig;
	Assert(serviceConfig != NULL);
	ServiceGetClientTimeout(serviceConfig, &rundownTimeout);
	Assert(request != NULL);

	PG_TRY();
	{
		SUPPRESS_PANIC();

		if (serviceClient->sockfd == -1)
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						     errmsg("Lost connection to '%s'",
						            serviceConfig->title)));

		/*
		 * write the request
		 */
		while( bytesWritten < requestLen )
		{
			n = write(serviceClient->sockfd,
				      ((char *)request) + bytesWritten,
				      requestLen - bytesWritten);
			saved_err = errno;

			if (n == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									 errmsg("Connection to '%s' is closed",
							                serviceConfig->title)));
			}

			if (n < 0)
			{
				if (saved_err != EINTR && saved_err != EWOULDBLOCK)
				{
					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
									 errmsg("Write error to '%s': %s",
									        serviceConfig->title,
									        strerror(saved_err))));
				}

				if (saved_err == EWOULDBLOCK)
				{
					/* we shouldn't really get here since we are dealing with
					 * small messages, but once we've read a bit of data we
					 * need to finish out reading till we get the message (or error)
					 */
					do
					{
						FD_ZERO(&wset);
						FD_SET(serviceClient->sockfd, &wset);
						n = select(serviceClient->sockfd + 1, NULL, &wset, NULL, &rundownTimeout);
						if (n == 0)
						{
							struct timeval wholeTimeout;

							ServiceGetClientTimeout(serviceConfig, &wholeTimeout);

							ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
											 errmsg("Write to '%s' timed out after %d.%03d seconds)",
								 		            serviceConfig->title,
								                    (int)wholeTimeout.tv_sec,
						 		                    (int)wholeTimeout.tv_usec / 1000)));
						}
						else if (n < 0 && errno == EINTR)
							continue;
						else if (n < 0)
						{
							saved_err = errno;

							ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
											 errmsg("Write error to '%s': %s",
											        serviceConfig->title,
											        strerror(saved_err))));
						}
					}
					while (n < 1);
				}
				/* else saved_err == EINTR */

				continue;
			}

			bytesWritten += n;
		}

		result = true;
		RESTORE_PANIC();
	}
	PG_CATCH();
	{
		RESTORE_PANIC();

		/* Report the error to the server log */
	    if (!elog_demote(WARNING))
    	{
    		elog(LOG,"unable to demote error");
        	PG_RE_THROW();
    	}

		message = elog_message();
 		if (message != NULL && strlen(message) + 1 < sizeof(ClientErrorString))
			strcpy(ClientErrorString, message);
		else
			strcpy(ClientErrorString, "");

		EmitErrorReport();
		FlushErrorState();

		result = false;
	}
	PG_END_TRY();

	return result;
}

/*
 * Used by clients to receive a response from a service.
 */
bool
ServiceClientReceiveResponse(ServiceClient *serviceClient, void* response, int responseLen, struct timeval *timeout)
{
	ServiceConfig *serviceConfig;
	bool successful;

	Assert(serviceClient != NULL);
	serviceConfig = serviceClient->serviceConfig;
	Assert(serviceConfig != NULL);
	Assert(response != NULL);
	Assert(responseLen == serviceClient->serviceConfig->responseLen);

	successful = ServiceClientRead(serviceClient, response, responseLen, timeout);

	return successful;
}

static bool
ServiceClientRead(ServiceClient *serviceClient, void* response, int responseLen, struct timeval *timeout)
{
	ServiceConfig *serviceConfig;
	int		n;
	int	   bytesRead = 0;
	int			saved_err;
	char        *message;
	bool		result = false;
	fd_set		rset;
	struct timeval rundownTimeout = {0,0};
		// Use local variable since select modifies
		// the timeout parameter with remaining time.
	DECLARE_SAVE_SUPPRESS_PANIC();

	Assert(serviceClient != NULL);
	serviceConfig = serviceClient->serviceConfig;
	Assert(serviceConfig != NULL);
	Assert(response != NULL);
	if (timeout != NULL)
		rundownTimeout = *timeout;

	PG_TRY();
	{
		SUPPRESS_PANIC();

		/*
		 * read the response
		 */
		while (bytesRead < responseLen)
		{
			n = read(serviceClient->sockfd,
				     ((char *)response) + bytesRead,
				     responseLen - bytesRead);
			saved_err = errno;

			if (n == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								 errmsg("Connection to '%s' is closed",
								        serviceConfig->title)));
			}

			if (n < 0)
			{
				if (saved_err != EINTR && saved_err != EWOULDBLOCK)
				{
					ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								     errmsg("Read error from '%s': %s",
								            serviceConfig->title,
								            strerror(saved_err))));
				}

				if (saved_err == EWOULDBLOCK)
				{
					/* we shouldn't really get here since we are dealing with
					 * small messages, but once we've read a bit of data we
					 * need to finish out reading till we get the message (or error)
					 */
					do
					{
						FD_ZERO(&rset);
						FD_SET(serviceClient->sockfd, &rset);
						n = select(serviceClient->sockfd + 1, &rset, NULL, NULL, (timeout == NULL ? NULL : &rundownTimeout));
						if (n == 0)
						{
							if (timeout != NULL)
							{
								ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										          errmsg("Read from '%s' timed out after %d.%03d seconds",
												         serviceConfig->title,
												         (int)timeout->tv_sec,
												         (int)timeout->tv_usec / 1000)));
							}
						}
						else if (n < 0 && errno == EINTR)
							continue;
						else if (n < 0)
						{
							saved_err = errno;

							ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										     errmsg("Read error from '%s': %s",
										            serviceConfig->title,
										            strerror(saved_err))));
						}
					}
					while (n < 1);
				}
				/* else saved_err == EINTR */

				continue;
			}
			else
				bytesRead += n;
		}

		result = true;
		RESTORE_PANIC();
	}
	PG_CATCH();
	{
		RESTORE_PANIC();

		/* Report the error to the server log */
	    if (!elog_demote(WARNING))
    	{
    		elog(LOG,"unable to demote error");
        	PG_RE_THROW();
    	}

		message = elog_message();
 		if (message != NULL && strlen(message) + 1 < sizeof(ClientErrorString))
			strcpy(ClientErrorString, message);
		else
			strcpy(ClientErrorString, "");

		EmitErrorReport();
		FlushErrorState();

		result = false;
	}
	PG_END_TRY();

	return result;
}

/*
 * Used by clients to receive a response from a service.
 */
bool
ServiceClientPollResponse(ServiceClient *serviceClient, void* response, int responseLen, bool *pollResponseReceived)
{
	ServiceConfig *serviceConfig;
	bool successful;

	Assert(serviceClient != NULL);
	serviceConfig = serviceClient->serviceConfig;
	Assert(serviceConfig != NULL);
	Assert(response != NULL);
	Assert(responseLen == serviceClient->serviceConfig->responseLen);

	successful = ServiceClientPollRead(serviceClient, response, responseLen, pollResponseReceived);

	return successful;
}

static bool
ServiceClientPollRead(ServiceClient *serviceClient, void* response, int responseLen, bool *pollResponseReceived)
{
	ServiceConfig 	*serviceConfig;
	int				n;
	int				saved_err;
	char            *message;
	bool		    result = false;
	DECLARE_SAVE_SUPPRESS_PANIC();

	Assert(serviceClient != NULL);
	serviceConfig = serviceClient->serviceConfig;
	Assert(serviceConfig != NULL);
	Assert(response != NULL);

	PG_TRY();
	{
		SUPPRESS_PANIC();

		/*
		 * Attempt to read the response
		 */
		while (true)
		{
			n = read(serviceClient->sockfd,
				     ((char *)response),
				     responseLen);
			saved_err = errno;

			if (n == 0)
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								 errmsg("Connection to '%s' is closed",
								         serviceConfig->title)));
			}
			else if (n < 0)
			{
				if (saved_err == EWOULDBLOCK)
				{
					*pollResponseReceived = false;
					break;
				}

				if (saved_err == EINTR)
					continue;

				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								 errmsg("Read error from '%s': %s",
								        serviceConfig->title,
								        strerror(saved_err))));
			}

			if (n != responseLen)
			{
				ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								 errmsg("Expecting message length %d and actual read length was %d from '%s'",
					 			        responseLen, n,
					 			        serviceConfig->title)));
				return false;
			}

			*pollResponseReceived = true;
			break;
		}

		result = true;
		RESTORE_PANIC();
	}
	PG_CATCH();
	{
		RESTORE_PANIC();

		/* Report the error to the server log */
	    if (!elog_demote(WARNING))
    	{
    		elog(LOG,"unable to demote error");
        	PG_RE_THROW();
    	}

		message = elog_message();
 		if (message != NULL && strlen(message) + 1 < sizeof(ClientErrorString))
			strcpy(ClientErrorString, message);
		else
			strcpy(ClientErrorString, "");

		EmitErrorReport();
		FlushErrorState();

		result = false;
	}
	PG_END_TRY();

	return result;
}


#ifdef EXEC_BACKEND
/*
 * ServiceForkExec()
 *
 * Format up the arglist for the service process, then fork and exec.
 */
pid_t
ServiceForkExec(ServiceConfig *serviceConfig)
{
	char	   *av[10];
	int			ac = 0;
#define MAX_FORK_PARAMETER_LEN 25
	char		forkParameter[MAX_FORK_PARAMETER_LEN];
	int			len;

	len = sprintf(forkParameter, "--fork%s",serviceConfig->forkTitle);
	Assert(len > 0 && len < MAX_FORK_PARAMETER_LEN);

	av[ac++] = "postgres";
	av[ac++] = forkParameter;
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */

/*
 * ServiceQuickDie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
ServiceQuickDie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	ereport(LOG,
			(errmsg("terminating immediately")));

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
	exit(2);
}

/*
 * Shutdown signal from postmaster: abort transaction and exit
 * at soonest convenient time
 */
static void
ServiceDie(SIGNAL_ARGS)
{
	ereport(LOG,
			(errmsg("terminating")));

	die(postgres_signal_arg);
}

/*
 * Common service main.
 */
void
ServiceMain(ServiceCtrl *serviceCtrl)
{
	ServiceConfig *serviceConfig;

	sigjmp_buf	local_sigjmp_buf;

	Assert(serviceCtrl != NULL);
	serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;
	Assert(serviceConfig != NULL);

	IsUnderPostmaster = true;

	/* reset MyProcPid */
	MyProcPid = getpid();


	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/* Identify myself via ps */
	init_ps_display(serviceConfig->psTitle, "", "", "");

	if (serviceConfig->ServiceEarlyInit != NULL)
	{
		serviceConfig->ServiceEarlyInit();
	}
	else
	{
		SetProcessingMode(InitProcessing);
	}

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
	pqsignal(SIGTERM, ServiceDie);
	pqsignal(SIGQUIT, ServiceQuickDie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, serviceConfig->ServiceRequestShutdown);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	if (serviceConfig->ServicePostgresInit != NULL)
	{
		serviceConfig->ServicePostgresInit();
	}

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

	/* set up a listener port and put it in shmem*/
	serviceCtrl->listenerPort = ServiceListenerSetup(serviceCtrl);

	if (serviceConfig->ServiceInit != NULL)
	{
		serviceConfig->ServiceInit(serviceCtrl->listenerPort);
	}

	/* listen loop */
	ServiceListenLoop(serviceCtrl);

	proc_exit(0);
}

static void
ServiceListenLoop(ServiceCtrl *serviceCtrl)
{
	ServiceConfig *serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;

	uint8 		*inputBuff;
	int			n,
				highsock = 0,
				newsockfd;
	fd_set		rset,
				rrset;

	struct sockaddr_in addr;
	socklen_t	addrlen;

	List   *connectedSockets = NIL;
	ListCell   *cell;

	Assert(TopMemoryContext != NULL);
	MemoryContextSwitchTo(TopMemoryContext);
	Assert(CurrentMemoryContext == TopMemoryContext);

	/*
	 * Setup scratch buffer.
	 */
	inputBuff = palloc(serviceConfig->requestLen);

	FD_ZERO(&rset);
	FD_SET(serviceCtrl->listenerFd, &rset);
	highsock = serviceCtrl->listenerFd + 1;

	/* we'll handle many incoming sockets but keep the sockets in blocking
	 * mode since we are dealing with very small messages.
	 */
	while(true)
	{
		struct timeval shutdownTimeout = {1,0};		// 1 second.
			// Use local variable since select modifies
			// the timeout parameter with remaining time.

		CHECK_FOR_INTERRUPTS();

		if (serviceConfig->ServiceShutdownRequested())
		{
			if (serviceConfig->ServiceShutdown != NULL)
			{
				serviceConfig->ServiceShutdown();
			}
			break;
		}

		/* no need to live on if postmaster has died */
		if (!PostmasterIsAlive(true))
		{
			if (serviceConfig->ServicePostmasterDied != NULL)
			{
				serviceConfig->ServicePostmasterDied();
			}
			else
			{
				ereport(LOG,
						(errmsg("exiting because postmaster has died")));
				proc_exit(1);
			}
		}

		memcpy(&rrset, &rset, sizeof(fd_set));

		n = select(highsock + 1, &rrset, NULL, NULL, &shutdownTimeout);
		if (n == 0 || (n < 0 && errno == EINTR))
		{
			/* intr or timeout: Have we been here too long ? */
			continue;
		}

		if (n < 0)
		{
			/* this may be a little severe, but if we error on select()
			 * we'll just go ahead and blow up.  This will result in the
			 * postmaster re-spawning a new process.
			 */
			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("'%s': error during select() call (error:%d).",
			     			        serviceConfig->title, errno)));
			break;
		}

		/* is it someone tickling our listener port? */
		if (FD_ISSET(serviceCtrl->listenerFd, &rrset))
		{
            addrlen = sizeof(addr);
			if ((newsockfd = accept(serviceCtrl->listenerFd,
				                    (struct sockaddr *) & addr,
				                    &addrlen)) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 	 errmsg("'%s': error from client connection: %s)",
				     				    serviceConfig->title,
				     				    strerror(errno))));
			}

			/* make socket non-blocking BEFORE we connect. */
			if (!pg_set_noblock(newsockfd))
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 	 errmsg("'%s': could not set outbound socket to non-blocking mode: %s",
									    serviceConfig->title,
									    strerror(errno))));
			}

			if (newsockfd > highsock)
				highsock = newsockfd + 1;

			FD_SET(newsockfd, &rset);

			/*
			 * Read connection message.
			 */
			// UNDONE: temporarily turn off new connection flag...
			if( !ServiceProcessRequest(serviceCtrl, newsockfd, inputBuff, false))
			{
				/* close it down */
				FD_CLR( newsockfd, &rset);
				shutdown(newsockfd, SHUT_WR);
				close(newsockfd);
			}
			else
			{
				connectedSockets = lappend_int(connectedSockets, newsockfd);
			}

		}


		/* loop through all of our established sockets */
		cell = list_head(connectedSockets);
		while (cell != NULL)
		{
			int	fd = lfirst_int(cell);

			/* get the next cell ready before we delete */
			cell = lnext(cell);

			if (FD_ISSET(fd, &rrset))
			{
				if( !ServiceProcessRequest(serviceCtrl, fd, inputBuff, false))
				{
					/* close it down */
					FD_CLR( fd, &rset);
					connectedSockets = list_delete_int(connectedSockets, fd);
					shutdown(fd, SHUT_WR);
					close(fd);
				}
			}
		}
	}

	ereport(LOG,
			(errmsg("normal shutdown")));
	proc_exit(0);

}

/*
 * Used by ServiceListenLoop to process an incoming request.
 */
static bool
ServiceProcessRequest(ServiceCtrl *serviceCtrl, int sockfd, uint8 *inputBuff, bool newConnection)
{
	ServiceConfig *serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;

	ServiceConnectionRequestMsg newConnectionRequestMsg;

	int			saved_err;
	fd_set		rset;
	struct timeval rundownTimeout;
		// Use local variable since select modifies
		// the timeout parameter with remaining time.

	int n;
	int bytesRead = 0;
	uint8 *request = NULL;
	int reqlen;

	bool successful;

	ServiceGetClientTimeout(serviceConfig, &rundownTimeout);

	if (newConnection)
	{
		request = (uint8*)&newConnectionRequestMsg;
		reqlen = sizeof(newConnectionRequestMsg);
	}
	else
	{
		request = inputBuff;
		reqlen = serviceConfig->requestLen;
	}

	/*
	 * Read in the incoming request message.
	 */
	while (bytesRead < reqlen)
	{
		CHECK_FOR_INTERRUPTS();

		n = read(sockfd, request + bytesRead, reqlen - bytesRead);
		saved_err = errno;

		if (n == 0)
		{
//			elog(LOG, "'%s': client socket sockfd %d is closed",
//				 serviceConfig->title, sockfd);
			return false;
		}

		if (n < 0)
		{
			if (saved_err != EINTR && saved_err != EWOULDBLOCK)
			{
				ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
								  errmsg("'%s': read error from backend process: %s",
									     serviceConfig->title,
									     strerror(saved_err))));
				return false;
			}

			if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					CHECK_FOR_INTERRUPTS();

					if (serviceConfig->ServiceShutdownRequested())
						return false;

					FD_ZERO(&rset);
					FD_SET(sockfd, &rset);
					n = select(sockfd + 1, &rset, NULL, NULL, &rundownTimeout);
					if (n == 0)
					{
						struct timeval wholeTimeout;

						ServiceGetClientTimeout(serviceConfig, &wholeTimeout);

						ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										 errmsg("'%s': read from backend process timed out after %d.%03d seconds",
												serviceConfig->title,
												(int)wholeTimeout.tv_sec,
										 		(int)wholeTimeout.tv_usec / 1000)));
						return false;
					}
					if (n < 0 && errno == EINTR)
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
										 errmsg("'%s': read error from backend process: %s",
												serviceConfig->title,
												strerror(saved_err))));
						return false;
					}
				}
				while (n < 1);

			}
			/* else saved_err == EINTR */
			continue;
		}

		bytesRead += n;
	}

	if (newConnection)
		successful = ServiceNewConnectionMsg(serviceCtrl, sockfd, &newConnectionRequestMsg);
	else
		successful = serviceConfig->ServiceRequest(serviceCtrl, sockfd, request);

	return successful;
}

bool
ServiceProcessRespond(ServiceCtrl *serviceCtrl, int sockfd, uint8 *response, int responseLen)
{
	ServiceConfig *serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;

	int 		n;
	int			saved_err;
	fd_set		wset;

	int bytesWritten = 0;

	struct timeval rundownTimeout;
		// Use local variable since select modifies
		// the timeout parameter with remaining time.

	ServiceGetClientTimeout(serviceConfig, &rundownTimeout);

//	elog(LOG,"ServiceProcessRespond called for sockfd %d, responseLen %d",
//		 sockfd, responseLen);

	/*
	 * Write the response
	 */
	while (bytesWritten < responseLen )
	{

		CHECK_FOR_INTERRUPTS();

		n = write(sockfd,
			      response + bytesWritten,
			      responseLen - bytesWritten);
		saved_err = errno;

		if (n == 0)
		{
			ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("'%s': connection to backend process is closed",
							        serviceConfig->title)));
			return false;
		}

		if (n < 0)
		{
			if (saved_err != EINTR && saved_err != EWOULDBLOCK)
			{
				ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							     errmsg("'%s': write error to backend process: %s",
					  					serviceConfig->title,
					  					strerror(saved_err))));
				return false;
			}

			if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					CHECK_FOR_INTERRUPTS();

					FD_ZERO(&wset);
					FD_SET(sockfd, &wset);
					n = select(sockfd + 1, NULL, &wset, NULL, &rundownTimeout);
					if (n == 0)
					{
						struct timeval wholeTimeout;

						ServiceGetClientTimeout(serviceConfig, &wholeTimeout);

						ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							     		 errmsg("'%s': write to backend process timed out after %d.%03d seconds)",
												 serviceConfig->title,
												 (int)wholeTimeout.tv_sec,
										 		 (int)wholeTimeout.tv_usec / 1000)));
						return false;
					}
					if (n < 0 && errno == EINTR)
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						ereport(NOTICE, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							     		 errmsg("'%s': write error to backend process: %s)",
							 					serviceConfig->title,
							 					strerror(saved_err))));
						return false;
					}
				}
				while (n < 1);
			}
			/* else saved_err == EINTR */
			continue;
		}
		else
			bytesWritten += n;
	}

	return true;
}

static bool
ServiceNewConnectionMsg(ServiceCtrl *serviceCtrl, int sockfd, ServiceConnectionRequestMsg *newConnectionRequestMsg)
{
//	ServiceConfig *serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;
	ServiceConnectionResponseMsg	connectionResponseMsg;
	bool successful;

	// UNDONE: Check fields...

	successful = ServiceProcessRespond(serviceCtrl, sockfd, (uint8*)&connectionResponseMsg, sizeof(connectionResponseMsg));

	return successful;
}

/*
 * Sets up a listener on a system-picked port.  Returns the port that the
 * system port.
 */
static int
ServiceListenerSetup(ServiceCtrl *serviceCtrl)
{
	ServiceConfig *serviceConfig = (ServiceConfig*)serviceCtrl->serviceConfig;

	int listenerPort;
	struct sockaddr_in serv_addr;
	struct sockaddr_in addr;
	socklen_t	addrlen;


	if ((serviceCtrl->listenerFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("'%s' could not create listener socket: %s",
						       serviceConfig->title,
						       strerror(errno))));
	}

	memset(&addr, 0, sizeof(addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	serv_addr.sin_port = 0;		/* system picks port */

	/*
	 * we let the system pick the TCP port here so we don't have to
	 * manage port resources ourselves.
	 */
	if (bind(serviceCtrl->listenerFd, (struct sockaddr *) & serv_addr, sizeof(serv_addr)) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
					    errmsg("'%s' could not setup listener socket: %s",
					           serviceConfig->title,
					           strerror(errno)),
						       errdetail("error during bind() call (error:%d).", errno)));

	}

	if (listen(serviceCtrl->listenerFd, LISTEN_BACKLOG) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						errmsg("'%s' could not setup listener socket: %s",
						       serviceConfig->title,
						       strerror(errno)),
							   errdetail("error during listen() call (error:%d).", errno)));
	}


	addrlen = sizeof(struct sockaddr_in);

	if (getsockname(serviceCtrl->listenerFd, (struct sockaddr *) & addr, &addrlen) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_GP_INTERNAL_ERROR),
						errmsg("'%s' could not setup listener socket: %s",
						       serviceConfig->title,
						       strerror(errno)),
		   					   errdetail("error during getsockname() call (error:%d).", errno)));
	}


	/* display which port was chosen by the system. */
	listenerPort = ntohs(addr.sin_port);

	elog(DEBUG5, "'%s' listener on port: %d", serviceConfig->title, listenerPort);

	return listenerPort;
}
