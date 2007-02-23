/*-------------------------------------------------------------------------
*
* uri.h
*	  Definitions for URI strings
*
* Copyright (c) 2007, greenplum inc
*
*-------------------------------------------------------------------------
*/
#ifndef URI_H
#define URI_H

typedef enum UriProtocol
{
	URI_FILE,
	URI_FTP,
	URI_HTTP
}	UriProtocol;

#define PROTOCOL_FILE	"file://"
#define PROTOCOL_FTP	"ftp://"
#define PROTOCOL_HTTP	"http://"

typedef struct Uri
{
	UriProtocol protocol;
	char	   *hostname;
	char	   *path;
}	Uri;

extern Uri *ParseUriString(const char *uri);

#endif   /* URI_H */
