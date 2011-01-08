/*-------------------------------------------------------------------------
 *
 * twophase.h
 *	  Two-phase-commit related declarations.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/twophase.h,v 1.11 2009/01/01 17:23:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "access/xlogdefs.h"
#include "storage/proc.h"
#include "utils/timestamp.h"

/*
 * Directory where two phase commit files reside within PGDATA
 */
#define TWOPHASE_DIR "pg_twophase"

/*
 * GlobalTransactionData is defined in twophase.c; other places have no
 * business knowing the internal definition.
 */
typedef struct GlobalTransactionData *GlobalTransaction;

/* GUC variable */
extern int	max_prepared_xacts;

extern Size TwoPhaseShmemSize(void);
extern void TwoPhaseShmemInit(void);

extern PGPROC *TwoPhaseGetDummyProc(TransactionId xid);

extern GlobalTransaction MarkAsPreparing(TransactionId xid, 
				LocalDistribXactRef *distribToLocalXactRef,
				const char *gid,
				TimestampTz prepared_at,
				Oid owner, Oid databaseid);

extern void StartPrepare(GlobalTransaction gxact);
extern void EndPrepare(GlobalTransaction gxact);

extern TransactionId PrescanPreparedTransactions(void);
extern void RecoverPreparedTransactions(void);

extern void RecreateTwoPhaseFile(TransactionId xid, void *content, int len);
extern void RemoveTwoPhaseFile(TransactionId xid, bool giveWarning);

extern void CheckPointTwoPhase(XLogRecPtr redo_horizon);

extern void PrepareIntentAppendOnlyCommitWork(char *gid);

extern bool FinishPreparedTransaction(const char *gid, bool isCommit, bool raiseErrorIfNotFound);

extern int TwoPhaseRecoverMirror(void);

#endif   /* TWOPHASE_H */
