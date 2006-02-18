/*-------------------------------------------------------------------------
 *
 * seqserver.c
 *	  Process under QD postmaster used for doling out sequence values to
 * 		QEs.
 *
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include <signal.h>
#include <sys/types.h>
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
#include "postmaster/seqserver.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/pmsignal.h"			/* PostmasterIsAlive */
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/ps_status.h"



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
 
 
#ifdef EXEC_BACKEND
static pid_t seqserver_forkexec(void);
#endif
NON_EXEC_STATIC void SeqServerMain(int argc, char *argv[]);

static void SeqServerLoop(void);
static bool processSequenceRequest( int sockfd );
static int listenerSetup(void);
static int64 readInt64(uint8 *buff);
static void sendInt64(uint8 *buff, int64 i);


/*=========================================================================
 * GLOBAL STATE VARIABLES
 */
/* Socket file descriptor for the listener. */
int			listenerFd;

/* our timeout value for select() and other socket operations. */
struct timeval tval;


static bool am_seqserver = false;
int outputBuffLength = sizeof(NextValResponse);
uint8 *inputBuff;
uint8 *outputBuff;


/*=========================================================================
 * VISIBLE FUNCTIONS
 */
 
int SeqServerShmemSize(void)
{
	
	/* just our one int4 */
	return sizeof(SeqServerControlBlock);
}


void SeqServerShmemInit(void)
{
	bool		found;
	
	
	/* Create or attach to the SharedSnapshot shared structure */
	seqServerCtl = (SeqServerControlBlock *) ShmemInitStruct("Sequence Server", SeqServerShmemSize(), &found);
	
}


/*
 * Main entry point for seqserver controller process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
seqserver_start(void)
{

	pid_t		SeqServerPID;

	
#ifdef EXEC_BACKEND
	switch ((SeqServerPID = seqserver_forkexec()))
#else
	switch ((SeqServerPID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork seqserver process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			SeqServerMain(0, NULL);
			break;
#endif
		default:
			return (int) SeqServerPID;
	}

	/* shouldn't get here */
	return 0;

}



/* 
 * Used by clients to seqeuence server to send a Sequence Request
 */
void
sendSequenceRequest(int sockfd, Oid dbid, Oid tablespaceid, Oid seq_oid, bool isTemp, 
					int16 qd_pid, int64   *plast, int64   *pcached,
					int64   *pincrement)
{	
	int		n;
	int    bytesWritten = 0;
	int	   bytesRead = 0;
	int			saved_err;
	fd_set		rset,wset;
	
	int16 netval_16;
	int32  netval_32;
	
	/* request message */
	int reqlen = sizeof(NextValRequest);
	uint8 request[reqlen];
	
	/* response message */
	int resplen = sizeof(NextValResponse);
	uint8 response[resplen];
	
	
	if (sockfd == -1)
		elog( ERROR, "Lost Connection to seqserver");
	
	
	/* 
	 * send the sequence request to the sequence server
	 */
	
	/* dbid */
	netval_32 = htons(dbid);
	memcpy(&request[0], &netval_32, 4);
	/* tablespace id */
	netval_32 = htons(tablespaceid);
	memcpy(&request[4], &netval_32, 4);
	/* seq oid */
	netval_32 = htons(seq_oid);
	memcpy(&request[8], &netval_32, 4);
	/* is temp */
	memcpy(&request[12], &isTemp, 2 );
	/* QD pid */
	netval_16 = htons(qd_pid);
	memcpy(&request[14], &netval_16, 2 );
	
	
	/*
	 * write the request
	 */	 
	while( bytesWritten < reqlen )
	{
		
		CHECK_FOR_INTERRUPTS();
					
		n = write(sockfd, &request[bytesWritten], reqlen - bytesWritten);
		saved_err = errno;
	
		if (n == 0)
		{
			elog(ERROR, "Error: seqserver socket closed.");
			return;
		}
		
		if (n < 0)
		{
			if (saved_err == EINTR)
				continue;
			else if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					struct timeval timeout = tval;
					
					CHECK_FOR_INTERRUPTS();
					
					FD_ZERO(&wset);
					FD_SET(sockfd, &wset);
					n = select(sockfd + 1, NULL, &wset, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
										errmsg("Error: Could not write()  message to seqserver: %s", 
										strerror(errno)),
										errdetail("error during write() call (error:%d) to seqserver on sockfd: %d ", 
													errno, sockfd)));
						return;
					}
				}
				while (n < 1);
				
			}
			else
			{
				ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
								errmsg("Error: Could not write()  message to seqserver: %s", 
								strerror(errno)),
								errdetail("error during write() call (error:%d) to seqserver on sockfd: %d ", 
											errno, sockfd)));
				return;							
			}
		}


		bytesWritten += n;
	}
	
	
	
	/* 
	 * read the response
	 */	
	while( bytesRead < resplen )
	{
		
		CHECK_FOR_INTERRUPTS();
			
		n = read(sockfd, &response[bytesRead], resplen - bytesRead);
		saved_err = errno;
		
		if (n == 0)
		{
			elog( ERROR, "seqserver socket is closed");
			return;
		}
		
		if (n < 0)
		{
			if (saved_err == EINTR)
				continue;
			else if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					struct timeval timeout = tval;
					
					CHECK_FOR_INTERRUPTS();
					
					FD_ZERO(&rset);
					FD_SET(sockfd, &rset);
					n = select(sockfd + 1, &rset, NULL, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
									    errmsg("Error: Could not read() message from seqserver"),
										errdetail("error during read() call (error:%d) from remote"										
										"sockfd = %d", errno, sockfd)));
						return;		
					}
				}
				while (n < 1);
				
			}
			else
			{
				elog( ERROR, "Error: Could not read() message from seqserver."
							  "(error:%d). Closing connection.", saved_err);
				return;
			}
		}
		else
			bytesRead += n;
	}
	

	
	*plast      = readInt64((uint8 *)&response[0]);
	*pcached    = readInt64(&response[8]);
	*pincrement = readInt64(&response[16]);

		
	elog( INFO, "JGE plast: %ld  pcached: %ld  pincrement: %ld", 
				*plast, *pcached, *pincrement );
				
}
 



/*=========================================================================
 * HELPER FUNCTIONS
 */
 

#ifdef EXEC_BACKEND
/*
 * seqserver_forkexec()
 *
 * Format up the arglist for the serqserver process, then fork and exec.
 */
static pid_t
seqserver_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "-forkseqserver";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */



/*
 * AutoVacMain
 */
NON_EXEC_STATIC void
SeqServerMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;
 
 
	IsUnderPostmaster = true;
	am_seqserver = true;
	
	/* reset MyProcPid */
	MyProcPid = getpid();
	
	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/* Identify myself via ps */
	init_ps_display("seqserver process", "", "");
	set_ps_display("");

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
	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/* Early initialization */
	BaseInit();

	InitProcess();
	
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
	seqServerCtl->seqServerPort = listenerSetup();
	
	/* 
	 * setup scratch buffers
	 */
	inputBuff = palloc( sizeof(NextValRequest) );	
	outputBuff = palloc( outputBuffLength );
	
	
	/* main loop */
	SeqServerLoop();
	

	/* One iteration done, go away */
	proc_exit(0);
}



void SeqServerLoop(void)
{
	int			n,
				highsock = 0,
				newsockfd;
	fd_set		rset,
				rrset;
	
	struct sockaddr_in addr;
	socklen_t	addrlen;
	
	List   *connectedSockets = NIL;
	ListCell   *cell;
	
	tval.tv_sec = 3;
	tval.tv_usec = 500000;
	
				
	FD_ZERO(&rset);
	FD_SET(listenerFd, &rset);
	highsock = listenerFd + 1;
	
	
	/* we'll handle many incoming sockets but keep the sockets in blocking
	 * mode since we are dealing with very small messages.
	 */
	while(true)
	{
		struct timeval timeout = tval;
		
		
		CHECK_FOR_INTERRUPTS();
		
		/* no need to live on if postmaster has died */
		if (!PostmasterIsAlive(true))
			exit(1);
		
		memcpy(&rrset, &rset, sizeof(fd_set));
		
		n = select(highsock + 1, &rrset, NULL, NULL, &timeout);
		if (n == 0)
		{
			/* timeout: Have we been here too long ? */			
			continue;
		}
		
		elog(DEBUG5, "seqserver select() returned %d ready sockets", n);

		/* is it someone tickling our listener port? */
		if (FD_ISSET(listenerFd, &rrset))
		{
			elog(DEBUG3, "seqserver: accepting an Incoming Connection ");
			if ((newsockfd = accept(listenerFd, (struct sockaddr *) & addr, &addrlen)) < 0)
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				elog(INFO, "seqserver: error during accept() call (error:%d)", errno);
			}
			
			/* make socket non-blocking BEFORE we connect. */
			if (!pg_set_noblock(newsockfd))
			{
				/*
				 * TODO: would be nice to read the errno and try and provide
				 * more useful info as to why this happened.
				 */
				elog(DEBUG3, "seqserver: Could not set outbound socket to non-blocking mode"
					 			"error during fcntl() call (error:%d) for sockfd: %d",
							   errno, newsockfd);
			}
			
			if (newsockfd > highsock)
				highsock = newsockfd + 1;
			
			FD_SET(newsockfd, &rset);
			
			connectedSockets = lappend_int(connectedSockets, newsockfd);
						
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
				if( !processSequenceRequest(fd))
				{
					/* close it down */	
					FD_CLR( fd, &rset);
					connectedSockets = list_delete_int(connectedSockets, fd);
					shutdown(fd, SHUT_WR);
					close(fd);
				}							
			}	
		}
		
		
	} /* end server loop */
	
}


/* 
 * Used by sequenceServer to process incoming Sequence Requests
 */
static bool
processSequenceRequest( int sockfd )
{
	NextValRequest nextValRequest;
	int			saved_err;
	fd_set		rset,wset;
	int64 plast;
	int64 pcached;
	int64 pincrement;
	
	int n;
	int bytesRead = 0;
	int bytesWritten = 0;
	int reqlen = sizeof(NextValRequest);
	
	
	/* Read in the incoming request message */
	while( bytesRead < reqlen )
	{
		
		CHECK_FOR_INTERRUPTS();
			
		n = read(sockfd, inputBuff + bytesRead, reqlen - bytesRead);
		saved_err = errno;
		
		if (n == 0)
		{
			elog( DEBUG3, "client socket is closed");
			return false;
		}
		
		if (n < 0)
		{
			if (saved_err == EINTR)
				continue;
			else if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					struct timeval timeout = tval;
					
					CHECK_FOR_INTERRUPTS();
					
					FD_ZERO(&rset);
					FD_SET(sockfd, &rset);
					n = select(sockfd + 1, &rset, NULL, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						elog( DEBUG3, "error reading incoming message from client. (errno: %d)", saved_err);
						return false;
					}
				}
				while (n < 1);
				
			}
			else
			{
				elog( DEBUG3, "Error: Could not read() message from client."
							  "(error:%d). Closing connection.", saved_err);
				return false;
			}
		}


		bytesRead += n;
	}
	
	
	/* 
	 * process what we've read 
	 */
	nextValRequest.dbid    	    = getInt32(inputBuff);
	nextValRequest.tablespaceid = getInt32(inputBuff+4);
	nextValRequest.seq_oid      = getInt32(inputBuff+8);
	memcpy( &nextValRequest.isTemp, inputBuff+12, 2 );
	nextValRequest.qd_pid       = getInt16(inputBuff+14);
	
	
	elog( DEBUG5, "Received nextval request for dbid: %ld tablespaceid: %ld seqoid: "
				  "%ld isTemp: %s qidPid: %d", 
				  (long int)nextValRequest.dbid, 
				  (long int)nextValRequest.tablespaceid,
				  (long int)nextValRequest.seq_oid, 
				  nextValRequest.isTemp?"true":"false", 
				  nextValRequest.qd_pid);
	
	
	/*
	 * Process request
	 */
	cdb_sequence_nextval_server(nextValRequest.tablespaceid,
    	                        nextValRequest.dbid,
        	                    nextValRequest.seq_oid,
            	                nextValRequest.isTemp,
                	            &plast,
                    	        &pcached,
                        	    &pincrement);
	 
	 
	/*
	 * Send the response
	 */ 
	sendInt64(outputBuff, plast);
	sendInt64(outputBuff+8, pcached);
	sendInt64(outputBuff+16, pincrement);

	

	/*
	 * Write the response
	 */
	while( bytesWritten < outputBuffLength )
	{
		
		CHECK_FOR_INTERRUPTS();
					
		n = write(sockfd, outputBuff + bytesWritten, outputBuffLength - bytesWritten);
		saved_err = errno;
	
		if (n == 0)
		{
			elog( DEBUG3, "client socket is closed");
			return false;
		}
		
		if (n < 0)
		{
			if (saved_err == EINTR)
				continue;
			else if (saved_err == EWOULDBLOCK)
			{
				/* we shouldn't really get here since we are dealing with
				 * small messages, but once we've read a bit of data we
				 * need to finish out reading till we get the message (or error)
				 */
				do
				{
					struct timeval timeout = tval;
					
					CHECK_FOR_INTERRUPTS();
					
					FD_ZERO(&wset);
					FD_SET(sockfd, &wset);
					n = select(sockfd + 1, NULL, &wset, NULL, &timeout);
					if (n == 0 || (n < 0 && errno == EINTR))
						continue;
					else if (n < 0)
					{
						saved_err = errno;

						elog( DEBUG3, "error writing incoming message from client. (errno: %d)", saved_err);
						return false;
					}
				}
				while (n < 1);
				
			}
			else
			{
				elog( DEBUG3, "Error: Could not write() message from client."
							  "(error:%d). Closing connection.", sockfd);
				return false;			  
			}
		}
		else
			bytesWritten += n;
	}
	
	return true;
}




/* listenerSetup
 *
 * Sets up a listener on a system-picked port.  Returns the port that the 
 * system port.  
 */
static int
listenerSetup(void)
{
	int listenerPort;
	struct sockaddr_in serv_addr;
	struct sockaddr_in addr;
	socklen_t	addrlen;

	
	if ((listenerFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
						errmsg("SeqServer Error: Could not setup listener socket: %s", strerror(errno)),
				errdetail("error during socket() call (error:%d).", errno)));
	}

	memset(&addr, 0, sizeof(addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	
	serv_addr.sin_port = 0;		/* system picks port */

	/*
	 * we let the system pick the TCP port here so we don't have to
	 * manage port resources ourselves.
	 */
	if (bind(listenerFd, (struct sockaddr *) & serv_addr, sizeof(serv_addr)) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
			   errmsg("SeqServer Error: Could not setup listener socket"),
				  errdetail("error during bind() call (error:%d).", errno)));
		
	}

	if (listen(listenerFd, LISTEN_BACKLOG) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
						errmsg("SeqServer Error: Could not setup listener socket: %s", strerror(errno)),
				errdetail("error during listen() call (error:%d).", errno)));
	}


	addrlen = sizeof(struct sockaddr_in);

	if (getsockname(listenerFd, (struct sockaddr *) & addr, &addrlen) < 0)
	{
		/*
		 * TODO: would be nice to read the errno and try and provide
		 * more useful info as to why this happened.
		 */
		ereport(ERROR, (errcode(ERRCODE_MPP_INTERNAL_ERROR),
						errmsg("SeqServer Error: Could not setup listener socket: %s", strerror(errno)),
		   errdetail("error during getsockname() call (error:%d).", errno)));
	}


	/* display which port was chosen by the system. */
	listenerPort = ntohs(addr.sin_port);

	elog(DEBUG1, "SeqServer listener on port: %d", listenerPort);

	return listenerPort;
}



int64 readInt64( uint8 *buff )
{
	int64		result;

	memcpy(&result, buff, 8 );	
		
	return result;
}


void sendInt64(uint8 *buff, int64 i)
{
	memcpy( buff, &i, 8);
}




