/*-------------------------------------------------------------------------
 *
 * walsendserver.c
 *	  Process under QD postmaster that manages sending WAL to the standby.
 *
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALSENDSERVER_H
#define WALSENDSERVER_H

#include "access/xlogdefs.h"

typedef enum WalSendRequestCommand
{
	PositionToEnd = 0,
	Catchup = 1,
	WriteWalPages = 2,
	MaxWalSendRequestCommand  = 3 /* must always be last */
} WalSendRequestCommand;

typedef struct WalSendRequest
{
	WalSendRequestCommand	command;

	/*
	 * Data for WriteWalPages.
	 */
	int			startidx;	/* Index to the start page the in WAL buffer pool */
	int			npages;		/* Number of pages to write */

	TimeLineID	timeLineID;
	uint32 		logId;
	uint32 		logSeg;
	uint32 		logOff;
	bool 		haveNewCheckpointLocation;
	XLogRecPtr 	newCheckpointLocation;

	/*
	 * Data for MirrorQDEnabled.
	 */
	XLogRecPtr	flushedLocation;
} WalSendRequest;

typedef struct WalSendResponse
{
	bool	ok;
	
} WalSendResponse;

extern int	walsendserver_start(void);
extern bool IsWalSendProcess(void);
extern char* WalSendRequestCommandToString(WalSendRequestCommand command);


#endif   /* WALSENDSERVER_H */

