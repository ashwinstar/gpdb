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
	if (pg_strncasecmp(uri_str, PROTOCOL_FILE, strlen(PROTOCOL_FILE)) == 0)
	{
		uri->protocol = URI_FILE;
		protocol_len = strlen(PROTOCOL_FILE);
	}
	else if (pg_strncasecmp(uri_str, PROTOCOL_FTP, strlen(PROTOCOL_FTP)) == 0)
	{
		uri->protocol = URI_FTP;
		protocol_len = strlen(PROTOCOL_FTP);
	}
	else if (pg_strncasecmp(uri_str, PROTOCOL_HTTP, strlen(PROTOCOL_HTTP)) == 0)
	{
		uri->protocol = URI_HTTP;
		protocol_len = strlen(PROTOCOL_HTTP);
	}
	else if (pg_strncasecmp(uri_str, PROTOCOL_GPFDIST, strlen(PROTOCOL_GPFDIST)) == 0)
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

		if(uri->protocol == URI_GPFDIST)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid URI \'%s\' : must specify host name with gpfdist protocol", uri_str)));
		
		if(uri->protocol == URI_HTTP)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid URI \'%s\' : localhost is not allowed with http protocol", uri_str)));
		
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
			char   *resolvedAddr; 
			struct hostent *lookup; 

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
						
			
			/* 
			 * We want to disallow anything that resolves to localhost as it 
			 * is WAY too ambiguous in a distributed system. For now we do 
			 * allow it for "file" protocol as the matchup with segdb hostnames
			 * is done against gp_configuration, which could include "localhost"
			 * entries in a 1 machine GPDB.
			 */
			if(uri->protocol != URI_FILE)
			{
				lookup = gethostbyname(uri->hostname); 
				if (lookup == NULL) 
				{ 
					elog(ERROR, "Cannot resolve specified hostname '%s'", uri->hostname); 
				} 
				
				resolvedAddr = inet_ntoa(*(struct in_addr *) (lookup->h_addr_list[0])); 
				if( strlen(resolvedAddr) >= 3 && (strncmp(resolvedAddr, "127", 3) == 0 ))	
				{ 
					ereport(ERROR, 
							(errcode(ERRCODE_SYNTAX_ERROR), 
							 errmsg("invalid URI \'%s\' : 'localhost' is not allowed. It is too ambiguous in a GPDB Array", uri_str))); 
				} 				
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
