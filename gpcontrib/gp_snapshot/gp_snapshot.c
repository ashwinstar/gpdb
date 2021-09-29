/*--------------------------------------------------------------------------
 *
 * gp_snapshot.c
 *	  Routines to support taking cluster consistent disk-level snapshots
 *
 * Portions Copyright (c) 2021-Present VMware, Inc. or its affiliates.
 *--------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "storage/lwlock.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(gp_start_snapshot);
PG_FUNCTION_INFO_V1(gp_stop_snapshot);

Datum
gp_start_snapshot(PG_FUNCTION_ARGS)
{
	/*
	 * Acquire TwophaseCommitLock in EXCLUSIVE mode. This is to ensure
	 * cluster-wide consistency by blocking distributed commit prepared
	 * broadcasts for concurrent twophase transactions where a QE segment has
	 * written WAL.
	 *
	 * Side effect: cancel/die interrupts are held off until lock release. So,
	 * really need to find better alternative than light-weight locks.
	 */
	LWLockAcquire(TwophaseCommitLock, LW_EXCLUSIVE);
	PG_RETURN_VOID();
}

Datum
gp_stop_snapshot(PG_FUNCTION_ARGS)
{
	LWLockRelease(TwophaseCommitLock);
	PG_RETURN_VOID();
}
