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

#include "cdb/cdbconn.h"            /* me */
#include "cdb/cdbutil.h"            /* CdbComponentDatabaseInfo */
#include "cdb/cdbvars.h"


void
cdbconn_initSegmentDescriptor(SegmentDatabaseDescriptor        *segdbDesc,
                              struct CdbComponentDatabaseInfo  *cdbinfo)
{
    memset(segdbDesc, 0, sizeof(*segdbDesc));

    segdbDesc->segment_database_info = cdbinfo;
    segdbDesc->conn = NULL;
    initPQExpBuffer(&segdbDesc->error_message);
    segdbDesc->segindex = cdbinfo->segindex;

    segdbDesc->dbname = NULL;
    segdbDesc->motionListener = 0;
    segdbDesc->whoami = NULL;
}                               /* cdbconn_initSegmentDescriptor */


/* Free all memory owned by this segment descriptor. */
void
cdbconn_termSegmentDescriptor(SegmentDatabaseDescriptor *segdbDesc)
{
    /* Free the error message buffer. */
    termPQExpBuffer(&segdbDesc->error_message);

    /* Free connection info. */
    if (segdbDesc->dbname)
    {
        free(segdbDesc->dbname);
        segdbDesc->dbname = NULL;
    }
    if (segdbDesc->whoami)
    {
        free(segdbDesc->whoami);
        segdbDesc->whoami = NULL;
    }
}                               /* cdbconn_termSegmentDescriptor */


/* Connect to a QE as a client via libpq. */
bool                            /* returns true if connected */
cdbconn_doConnect(SegmentDatabaseDescriptor    *segdbDesc,
                  const char                   *dbname,
                  const char                   *options)
{
    CdbComponentDatabaseInfo   *q = segdbDesc->segment_database_info;
    PQExpBufferData             buffer;
    int                         snipoff;

    /*
     * We use PQExpBufferData instead of StringInfoData
     * because the former uses malloc, the latter palloc.
     * We are in a thread, and we CANNOT use palloc since it's not
     * thread safe.  We cannot call elog or ereport either for the
     * same reason.
     */
    initPQExpBuffer(&buffer);

    /* Save the database name, if given.  Can be NULL during cdb_setup(). */
    free(segdbDesc->dbname);
    segdbDesc->dbname = NULL;
    if (dbname)
        segdbDesc->dbname = strdup(dbname);

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

    if (dbname)
        appendPQExpBuffer(&buffer, "dbname=%s ", dbname);
    
    appendPQExpBuffer(&buffer, "user=%s ", q->mppsuperuser);

    /* Password is last so it can be snipped for error message. */
    snipoff = buffer.len;
    if ( q->mppsuperpswd != NULL )
        appendPQExpBuffer(&buffer, "password='%s' ", q->mppsuperpswd);

    /*
     * Call libpq to connect
     */
    segdbDesc->conn = PQconnectdb(buffer.data);

    /* Wipe password. */
    memset(buffer.data+snipoff, 0, buffer.len-snipoff);
    buffer.len = snipoff;

    /* Build whoami string to identify the QE for use in messages. */
    cdbconn_setSliceIndex(segdbDesc, -1);

    /*
     * Check for connection failure.
     */
    if ( PQstatus(segdbDesc->conn) == CONNECTION_BAD )
    {
        appendPQExpBuffer(&segdbDesc->error_message,
                          "Unable to connect to %s with options: %s; %s",
                          segdbDesc->whoami,
                          buffer.data,
                          PQerrorMessage(segdbDesc->conn));

        /* Don't use elog, it's not thread-safe */
        if (DEBUG2 >= log_min_messages)
            fputs(segdbDesc->error_message.data, stderr);

        PQfinish(segdbDesc->conn);
        segdbDesc->conn = NULL;
    }

    /*
     * Successfully connected.
     */
    else
    {
        /* Command the QE to initialize its motion layer.
         * Wait for it to respond giving us the TCP port number
         * where it listens for connections from the gang below.
         */
        segdbDesc->motionListener = PQmotionListener(segdbDesc->conn);
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

    /* Format the slice index (if any). */
    if (sliceIndex >= 0)
        appendPQExpBuffer(scratchbuf, "slice#%d ", sliceIndex);

    /* Format the identity of the segment db. */
    if (q->segindex >= 0)
        appendPQExpBuffer(scratchbuf, "qe#%d%s",
        q->segindex, (q->isprimary ? "" : "S"));
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


