/**
 * This code is used for clients that need to connect to the postmaster to send a so-called "transition" message
 * Transition messages are special in that they do not require a full database backend to be executed; so they
 *    can run on a mirror segment that does not have a running database.
 */
#include "postmaster/primary_mirror_mode.h"
#include "postmaster/primary_mirror_transition_client.h"
#include <unistd.h>
#include "libpq/pqcomm.h"
#include "libpq/ip.h"

 /*
  * These macros are needed to let error-handling code be portable between
  * Unix and Windows.  (ugh)
  *
  * macros taken from gp-libpq-int.h
  */
 #ifdef WIN32
 #define SOCK_ERRNO (WSAGetLastError())
 #define SOCK_STRERROR winsock_strerror
 #define SOCK_ERRNO_SET(e) WSASetLastError(e)
 #else
 #define SOCK_ERRNO errno
 #define SOCK_STRERROR pqStrerror
 #define SOCK_ERRNO_SET(e) (errno = (e))
 #endif

/**
 * Send all dataLength of data to the target socket.
 *
 * Return false if the data cannot all be set, in which case SOCK_ERRNO will still
 *   be set to the error number from the socket call
 */
static bool
sendFully(PrimaryMirrorTransitionClientInfo *client, int socket, char *data, int dataLength)
{
	while ( dataLength > 0)
	{
		if (client->checkForNeedToExitFn())
			return false;

		int numSent = send(socket, data, dataLength, 0);
		if ( numSent < 0)
		{
			if (SOCK_ERRNO != EINTR)
				return false;
 			/* else .. Interrupted system call - we'll just try again */
		}
		else
		{
			data += numSent;
			dataLength -= numSent;
		}
	}
	return true;
}
 
 /**
  * Send the given data to the given address(es).
  *
  * addr may be a chain of addresses
  *
  * if data is NULL then we don't send any data...we just close immediately
  *
  * @return the error code
  */
int
sendTransitionMessage(PrimaryMirrorTransitionClientInfo *client, struct addrinfo *addr, void *data, int dataLength,
			int maxRetries, int retrySleepTimeSeconds)
{

#define CHECK_FOR_EXIT_REQUEST do { if (client->checkForNeedToExitFn()) goto exitRequestedByClient; } while (0)

 	int retry;
 	int save_errno = SOCK_ERRNO;
 	int tmpsock = -1;
 	char sebuf[256];
 	int result = TRANS_ERRCODE_SUCCESS;
 	char errMessagePrefix[10000];
 	char errMessage[10000];

 	struct
 	{
 		uint32		packetlen;
 		PrimaryMirrorTransitionPacket payload;
 	} trp;

 	errMessagePrefix[0] = '\0';
 	errMessage[0] = '\0';

 	for ( retry = 0; retry >= 0; retry++ )
 	{
 		for ( ;; )
 		{
 			/*
 			 * Open connection to one of the addresses
 			 */
			CHECK_FOR_EXIT_REQUEST;
 			if ((tmpsock = socket(addr->ai_family, SOCK_STREAM, 0)) >= 0)
 			{
 				/* got connection! */
 				break;
 			}

 			if ( addr->ai_next == NULL )
 			{
 				snprintf(errMessagePrefix, sizeof(errMessagePrefix), "failed to create socket: " );
 				goto sockErrReturn;
 			}
 			else addr = addr->ai_next;
 		}

 		/* loop around to retry interrupted system calls */
 		for ( ;; )
 		{
 			CHECK_FOR_EXIT_REQUEST;
 			int connectVal = connect(tmpsock, addr->ai_addr, addr->ai_addrlen);
 			if ( connectVal >= 0 )
 			{
 				retry = -100; /* success -- break out of outer loop */
 				break;
 			}

 			if ( SOCK_ERRNO == ECONNREFUSED &&
 					retry < maxRetries )
 			{
				/* connection refused and we have retries left -- server may not be up yet, so try again later */
				closesocket(tmpsock);
				tmpsock = -1;
				sleep(retrySleepTimeSeconds);
				break;
 			}

 			if (SOCK_ERRNO != EINTR)
 			{
				/* a real error -- see if there is another address to try.  If so, break out
				 *   to try it.  Otherwise, error! */
				addr = addr->ai_next;

				if ( addr == NULL)
				{
					/* error */
					snprintf(errMessagePrefix, sizeof(errMessagePrefix), "failed to connect: ");
					goto sockErrReturn;
				 }
				 else
				 {
					 retry--; /* don't lose a retry attempt because of trying a different address */
					 break;
				 }
 			}
 			/* else ... interrupted system call - we'll just try again */
 		}
 	}

 	/*
 	 * We needn't set nonblocking I/O or NODELAY options here.
 	 */

 	/* Create and send the transition request packet. */
 	trp.packetlen = htonl((uint32) sizeof(trp));
 	trp.payload.protocolCode = (MsgType) htonl( PRIMARY_MIRROR_TRANSITION_REQUEST_CODE );
 	trp.payload.dataLength = htonl(dataLength);

 	if ( ! sendFully(client, tmpsock, (char *) &trp, sizeof(trp)))
 	{
 		CHECK_FOR_EXIT_REQUEST; /* since sendFully may have returned because of a cancel request, check again here */
 		snprintf(errMessagePrefix, sizeof(errMessagePrefix), "failed sending message: ");
 		goto sockErrReturn;
 	}

 	if ( ! sendFully(client, tmpsock, (char *) data, dataLength ))
 	{
 		CHECK_FOR_EXIT_REQUEST; /* since sendFully may have returned because of a cancel request, check again here */
 		snprintf(errMessagePrefix, sizeof(errMessagePrefix), "failed sending message: ");
 		goto sockErrReturn;
 	}

 	/*
 	 * Wait for the postmaster to send the message indicating results
 	 */
 	{
 		char buf[10000];
 		char *data = buf+4; /* skip the 4 length bytes in the output data */
 		int offset = 0;
 		int lengthLeft = sizeof(buf) - 1; /* leave room for trailing '\0 */

 		*data = '\0';
 		for ( ;;)
 		{
 			int numRead;

 			if ( lengthLeft < 1 )
 			{
 				snprintf(errMessagePrefix, sizeof(errMessagePrefix), "failed: response from server is too large\n" );
 				result = TRANS_ERRCODE_ERROR_PROTOCOL_VIOLATED;
 				goto allDone;
 			}
 			CHECK_FOR_EXIT_REQUEST;
 			numRead = recv(tmpsock, buf + offset, lengthLeft, 0 /* flags */);
 			if (numRead < 0)
 			{
 				if (SOCK_ERRNO == EINTR)
 				{
 					/* Interrupted system call - we'll just try again */
 				}
 				else
 				{
 					goto sockErrReturn;
 				}
 			}
 			else if ( numRead == 0 )
 			{
 				break;
 			}
 			else
 			{
 				lengthLeft -= numRead;
 				offset += numRead;
 			}
 		}

		/* validate length of received message */
		int32 lengthFromMsg;

		if ( offset < 4)
		{
			snprintf(errMessage, sizeof(errMessage), "failed: server did not respond with enough data");
			result = TRANS_ERRCODE_ERROR_SERVER_DID_NOT_RETURN_DATA;
			goto errReturn;
		}

		lengthFromMsg = htonl(*(int32*)buf);
		if ( lengthFromMsg != offset )
		{
			snprintf(errMessage, sizeof(errMessage),
				"failed: received %d bytes but server message said %d were sent",
				(int) offset, (int) lengthFromMsg );
			result = TRANS_ERRCODE_ERROR_PROTOCOL_VIOLATED;
			goto errReturn;
		}

 		buf[offset] = '\0';
 		client->receivedDataCallbackFn(data);

 		/* Anything equal to success or STARTING with success: is considered success */
 		if ( strcmp(data, "Success") != 0 &&
		 	strncmp(data, "Success:", 8) != 0 )
 		{
 			goto errReturn;
 		}
 	}

 allDone:
 	/* All done */
 	if ( tmpsock >= 0)
 		closesocket(tmpsock);
 	SOCK_ERRNO_SET(save_errno);
 	return result;

 sockErrReturn:
 	snprintf(errMessage, sizeof(errMessage), "%s%s\n", errMessagePrefix, SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
 	if ( result == TRANS_ERRCODE_SUCCESS )
		result = TRANS_ERRCODE_ERROR_SOCKET;
	goto errReturn;

 exitRequestedByClient:
 	snprintf(errMessage, sizeof(errMessage), "failure: interrupted\n");
 	result = TRANS_ERRCODE_INTERRUPT_REQUESTED;
 	goto errReturn;

 errReturn:
	client->errorLogFn(errMessage);
	if ( result == TRANS_ERRCODE_SUCCESS )
		result = TRANS_ERRCODE_ERROR_UNSPECIFIED;
 	goto allDone;
}
