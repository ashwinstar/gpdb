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
#include "utils/uri.h"

#include <ctype.h>
#include <arpa/inet.h>	 /* inet_ntoa() */ 

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
	if (IS_FILE_URI(uri_str))
	{
		uri->protocol = URI_FILE;
		protocol_len = strlen(PROTOCOL_FILE);
	}
	else if (pg_strncasecmp(uri_str, PROTOCOL_FTP, strlen(PROTOCOL_FTP)) == 0)
	{
		uri->protocol = URI_FTP;
		protocol_len = strlen(PROTOCOL_FTP);
	}
	else if (IS_HTTP_URI(uri_str))
	{
		uri->protocol = URI_HTTP;
		protocol_len = strlen(PROTOCOL_HTTP);
	}
	else if (IS_GPFDIST_URI(uri_str))
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
			char   *colon;

			len = end - start;

			/*
			 * host
			 */
			uri->hostname = (char *) palloc(len + 1);
			strncpy(uri->hostname, start, len);
			uri->hostname[len] = '\0';
			
			/*
			 * port
			 */
			colon = strchr(uri->hostname, ':');
			if (colon) 
			{
				int portlen = 0;
				
				uri->port = atoi(colon + 1);
				portlen = strlen(colon);
				
				/* now truncate ":<port>" from hostname */
				uri->hostname[len - portlen] = '\0';

				*colon = 0;
			}
			else
			{
				uri->port = -1; /* no port was indicated. will use default if needed */
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
