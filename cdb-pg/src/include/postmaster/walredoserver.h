/*-------------------------------------------------------------------------
 *
 * walredoserver.c
 *	  Process under QD postmaster that manages redo of the WAL by the standby.
 *
 *
 * Copyright (c) 2007, greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALREDOSERVER_H
#define WALREDOSERVER_H

extern int	walredoserver_start(void);
extern bool IsWalRedoProcess(void);
extern int  WalRedoServerShmemSize(void);
extern void WalRedoServerShmemInit(void);
extern bool WalRedoServerNewCheckpointLocation(XLogRecPtr *newCheckpointLocation);
#endif   /* WALREDOSERVER_H */


