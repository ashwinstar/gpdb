/*
 *  filerepconnclient.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved.
 *
 */

/*
 *
 * Responsibilities of this module.
 *		*) 
 *
 */
#include "postgres.h"
#include <signal.h>

#include "cdb/cdbvars.h"

#include "cdb/cdbfilerepconnclient.h"
#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"
#include "pqexpbuffer.h"

static PGconn	*conn = NULL;
/*
 *
 */
int 
FileRepConnClient_EstablishConnection(
	char	*hostAddress,
	int		port)
{
	int			status = STATUS_OK;
	PQExpBuffer	buffer = NULL;
	
/*	FileRepConnClient_CloseConnection();*/
	
	buffer = createPQExpBuffer();

	if (buffer == NULL)
	{
		/* XXX: allocation failed. Is this the right thing to do ? */
		return STATUS_ERROR;
	}

	appendPQExpBuffer(buffer, "host=%s ", hostAddress);	
	appendPQExpBuffer(buffer, "port=%d ", port);
	appendPQExpBuffer(buffer, "dbname=postgres ");
	appendPQExpBuffer(buffer, "connect_timeout=%d", gp_segment_connect_timeout);
	
	conn = PQconnectdb(buffer->data);

	if (PQstatus(conn) != CONNECTION_OK) {		
		ereport(LOG, 
				(errcode_for_socket_access(),
				 errmsg("ERROR(filerep): could not establish connection with server, host:'%s' port:'%d' err:'%s' : %m",
						hostAddress,
						port,
						PQerrorMessage(conn)),
				 FileRep_errcontext()));
		
		status = STATUS_ERROR;
	}
	
	/* NOTE Handle error message see ftsprobe.c */
	
	if (buffer) {
		destroyPQExpBuffer(buffer);
		buffer = NULL;
	}
	return status;	
}

/*
 *
 */
void
FileRepConnClient_CloseConnection(void)
{
	if (conn) {
		PQfinish(conn);
		conn = NULL;
	}
	return;
}

/*
 *
 *
 * Control Message has msg_type='C'.
 * Control Message is consumed by Receiver thread on mirror side.
 *
 * Data Message has msg_type='M'.
 * Data Message is inserted in Shared memory and consumed by Consumer
 * thread on mirror side.
 */
bool
FileRepConnClient_SendMessage(
	FileRepConsumerProcIndex_e	messageType, 
	bool	messageSynchronous,
	char*	message, 
	uint32	messageLength)
{
	char msgType = 0;
	int status = STATUS_OK;

#ifdef USE_ASSERT_CHECKING
	int prevOutCount = conn->outCount;
#endif // USE_ASSERT_CHECKING

	switch(messageType)
	{
		case FileRepMessageTypeXLog:
			msgType = '1';
			break;
		case FileRepMessageTypeWriter:
			msgType = '2';
			break;
		case FileRepMessageTypeAO01:
			msgType = '3';
			break;
		case FileRepMessageTypeAO02:
			msgType = '4';
			break;
		case FileRepMessageTypeShutdown:
			msgType = 'S';
			break;
		default:
			return false;
	}

	/**
	 * Note that pqPutMsgStart and pqPutnchar both may grow the connection's internal buffer, and do not
	 *   flush data
	 */
	if (pqPutMsgStart(msgType, true, conn) < 0 )
	{
		return false;
	}

	if ( pqPutnchar(message, messageLength, conn) < 0 )
	{
		return false;
	}

	/* Server side needs complete messages for mode-transitions so disable auto-flush since it flushes
	 *  partial messages
	 */
	pqPutMsgEndNoAutoFlush(conn);

	/* assert that a flush did not occur */
	Assert( prevOutCount + messageLength + 5 == conn->outCount ); /* the +5 is the amount added by pgPutMsgStart */

	/*
	 *                note also that we could do a flush beforehand to avoid
	 *                having pqPutMsgStart and pqPutnchar growing the buffer
	 */
	 if (messageSynchronous || conn->outCount >= file_rep_min_data_before_flush )
	{
		while ((status = pqFlush(conn)) > 0)
		{
			if (pqWait(FALSE, TRUE, conn))
			{
				status = -1;
				break;
			}
		}

		if (status < 0)
		{
			return false;
		}
		Assert( status == 0 );
		return true;
	}

	return true;
}
