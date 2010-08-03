/**
 * Utility to contact a segment and issue a primary/mirror mode transition
 *
 * Copyright (c) 2009 Greenplum, Inc.
 *
 */

#include "postmaster/primary_mirror_mode.h"
#include "postmaster/primary_mirror_transition_client.h"
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

static inline bool
isEmpty(char *str)
{
	return str == NULL || str[0] == '\0';
}

static bool
gpCheckForNeedToExitFn(void)
{
	return false;
}

static void
gpMirrorErrorLogFunction(char *str)
{
	fprintf(stderr, "%s\n", str);
}

static void
gpMirrorReceivedDataCallbackFunction(char *buf)
{
	fprintf(stderr, "%s\n", buf);
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
		return TRANS_ERRCODE_ERROR_INVALID_ARGUMENT;
	}
	if ( port == NULL )
	{
		fprintf(stderr, "Missing port (-p) argument\n");
		return TRANS_ERRCODE_ERROR_INVALID_ARGUMENT;
	}

	/* find the target machine */
	if ( ! determineTargetHost(&addrList, host, port))
	{
		return TRANS_ERRCODE_ERROR_HOST_LOOKUP_FAILED;
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
			return TRANS_ERRCODE_ERROR_READING_INPUT;
		}
		msg = readFully(f, &msgLen);
		fclose(f);
	}
	if ( msg == NULL )
		return TRANS_ERRCODE_ERROR_READING_INPUT; /* an error will have been logged by readFully */

	/* send the message */
	PrimaryMirrorTransitionClientInfo client;
	client.receivedDataCallbackFn = gpMirrorReceivedDataCallbackFunction;
	client.errorLogFn = gpMirrorErrorLogFunction;
	client.checkForNeedToExitFn = gpCheckForNeedToExitFn;
	return sendTransitionMessage(&client, addrList, msg, msgLen, 20 /* maxRetries */, 1 /* retrySleepTimeSeconds */);
}
