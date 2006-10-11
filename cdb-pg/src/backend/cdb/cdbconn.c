/*-------------------------------------------------------------------------
 *
 * cdbconn.c
 *
 * SegmentDatabaseDescriptor methods
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "cdb/cdbconn.h"            /* me */
#include "cdb/cdbutil.h"            /* CdbComponentDatabaseInfo */
#include "cdb/cdbvars.h"


extern ErrorData  * MppGetErrorData(void);
static void MPPnoticeReceiver(void * arg, const PGresult * res)
{
	PGMessageField *pfield;
	int elevel = INFO;
	int errorcode = ERRCODE_SUCCESSFUL_COMPLETION;
	char * file = "";
	int line = 0;
	char * func = "";
	char  message[1024];
	char * detail = NULL;
	char * hint = NULL;
	char * context = NULL;
	
	SegmentDatabaseDescriptor    *segdbDesc = (SegmentDatabaseDescriptor    *) arg;
	if (!res)
		return;
	message[0] = '\0';
	for (pfield = res->errFields; pfield != NULL; pfield = pfield->next)
	{
		switch (pfield->code)
		{
			case PG_DIAG_SEVERITY:
				if (strcmp(pfield->contents,"WARNING")==0)
					elevel = WARNING;
				else if (strcmp(pfield->contents,"NOTICE")==0)
					elevel = NOTICE;
				else if (strcmp(pfield->contents,"DEBUG")==0)
					elevel = DEBUG1;
				else 
					elevel = INFO;
			case PG_DIAG_SQLSTATE:
				break;
			case PG_DIAG_MESSAGE_PRIMARY:
				strcpy(message, pfield->contents);
				if (segdbDesc)
				{
					cdbconn_setSliceIndex(segdbDesc, -1);
				}
				if (segdbDesc && segdbDesc->whoami)
				{
					strcat(message,"  (");
					strcat(message, segdbDesc->whoami);
					strcat(message,")");
				}
				break;
			case PG_DIAG_MESSAGE_DETAIL:
				detail = pfield->contents;
				break;
			case PG_DIAG_MESSAGE_HINT:
				hint = pfield->contents;
				break;
			case PG_DIAG_STATEMENT_POSITION:
			case PG_DIAG_INTERNAL_POSITION:
			case PG_DIAG_INTERNAL_QUERY:
				break;
			case PG_DIAG_CONTEXT:
				context = pfield->contents;
				break;
			case PG_DIAG_SOURCE_FILE:
				file = pfield->contents;
				break;
			case PG_DIAG_SOURCE_LINE:
				line = atol(pfield->contents);
				break;
			case PG_DIAG_SOURCE_FUNCTION:
				func = pfield->contents;
				break;
			case PG_DIAG_MPP_PROCESS_TAG:
				break;
			default:
				break;
			
		}
	}
	
	if (errstart(elevel, file, line, func))
	{
		ErrorData  *edata = MppGetErrorData();
		MemoryContext oldcontext;
		oldcontext = MemoryContextSwitchTo(ErrorContext);
		
		
		edata->sqlerrcode = errorcode;
			
		edata->message = pstrdup(message);
		
		
		if (detail)
			edata->detail = pstrdup(detail);
		if (hint)
			edata->hint = pstrdup(hint);
		if (context)
			edata->context = pstrdup(context);
		MemoryContextSwitchTo(oldcontext);
		errfinish(0);
	} 
	 
}


/* Initialize a QE connection descriptor in storage provided by the caller. */
void
cdbconn_initSegmentDescriptor(SegmentDatabaseDescriptor *segdbDesc,
                              struct CdbComponentDatabaseInfo  *cdbinfo,
                              const char                       *dbname,
                              const char                       *user)
{
    memset(segdbDesc, 0, sizeof(*segdbDesc));

    /* Segment db info */
    segdbDesc->segment_database_info = cdbinfo;
    segdbDesc->segindex = cdbinfo->segindex;
    segdbDesc->dbname = dbname ? strdup(dbname) : NULL;
	segdbDesc->username = user ? strdup(user) : NULL;

    /* Connection info */
    segdbDesc->conn = NULL;
    segdbDesc->motionListener = 0;
    segdbDesc->whoami = NULL;

    /* Connection error info */
    segdbDesc->errcode = 0;
    initPQExpBuffer(&segdbDesc->error_message);
}                               /* cdbconn_initSegmentDescriptor */


/* Free all memory owned by this segment descriptor. */
void
cdbconn_termSegmentDescriptor(SegmentDatabaseDescriptor *segdbDesc)
{
    /* Free the error message buffer. */
    segdbDesc->errcode = 0;
    termPQExpBuffer(&segdbDesc->error_message);

    /* Free database name. */
    if (segdbDesc->dbname)
    {
        free(segdbDesc->dbname);
        segdbDesc->dbname = NULL;
    }

	if (segdbDesc->username)
	{
		free(segdbDesc->username);
		segdbDesc->username = NULL;
	}

    /* Free connection info. */
    if (segdbDesc->whoami)
    {
        free(segdbDesc->whoami);
        segdbDesc->whoami = NULL;
    }
}                               /* cdbconn_termSegmentDescriptor */


/* Connect to a QE as a client via libpq. */
bool                            /* returns true if connected */
cdbconn_doConnect(SegmentDatabaseDescriptor    *segdbDesc,
                  const char                   *options)
{
    CdbComponentDatabaseInfo   *q = segdbDesc->segment_database_info;
    PQExpBufferData             buffer;

    /*
     * We use PQExpBufferData instead of StringInfoData
     * because the former uses malloc, the latter palloc.
     * We are in a thread, and we CANNOT use palloc since it's not
     * thread safe.  We cannot call elog or ereport either for the
     * same reason.
     */
    initPQExpBuffer(&buffer);

    /*
     * Build the connection string
     */
    if (options)
    {
        appendPQExpBufferStr(&buffer, options);
        appendPQExpBufferChar(&buffer, ' ');
    }

    if ( q->hostname == NULL )
        appendPQExpBufferStr(&buffer, "host='' " );
    else if ( isdigit(q->hostname[0]) )
        appendPQExpBuffer(&buffer, "hostaddr=%s ", q->hostname );
    else
        appendPQExpBuffer(&buffer, "host=%s ", q->hostname );

    appendPQExpBuffer(&buffer, "port=%u ", q->port);

    if (segdbDesc->dbname)
        appendPQExpBuffer(&buffer, "dbname=%s ", segdbDesc->dbname);

    appendPQExpBuffer(&buffer, "user=%s ", segdbDesc->username);

    /*
     * Call libpq to connect
     */
    segdbDesc->conn = PQconnectdb(buffer.data);

    /* Build whoami string to identify the QE for use in messages. */
    cdbconn_setSliceIndex(segdbDesc, -1);

    /*
     * Check for connection failure.
     */
    if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
    {
        if (!segdbDesc->errcode)
            segdbDesc->errcode = ERRCODE_MPP_INTERCONNECTION_ERROR;
        appendPQExpBuffer(&segdbDesc->error_message,
                          "QD unable to connect to %s with options %s: %s\n",
                          segdbDesc->whoami,
                          buffer.data,
                          PQerrorMessage(segdbDesc->conn));

        /* Don't use elog, it's not thread-safe */
        if (DEBUG2 >= log_min_messages)
        {
            fputs(segdbDesc->error_message.data, stderr);
            fputs("\n", stderr);
        }

        PQfinish(segdbDesc->conn);
        segdbDesc->conn = NULL;
    }
    /*
     * Successfully connected.
     */
    else
    {
    	PQsetNoticeReceiver(segdbDesc->conn, &MPPnoticeReceiver, segdbDesc);
        /* Command the QE to initialize its motion layer.
         * Wait for it to respond giving us the TCP port number
         * where it listens for connections from the gang below.
         */
        segdbDesc->motionListener = PQmotionListener(segdbDesc->conn);

        /* Don't use elog, it's not thread-safe */
        if (DEBUG3 >= log_min_messages)
            fprintf(stderr, "Connected to %s motionListener=%d with options %s\n",
                    segdbDesc->whoami,
                    segdbDesc->motionListener,
                    buffer.data);
    }

    free(buffer.data);
    return segdbDesc->conn != NULL;
}                               /* cdbconn_doConnect */


/* Build text to identify this QE in error messages. */
void
cdbconn_setSliceIndex(SegmentDatabaseDescriptor    *segdbDesc,
                      int                           sliceIndex)
{
    CdbComponentDatabaseInfo   *q = segdbDesc->segment_database_info;
    PQExpBuffer scratchbuf = &segdbDesc->error_message;
    int         scratchoff = scratchbuf->len;

    /* Format the identity of the segment db. */
    if (q->segindex >= 0)
    {
        appendPQExpBuffer(scratchbuf, "%s%d",
                          q->isprimary ? "seg" : "mir",
                          q->segindex);

        /* Format the slice index. */
        if (sliceIndex > 0)
            appendPQExpBuffer(scratchbuf, " slice%d", sliceIndex);
    }
    else
        appendPQExpBuffer(scratchbuf, "secondary entry db");

    /* Format the connection info. */
    appendPQExpBuffer(scratchbuf, " %s:%d",
                      q->hostname, q->port);

    /* If connected, format the QE's process id. */
    if (segdbDesc->conn)
    {
        int pid = PQbackendPID(segdbDesc->conn);
        if (pid)
            appendPQExpBuffer(scratchbuf, " pid=%d", pid);
    }

    /* Store updated whoami text. */
    free(segdbDesc->whoami);
    segdbDesc->whoami = strdup(scratchbuf->data + scratchoff);

    /* Give back our scratch space at tail of error_message buffer. */
    truncatePQExpBuffer(scratchbuf, scratchoff);
}                               /* cdbconn_setSliceIndex */


