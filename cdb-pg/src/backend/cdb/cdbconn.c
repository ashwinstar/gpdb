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

#include "cdb/cdbconn.h"		/* me */
#include "cdb/cdbutil.h"		/* CdbComponentDatabaseInfo */
#include "cdb/cdbvars.h"


/* Initialize a QE connection descriptor in storage provided by the caller. */
void
cdbconn_initSegmentDescriptor(SegmentDatabaseDescriptor * segdbDesc,
							  struct CdbComponentDatabaseInfo * cdbinfo,
							  const char *dbname,
							  struct Gang * gang,
							  int gangindex)
{
	memset(segdbDesc, 0, sizeof(*segdbDesc));

	/* Segment db info */
	segdbDesc->segment_database_info = cdbinfo;
	segdbDesc->segindex = cdbinfo->segindex;
	segdbDesc->dbname = dbname ? strdup(dbname) : NULL;

	/* The gang I belong to, and 0-based position within the gang. */
	segdbDesc->gang = gang;
	segdbDesc->gangindex = gangindex;

	/* Connection info */
	segdbDesc->conn = NULL;
	segdbDesc->motionListener = 0;
	segdbDesc->whoami = NULL;

	/* Connection error info */
	segdbDesc->errcode = 0;
	initPQExpBuffer(&segdbDesc->error_message);
}	/* cdbconn_initSegmentDescriptor */


/* Free all memory owned by this segment descriptor. */
void
cdbconn_termSegmentDescriptor(SegmentDatabaseDescriptor * segdbDesc)
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

	/* Free connection info. */
	if (segdbDesc->whoami)
	{
		free(segdbDesc->whoami);
		segdbDesc->whoami = NULL;
	}
}	/* cdbconn_termSegmentDescriptor */


/* Connect to a QE as a client via libpq. */
bool							/* returns true if connected */
cdbconn_doConnect(SegmentDatabaseDescriptor * segdbDesc,
				  const char *options)
{
	CdbComponentDatabaseInfo *q = segdbDesc->segment_database_info;
	PQExpBufferData buffer;
	int			passoff = 0;
	int			passend = 0;

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

	if (q->hostname == NULL)
		appendPQExpBufferStr(&buffer, "host='' ");
	else if (isdigit(q->hostname[0]))
		appendPQExpBuffer(&buffer, "hostaddr=%s ", q->hostname);
	else
		appendPQExpBuffer(&buffer, "host=%s ", q->hostname);

	appendPQExpBuffer(&buffer, "port=%u ", q->port);

	if (segdbDesc->dbname)
		appendPQExpBuffer(&buffer, "dbname=%s ", segdbDesc->dbname);

	appendPQExpBuffer(&buffer, "user=%s ", q->mppsuperuser);

	if (q->mppsuperpswd != NULL)
	{
		appendPQExpBufferStr(&buffer, "password='");
		passoff = buffer.len;
		appendPQExpBufferStr(&buffer, q->mppsuperpswd);
		passoff = buffer.len;
		appendPQExpBufferStr(&buffer, "' ");
	}

	/*
	 * Call libpq to connect
	 */
	segdbDesc->conn = PQconnectdb(buffer.data);

	/* Wipe password. */
	while (passoff < passend)
		buffer.data[passoff++] = '*';

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
			fputs(segdbDesc->error_message.data, stderr);

		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;
	}

	/*
	 * Successfully connected.
	 */
	else
	{
		/*
		 * Command the QE to initialize its motion layer. Wait for it to
		 * respond giving us the TCP port number where it listens for
		 * connections from the gang below.
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
}	/* cdbconn_doConnect */


/* Build text to identify this QE in error messages. */
void
cdbconn_setSliceIndex(SegmentDatabaseDescriptor * segdbDesc,
					  int sliceIndex)
{
	CdbComponentDatabaseInfo *q = segdbDesc->segment_database_info;
	PQExpBuffer scratchbuf = &segdbDesc->error_message;
	int			scratchoff = scratchbuf->len;

	/* Format the slice index (if any). */
	if (sliceIndex >= 0)
		appendPQExpBuffer(scratchbuf, "slice#%d ", sliceIndex);

	/* Format the identity of the segment db. */
	if (q->segindex >= 0)
		appendPQExpBuffer(scratchbuf, "%s#%d",
						  q->isprimary ? "seg" : "mirror",
						  q->segindex);
	else
		appendPQExpBuffer(scratchbuf, "secondary entry db");

	/* Format the connection info. */
	appendPQExpBuffer(scratchbuf, " %s:%d",
					  q->hostname, q->port);

	/* If connected, format the QE's process id. */
	if (segdbDesc->conn)
	{
		int			pid = PQbackendPID(segdbDesc->conn);

		if (pid)
			appendPQExpBuffer(scratchbuf, " pid=%d", pid);
	}

	/* Store updated whoami text. */
	free(segdbDesc->whoami);
	segdbDesc->whoami = strdup(scratchbuf->data + scratchoff);

	/* Give back our scratch space at tail of error_message buffer. */
	truncatePQExpBuffer(scratchbuf, scratchoff);
}	/* cdbconn_setSliceIndex */
