/*-------------------------------------------------------------------------
*
* uriparser.c
*	  Functions for parsing URI strings
*
* Copyright (c) 2007, greenplum inc
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include <ctype.h>

#include "utils/uri.h"

/*
 * This routines converts a string to URI object.
 * It is also used to validate the URI format.
 */
Uri *
ParseUriString(const char *uri_str)
{
	Uri		   *uri = (Uri *) palloc(sizeof(Uri));
	char	   *start,
			   *end;
	int			protocol_len,
				len;

 	uri->port = -1;

	/*
	 * parse protocol
	 */
	if (strncmp(uri_str, PROTOCOL_FILE, strlen(PROTOCOL_FILE)) == 0)
	{
		uri->protocol = URI_FILE;
		protocol_len = strlen(PROTOCOL_FILE);
	}
	else if (strncmp(uri_str, PROTOCOL_FTP, strlen(PROTOCOL_FTP)) == 0)
	{
		uri->protocol = URI_FTP;
		protocol_len = strlen(PROTOCOL_FTP);
	}
	else if (strncmp(uri_str, PROTOCOL_HTTP, strlen(PROTOCOL_HTTP)) == 0)
	{
		uri->protocol = URI_HTTP;
		protocol_len = strlen(PROTOCOL_HTTP);
	}
	else if (strncmp(uri_str, PROTOCOL_GPFDIST, strlen(PROTOCOL_GPFDIST)) == 0)
	{
		uri->protocol = URI_GPFDIST;
		protocol_len = strlen(PROTOCOL_GPFDIST);
	}
	else
	{
		protocol_len = 0; /* shut compiler up */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid URI \'%s\' : undefined protocol", uri_str)));
	}

	/* make sure there is more to the uri string */
	if (strlen(uri_str) <= protocol_len)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		errmsg("invalid URI \'%s\' : missing host name and path", uri_str)));

	/*
	 * parse host name
	 */
	start = (char *) uri_str + protocol_len;

	if (*start == '/')			/* format "prot:///" ? (no hostname) */
	{
		/* the default is "localhost" */
		const char *lh = "localhost";

		len = strlen(lh);

		uri->hostname = (char *) palloc(len + 1);
		strncpy(uri->hostname, lh, len);
		uri->hostname[len] = '\0';

		end = start;
	}
	else
	{
		end = strchr(start, '/');

		if (end == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid URI \'%s\' : missing host name or path", uri_str)));
		}
		else
		{
			char* colon;

			len = end - start;

			uri->hostname = (char *) palloc(len + 1);
			strncpy(uri->hostname, start, len);
			uri->hostname[len] = '\0';
			
			colon = strchr(uri->hostname, ':');
			if (colon) {
				uri->port = atoi(colon + 1);
				*colon = 0;
			}
			else if(uri->protocol == URI_GPFDIST)
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid URI \'%s\' : gpfdist must indicate a "
						        "port number", uri_str)));			
			}
		}
	}

	/*
	 * We continue from the trailing host '/' since the
	 * path is an absolute path. Our previous ending point
	 * is the beginning of the file path, until the end of
	 * the uri string.
	 */
	start = end;

	len = strlen(start);

	/* make sure there is more to the uri string */
	if (len <= 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid URI \'%s\' : missing path", uri_str)));

	uri->path = (char *) palloc(len + 1);
	strcpy(uri->path, start);
	uri->path[len] = '\0';

	return uri;
}
