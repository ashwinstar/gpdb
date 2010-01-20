/**
 * Utility to contact a segment and issue a primary/mirror mode transition
 *
 * Copyright (c) 2009 Greenplum, Inc.
 *
 */

#include "postmaster/primary_mirror_mode.h"
#include <unistd.h>
#include "libpq/pqcomm.h"
#include "libpq/ip.h"

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

/**
 * gpmirrortransition takes a message on stdin (or from an input file if -i is specified) and
 *    transmits it to the given server as mirror transition message.  The caller must of course
 *    format the message properly.
 */

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

static inline bool
isEmpty(char *str)
{
	return str == NULL || str[0] == '\0';
}

/**
 * Send all dataLength of data to the target socket.
 *
 * Return false if the data cannot all be set, in which case SOCK_ERRNO will still
 *   be set to the error number from the socket call
 */
static bool
sendFully(int socket, char *data, int dataLength)
{
	while ( dataLength > 0)
	{
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
 */
static bool
sendMessage(struct addrinfo *addr, void *data, int dataLength)
{
#define MAX_RETRIES (1)
#define RETRY_SLEEP_TIME_SECONDS (1)

	int retry;
	int			save_errno = SOCK_ERRNO;
	int			tmpsock = -1;
	char		sebuf[256];
	bool result = true;

	struct
	{
		uint32		packetlen;
		PrimaryMirrorTransitionPacket payload;
	} trp;

	for ( retry = 0; retry >= 0; retry++ )
	{
		for ( ;; )
		{
			/*
			 * Open connection to one of the addresses
			 */
			if ((tmpsock = socket(addr->ai_family, SOCK_STREAM, 0)) >= 0)
			{
				/* got connection! */
				break;
			}

			if ( addr->ai_next == NULL )
			{
				fprintf(stderr, "gpmirrortransition -- socket() failed:\n" );
				goto sockErrReturn;
			}
			else addr = addr->ai_next;
		}

		/* loop around to retry interrupted system calls */
		for ( ;; )
		{
			int connectVal = connect(tmpsock, addr->ai_addr, addr->ai_addrlen);
			if ( connectVal >= 0 )
			{
				retry = -100; /* break out of outer loop */
				break;
			}
			
			if ( SOCK_ERRNO == ECONNREFUSED &&
					retry < MAX_RETRIES )
			{
				closesocket(tmpsock);
				tmpsock = -1;
				sleep(RETRY_SLEEP_TIME_SECONDS);
				break;
			}
			if (SOCK_ERRNO != EINTR)
			{
				fprintf(stderr, "gpmirrortransition -- connect() failed %d:\n", SOCK_ERRNO);
				goto sockErrReturn;
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

	if ( ! sendFully(tmpsock, (char *) &trp, sizeof(trp)))
	{
		fprintf(stderr, "gpmirrortransition -- sendMessage() failed:\n");
		goto sockErrReturn;
	}

	if ( ! sendFully(tmpsock, (char *) data, dataLength ))
	{
		fprintf(stderr, "gpmirrortransition -- sendMessage() failed:\n");
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
				fprintf(stderr, "Response from server is too large\n" );
				result = false;
				goto allDone;
			}
			numRead = recv(tmpsock, buf + offset, lengthLeft, 0 /* flags */);
			if (numRead < 0)
			{
				if (SOCK_ERRNO == EINTR)
				{
					/* Interrupted system call - we'll just try again */
				}
				goto sockErrReturn;
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

		{
			/* validate length of received message */
			int32 lengthFromMsg;
			
			if ( offset < 4)
			{
				fprintf(stderr, "gpmirrortransition -- sendMessage() did not receive enough data");
				goto errReturn;
			}

			lengthFromMsg = htonl(*(int32*)buf);
			if ( lengthFromMsg != offset )
			{
				fprintf(stderr, "gpmirrortransition -- sendMessage() received %d bytes but message said %d were sent", (int) offset, (int) lengthFromMsg );
				result = false;
				goto errReturn;
			}
		}

		buf[offset] = '\0';
        fprintf(stderr, "%s\n", data);

		/* Anything equal to success or STARTING with success: is considered success */
		if ( strcmp(data, "Success") != 0 &&
		     strncmp(data, "Success:", 8) != 0
		)
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
	fprintf(stderr, "%s\n", SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));

errReturn:
	result = false;
	goto allDone;
}

/**
 * *addrList will be filled in with the address(es) of the host/port when true is returned
 *
 * host/port may not be NULL
 */
static bool
determineTargetHost( struct addrinfo **addrList, char *host, char *port)
{
	struct addrinfo hint;
	int			ret;

	*addrList = NULL;

	/* Initialize hint structure */
	MemSet(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_family = AF_UNSPEC;

	/* Using pghost, so we have to look-up the hostname */
	hint.ai_family = AF_UNSPEC;

	/* Use pg_getaddrinfo_all() to resolve the address */
	ret = pg_getaddrinfo_all(host, port, &hint, addrList);
	if (ret || ! *addrList)
	{
		fprintf(stderr,"could not translate host name \"%s\" to address: %s\n", host, gai_strerror(ret));
		return false;
	}
	return true;
}

static char*
readFully(FILE *f, int *msgLenOut)
{
	int bufSize = 10;
	char *buf = malloc(bufSize * sizeof(char));
	int bufOffset = 0;

	if ( buf == NULL )
	{
		fprintf(stderr, "Out of memory\n");
		return NULL;
	}

	for ( ;; )
	{
		int numRead;

		errno = 0;
		numRead = fread(buf + bufOffset, sizeof(char), bufSize - bufOffset, f);
		if ( errno != 0 )
		{
			if ( feof(f))
				break;
			fprintf( stderr, "Error reading input.  Error code %d\n", errno);
			return NULL;
		}
		else if ( numRead <= 0 && feof(f))
			break;

		bufOffset += numRead;

		if ( bufOffset == bufSize )
		{
			// increase size!
			bufSize *= 2;
			buf = realloc(buf, bufSize * sizeof(char));
			if ( buf == NULL )
			{
				fprintf(stderr, "Out of memory\n");
				return NULL;
			}
		}
	}

	*msgLenOut = bufOffset;
	return buf;
}

int
main(int argc, char **argv)
{
	struct addrinfo *addrList = NULL;
	char *host = NULL, *port = NULL, *inputFile = NULL;
	char opt;
	char *msg;
	int msgLen = 0;

	while ((opt = getopt(argc, argv, "h:i:p:")) != -1)
	{
		switch (opt)
		{
			case 'h':
				host = optarg;
				break;
			case 'i':
				inputFile = optarg;
				break;
			case 'p':
				port = optarg;
				break;
		}
	}
	if ( host == NULL)
	{
		fprintf(stderr, "Missing host (-h) argument\n");
		return 1;
	}
	if ( port == NULL )
	{
		fprintf(stderr, "Missing port (-p) argument\n");
		return 1;
	}

	/* find the target machine */
	if ( ! determineTargetHost(&addrList, host, port))
	{
		return 1;
	}

	/* load the input message into memory */
	if ( inputFile == NULL)
	{
		msg = readFully(stdin, &msgLen);
	}
	else
	{
		FILE *f = fopen(inputFile, "r");
		if ( f == NULL)
		{
			fprintf(stderr, "Unable to open file %s\n", inputFile);
			return 1;
		}
		msg = readFully(f, &msgLen);
		fclose(f);
	}
	if ( msg == NULL )
		return 1; /* an error will have been logged by readFully */

	/* send the message */
	if (! sendMessage(addrList, msg, msgLen))
		return 1;

	return 0;
}
