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
	URI_HTTP,
	URI_GPFDIST,
	URI_EXEC
}	UriProtocol;

#define PROTOCOL_FILE	"file://"
#define PROTOCOL_FTP	"ftp://"
#define PROTOCOL_HTTP	"http://"
#define PROTOCOL_GPFDIST	"gpfdist://"
#define PROTOCOL_EXEC 	"exec://"

typedef struct Uri
{
	UriProtocol protocol;
	char	   *hostname;
	int         port;
	char	   *path;
}	Uri;

extern Uri *ParseUriString(const char *uri);

#endif   /* URI_H */
