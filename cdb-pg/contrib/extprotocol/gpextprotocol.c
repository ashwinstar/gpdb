#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "access/extprotocol.h"
#include "catalog/pg_proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

typedef struct DemoUri
{
	char	   *protocol;
	char	   *path;
	
}	DemoUri;

static DemoUri *ParseDemoUri(const char *uri_str);
static void FreeDemoUri(DemoUri* uri);


/* Do the module magic dance */
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(demoprot_export);
PG_FUNCTION_INFO_V1(demoprot_import);

Datum demoprot_export(PG_FUNCTION_ARGS);
Datum demoprot_import(PG_FUNCTION_ARGS);


typedef struct {
	char	  *url;
	char	  *filename;
	FILE	  *file;
} extprotocol_t;

/*
 * Import data into GPDB.
 */
Datum 
demoprot_import(PG_FUNCTION_ARGS)
{
	extprotocol_t   *myData;
	char			*data;
	int				 datlen;
	size_t			 nread = 0;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_EXTPROTOCOL(fcinfo))
		elog(ERROR, "extprotocol_import: not called by external protocol manager");

	/* Get our internal description of the protocol */
	myData = (extprotocol_t *) EXTPROTOCOL_GET_USER_CTX(fcinfo);

	if(EXTPROTOCOL_IS_LAST_CALL(fcinfo))
	{
		/* we're done receiving data. close our connection */
		if(myData && myData->file)
			if(fclose(myData->file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not close file \"%s\": %m",
								 myData->filename)));
		
		PG_RETURN_INT32(0);
	}	

	if (myData == NULL)
	{
		/* first call. do any desired init */
		
		const char	*p_name = "demoprot";
		DemoUri		*parsed_url;
		char		*url = EXTPROTOCOL_GET_URL(fcinfo);
			
		myData 			 = palloc(sizeof(extprotocol_t));
				
		myData->url 	 = pstrdup(url);
		parsed_url 		 = ParseDemoUri(myData->url);
		myData->filename = pstrdup(parsed_url->path);
	
		if(strcasecmp(parsed_url->protocol, p_name) != 0)
			elog(ERROR, "internal error: demoprot called with a different protocol (%s)",
						parsed_url->protocol);

		FreeDemoUri(parsed_url);
		
		/* open the destination file (or connect to remote server in other cases) */
		myData->file = fopen(myData->filename, "r");
		
		if (myData->file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("demoprot_import: could not open file \"%s\" for reading: %m",
							 myData->filename),
					 errOmitLocation(true)));
		
		EXTPROTOCOL_SET_USER_CTX(fcinfo, myData);
	}

	/* =======================================================================
	 *                            DO THE IMPORT
	 * ======================================================================= */
	
	data 	= EXTPROTOCOL_GET_DATABUF(fcinfo);
	datlen 	= EXTPROTOCOL_GET_DATALEN(fcinfo);

	if(datlen > 0)
	{
		nread = fread(data, 1, datlen, myData->file);
		if (ferror(myData->file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("demoprot_import: could not write to file \"%s\": %m",
							 myData->filename)));		
	}

	
	PG_RETURN_INT32((int)nread);
}

/*
 * Export data out of GPDB.
 */
Datum 
demoprot_export(PG_FUNCTION_ARGS)
{
	extprotocol_t   *myData;
	char			*data;
	int				 datlen;
	size_t			 wrote = 0;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_EXTPROTOCOL(fcinfo))
		elog(ERROR, "extprotocol_export: not called by external protocol manager");

	/* Get our internal description of the protocol */
	myData = (extprotocol_t *) EXTPROTOCOL_GET_USER_CTX(fcinfo);

	if(EXTPROTOCOL_IS_LAST_CALL(fcinfo))
	{
		/* we're done sending data. close our connection */
		if(myData && myData->file)
			if(fclose(myData->file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not close file \"%s\": %m",
								 myData->filename)));
		
		PG_RETURN_INT32(0);
	}	

	if (myData == NULL)
	{
		/* first call. do any desired init */
		
		const char	*p_name = "demoprot";
		DemoUri		*parsed_url;
		char		*url = EXTPROTOCOL_GET_URL(fcinfo);
			
		myData 			 = palloc(sizeof(extprotocol_t));
				
		myData->url 	 = pstrdup(url);
		parsed_url 		 = ParseDemoUri(myData->url);
		myData->filename = pstrdup(parsed_url->path);
	
		if(strcasecmp(parsed_url->protocol, p_name) != 0)
			elog(ERROR, "internal error: demoprot called with a different protocol (%s)",
						parsed_url->protocol);

		FreeDemoUri(parsed_url);
		
		/* open the destination file (or connect to remote server in other cases) */
		myData->file = fopen(myData->filename, "a");
		
		if (myData->file == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("demoprot_export: could not open file \"%s\" for writing: %m",
							 myData->filename),
					 errOmitLocation(true)));
		
		EXTPROTOCOL_SET_USER_CTX(fcinfo, myData);
	}

	/* =======================================================================
	 *                            DO THE EXPORT
	 * ======================================================================= */
	
	data 	= EXTPROTOCOL_GET_DATABUF(fcinfo);
	datlen 	= EXTPROTOCOL_GET_DATALEN(fcinfo);

	if(datlen > 0)
	{
		wrote = fwrite(data, 1, datlen, myData->file);
		if (ferror(myData->file))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("demoprot_import: could not read from file \"%s\": %m",
							 myData->filename)));		
	}

	PG_RETURN_INT32((int)wrote);
}

/* --- utility functions --- */

static 
DemoUri *ParseDemoUri(const char *uri_str)
{
	DemoUri	   *uri = (DemoUri *) palloc0(sizeof(DemoUri));
	int			protocol_len;

 	uri->path = NULL;
 	uri->protocol = NULL;
 	
	/*
	 * parse protocol
	 */
	char *post_protocol = strstr(uri_str, "://");
		
	if(!post_protocol)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid demo prot URI \'%s\'", uri_str),
				 errOmitLocation(true)));
	}

	protocol_len = post_protocol - uri_str;
	uri->protocol = (char *) palloc0 (protocol_len + 1);
	strncpy(uri->protocol, uri_str, protocol_len);				

	/* make sure there is more to the uri string */
	if (strlen(uri_str) <= protocol_len)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		errmsg("invalid demo prot URI \'%s\' : missing path", uri_str),
		errOmitLocation(true)));

	/*
	 * parse path
	 */
	uri->path = pstrdup(uri_str + protocol_len + strlen("://"));
	
	return uri;
}

static
void FreeDemoUri(DemoUri *uri)
{
	if (uri->path)
		pfree(uri->path);
	if (uri->protocol)
		pfree(uri->protocol);
	
	pfree(uri);
}
