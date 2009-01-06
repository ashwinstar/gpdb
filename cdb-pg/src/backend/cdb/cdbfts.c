/*-------------------------------------------------------------------------
 *
 * cdbfts.c
 *	  Provides fault tolerance service routines for mpp.
 *
 * Copyright (c) 2003-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <assert.h>

#include "miscadmin.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "utils/memutils.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbdisp.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "cdb/cdblink.h"
#include "cdb/cdbtm.h"
#include "libpq/libpq-be.h"
#include "commands/dbcommands.h"
#include "storage/proc.h"

#include "executor/spi.h"

#include "postmaster/service.h"
#include "postmaster/walsendserver.h"

FtsProbeInfo *ftsProbeInfo = NULL; /* Probe process updates this structure */
volatile bool	*ftsEnabled;
static LWLockId	ftsControlLock;

volatile int		*ftsAction;
static volatile bool	*ftsReadOnlyFlag;
static volatile bool	*ftsAdminRequestedRO;

static uint64	local_fts_statusVersion;

static LWLockId ftsQDMirrorLock;
static LWLockId ftsQDMirrorUpdateConfigLock;
static struct FtsQDMirrorInfo *ftsQDMirrorInfo=NULL;

/*
 * This is a per-process global to indicate if that backend has been warned
 * about master mirroring not being synchronized.
 */
static bool QDMirroringNotSynchronizedWarningGiven = false;

static void CdbShutdown(void);
static char *QDMirroringDisabledReasonToString(QDMIRRORDisabledReason disabledReason);

/*
 * get fts share memory size
 */
int
FtsShmemSize(void)
{
	/*
	 * this shared memory block doesn't even need to *exist* on the
	 * QEs!
	 */
	if ((Gp_role != GP_ROLE_DISPATCH) && (Gp_role != GP_ROLE_UTILITY))
		return 0;

	return MAXALIGN(sizeof(FtsControlBlock));
}

void
FtsShmemInit(void)
{
	bool		found;
	FtsControlBlock *shared;

	shared = (FtsControlBlock *)ShmemInitStruct("Fault Tolerance manager", FtsShmemSize(), &found);
	if (!shared)
		elog(FATAL, "FTS: could not initialize fault tolerance manager share memory");

	/* Initialize locks and shared memory area */

	ftsEnabled = &shared->ftsEnabled;
	ftsControlLock = shared->ControlLock;
	ftsAction = &shared->ftsAction;

	ftsReadOnlyFlag = &shared->ftsReadOnlyFlag; /* global RO state */

	ftsAdminRequestedRO = &shared->ftsAdminRequestedRO; /* Admin request -- guc-controlled RO state */

	ftsQDMirrorLock = shared->ftsQDMirrorLock;
	ftsQDMirrorUpdateConfigLock = shared->ftsQDMirrorUpdateConfigLock;
	ftsQDMirrorInfo = &shared->ftsQDMirror;

	ftsProbeInfo = &shared->fts_probe_info;

	if (!IsUnderPostmaster)
	{
		shared->ControlLock = LWLockAssign();
		ftsControlLock = shared->ControlLock;

		shared->ftsQDMirrorLock = LWLockAssign();
		ftsQDMirrorLock = shared->ftsQDMirrorLock;
		
		shared->ftsQDMirrorUpdateConfigLock = LWLockAssign();
		ftsQDMirrorUpdateConfigLock = shared->ftsQDMirrorUpdateConfigLock;

		ftsQDMirrorInfo->configurationChecked = false;
		ftsQDMirrorInfo->state = QDMIRROR_STATE_NONE;
		ftsQDMirrorInfo->updateMask = QDMIRROR_UPDATEMASK_NONE;
		ftsQDMirrorInfo->disabledReason = QDMIRROR_DISABLEDREASON_NONE;
		memset(&ftsQDMirrorInfo->lastLogTimeVal, 0, sizeof(struct timeval));
		strcpy(ftsQDMirrorInfo->errorMessage, "");
		ftsQDMirrorInfo->haveNewCheckpointLocation = false;
		memset(&ftsQDMirrorInfo->newCheckpointLocation, 0, sizeof(XLogRecPtr));
		memset(ftsQDMirrorInfo->name, 0, sizeof(ftsQDMirrorInfo->name));
		ftsQDMirrorInfo->port = 0;

		/* initialize */
		shared->ftsReadOnlyFlag = gp_set_read_only;
		shared->ftsAdminRequestedRO = gp_set_read_only;

		shared->fts_probe_info.fts_probePid = 0;
		shared->fts_probe_info.fts_statusVersion = 0;

		if (Gp_role == GP_ROLE_DISPATCH)
		{
			shared->ftsAction = convertFaultActionValue(gp_fault_action_string);
			elog(DEBUG1, "FTS: Fault Tolerance Action is set to : %s", gp_fault_action_string);
		}
		shared->ftsEnabled = (*ftsAction != FTS_NONE);
	}
	elog(LOG, "ftsenabled %d", shared->ftsEnabled);
}

void
ftsLock(void)
{
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
}

void
ftsUnlock(void)
{
	LWLockRelease(ftsControlLock);
}

/*
 * Set System Read Only
 */
void
CondSetMPPReadonly(void)
{
	/* this no longer actually *sets* things to readonly, just checks. */
	if (*ftsAction == FTS_READONLY)
	{
		Assert(*ftsReadOnlyFlag == true);
	}
}

/*
 * Re-Connect segDB
 */
static bool
FtsReConnectDB(void *failedDB)
{
	return reconnectSegDB(failedDB);
}

static void
FtsNotifyProber(void)
{
	/*
	 * This is a full-scan request. We set the request-flag == to the bitmap version flag.
	 * When the version has been bumped, we know that the request has been filled.
	 */
	ftsProbeInfo->fts_probeScanRequested = ftsProbeInfo->fts_statusVersion;

	/* signal fts-probe */
	if (ftsProbeInfo->fts_probePid)
		kill(ftsProbeInfo->fts_probePid, SIGINT);

	/* sit and spin */
	while (ftsProbeInfo->fts_probeScanRequested == ftsProbeInfo->fts_statusVersion);
	{
		struct timeval tv;

		tv.tv_usec = 10000;
		tv.tv_sec = 0;
		select(0, NULL, NULL, NULL, &tv); /* don't care about return value. */
	}
}

/*
 * Test-Connection: This is called from the threaded context inside the
 * dispatcher: ONLY CALL THREADSAFE FUNCTIONS -- elog() is NOT threadsafe.
 */
bool
FtsTestConnection(CdbComponentDatabaseInfo *failedDBInfo, bool fullScan)
{
	if (!fullScan)
	{
		return FTS_STATUS_ISALIVE(failedDBInfo->dbid, ftsProbeInfo->fts_status);
	}

	FtsNotifyProber();

	return FTS_STATUS_ISALIVE(failedDBInfo->dbid, ftsProbeInfo->fts_status);
}

/*
 * Re-Configure the system: if someone has noticed that the status
 * version has been updated, they call this to verify that they've got
 * the right configuration.
 */
void
FtsReConfigureMPP(void)
{
	/* Has the version been bumped ? */
	if (local_fts_statusVersion != ftsProbeInfo->fts_statusVersion)
	{
		/* need to scan to pick up the latest view */
		detectFailedConnections();
		local_fts_statusVersion = ftsProbeInfo->fts_statusVersion;
	}

	elog(LOG, "FTS: reconfiguration is in progress");
	disconnectAndDestroyAllGangs();

	allocateWriterGang(GANGTYPE_PRIMARY_WRITER);
	allocateWriterGang(GANGTYPE_MIRROR_WRITER);

	/* rollback current DTM transaction */
	AbortCurrentTransaction();

	return;
}

/*
 * Shutdown the system
 */
static void
CdbShutdown(void)
{
	elog(ERROR, "FTS: Greenplum system is shut down (segment failure)");
}

static void
QDMirroringFormatTime(char *logTimeStr, int logTimeStrMax, 
							  struct timeval *lastLogTimeVal)
{
	char msbuf[8];
	pg_time_t stamp_time = (pg_time_t) lastLogTimeVal->tv_sec;
	
	/*
	 * The following code is copied from log_line_prefix.
	 */
	pg_strftime(logTimeStr, logTimeStrMax,
	/* leave room for microseconds... */
			 "%Y-%m-%d %H:%M:%S        %Z",
			 pg_localtime(&stamp_time, default_timezone));

	/* 'paste' milliseconds into place... */
	sprintf(msbuf, ".%06d", (int) (lastLogTimeVal->tv_usec));
	strncpy(logTimeStr + 19, msbuf, 7);
}

static void
QDMirroringUpdate(
	QDMIRRORUpdateMask 		updateMask,
	bool					validFlag,
	QDMIRRORState 			state,
	QDMIRRORDisabledReason	disabledReason,
	struct timeval          *lastLogTimeVal,
	char                    *errorMessage)
{
#define UPDATE_VALIDFLAG_CMD "update gp_configuration set valid='%c' where dbid = CAST(%d AS SMALLINT)"
#define UPDATE_MASTER_MIRRORING_CMD "update gp_master_mirroring set (summary_state, detail_state, log_time, error_message) = ('%s', %s, '%s'::timestamptz, %s);"
	int count = 0;
	char cmd[200 + QDMIRRORErrorMessageSize * 2 + 3];
	char detailValue[100];
	char logTimeStr[128];
	int err;
	char *summaryStateString;
	char *detailStateString;
	char errorMessageQuoted[QDMIRRORErrorMessageSize * 2 + 3];
	
	PG_TRY();
	{
		StartTransactionCommand();

		err = SPI_connect();
		if (err != SPI_OK_CONNECT)
		{
			elog(ERROR, "SPI_connect failed with error %d",
				 err);
		}

		if ((updateMask & QDMIRROR_UPDATEMASK_VALIDFLAG) != 0)
		{
			count = snprintf(cmd, sizeof(cmd),
					 		 UPDATE_VALIDFLAG_CMD,
					 		 (validFlag ? 't' : 'f'),
					 		 ftsQDMirrorInfo->dbid);
			if (count >= sizeof(cmd))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("FTS: format command string failure")));
			}

			err = SPI_execute(cmd, false, 0);
			if (err != SPI_OK_UPDATE)
			{
				elog(ERROR, "SPI_execute command \"%s\" failed with error %d",
					 cmd, err);
			}
		}

		if ((updateMask & QDMIRROR_UPDATEMASK_MASTERMIRRORING)!= 0)
		{
			switch (state)
			{
				case QDMIRROR_STATE_NONE:
					summaryStateString = "None";
					break;

				case QDMIRROR_STATE_NOTCONFIGURED:
					summaryStateString = "Not Configured";
					break;

				case QDMIRROR_STATE_CONNECTINGWALSENDSERVER:
				case QDMIRROR_STATE_POSITIONINGTOEND:
				case QDMIRROR_STATE_CATCHUPPENDING:
				case QDMIRROR_STATE_CATCHINGUP:
					summaryStateString = "Synchronizing";
					break;

				case QDMIRROR_STATE_SYNCHRONIZED:
					summaryStateString = "Synchronized";
					break;

				case QDMIRROR_STATE_DISABLED:
					summaryStateString = "Not Synchronized";
					break;

				default:
					summaryStateString = "Unknown";
					break;
			}

			if (state == QDMIRROR_STATE_DISABLED)
				detailStateString = 
					QDMirroringDisabledReasonToString(disabledReason);
			else
				detailStateString = NULL;

			if (detailStateString == NULL)
				strcpy(detailValue, "null");
			else
			{
				count = snprintf(detailValue, sizeof(detailValue),
						 		 "'%s'",
						 		 detailStateString);
				if (count >= sizeof(detailValue))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("FTS: format command string failure")));
				}
			}

			QDMirroringFormatTime(logTimeStr, sizeof(logTimeStr), 
								  lastLogTimeVal);
			
			/*
			 * Escape quote the error string before putting in DML statement...
			 */
			if (errorMessage != NULL)
			{
				int errorMessageLen = strlen(errorMessage);

				if (errorMessageLen == 0)
					strcpy(errorMessageQuoted, "null");
				else
				{
					size_t escapedLen;
					
					errorMessageQuoted[0] = '\'';
					escapedLen = PQescapeString(
						&errorMessageQuoted[1], 
						errorMessage, 
						errorMessageLen);
					errorMessageQuoted[escapedLen + 1] = '\'';
					errorMessageQuoted[escapedLen + 2] = '\0';

					elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
						 "Error message quoted: \"%s\"", errorMessageQuoted);
				}
			}
			else
				strcpy(errorMessageQuoted, "null");
			
			count = snprintf(cmd, sizeof(cmd),
					 		 UPDATE_MASTER_MIRRORING_CMD,
					 		 summaryStateString,
					 		 detailValue,
					 		 logTimeStr,
					 		 errorMessageQuoted);
			if (count >= sizeof(cmd))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("FTS: format command string failure")));
			}
			err = SPI_execute(cmd, false, 0);
			if (err != SPI_OK_UPDATE)
			{
				elog(ERROR, "SPI_execute command \"%s\" failed with error %d",
					 cmd, err);
			}

			elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
				 "Successfully executed command \"%s\"",
				 cmd);
		}
		
		SPI_finish();

		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		/* 
		 * Report the unexpected error.
		 */
		elog_dismiss(NOTICE);

		AbortCurrentTransaction();
	}
	PG_END_TRY();

}

void
CheckForQDMirroringWork(void)
{
	QDMIRRORUpdateMask 		updateMask;
	bool					validFlag;
	QDMIRRORState 			state;
	QDMIRRORDisabledReason	disabledReason;
	struct timeval          lastLogTimeVal;
	char					errorMessage[QDMIRRORErrorMessageSize];
	
	if (ftsQDMirrorInfo == NULL)
		return;
	/*
	 * Test without taking lock.
	 */
	if (ftsQDMirrorInfo->updateMask == QDMIRROR_UPDATEMASK_NONE)
		return;

	if (IsTransactionOrTransactionBlock())
		return;

	/*
	 * NOTE: We are trying to use the longer-term update config lock here.
	 */
	if (!LWLockConditionalAcquire(ftsQDMirrorUpdateConfigLock, LW_EXCLUSIVE))
		return;
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	
	if (ftsQDMirrorInfo->updateMask == QDMIRROR_UPDATEMASK_NONE)
	{
		LWLockRelease(ftsQDMirrorLock);
		LWLockRelease(ftsQDMirrorUpdateConfigLock);
		return;
	}

	updateMask = ftsQDMirrorInfo->updateMask;
	ftsQDMirrorInfo->updateMask = QDMIRROR_UPDATEMASK_NONE;

	validFlag = false;
	if ((updateMask & QDMIRROR_UPDATEMASK_VALIDFLAG) != 0)
	{
		validFlag = !ftsQDMirrorInfo->valid;

		/*
		 * Assume we are successful...
		 */
		ftsQDMirrorInfo->valid = validFlag;
	}

	state = ftsQDMirrorInfo->state;
	disabledReason = ftsQDMirrorInfo->disabledReason;
	lastLogTimeVal = ftsQDMirrorInfo->lastLogTimeVal;
	strcpy(errorMessage, ftsQDMirrorInfo->errorMessage);
	
	LWLockRelease(ftsQDMirrorLock);

	QDMirroringUpdate(updateMask, validFlag, 
		              state, disabledReason, &lastLogTimeVal, errorMessage);
	
	LWLockRelease(ftsQDMirrorUpdateConfigLock);
}

bool QDMirroringWriteCheck(void)
{
	bool giveWarning = false;
	QDMIRRORDisabledReason	disabledReason = QDMIRROR_DISABLEDREASON_NONE;
	struct timeval lastLogTimeVal = {0, 0};
	
	if (ftsQDMirrorInfo == NULL)
		return false;	// Don't know yet.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->state == QDMIRROR_STATE_SYNCHRONIZED)
	{
		LWLockRelease(ftsQDMirrorLock);
		return true;
	}
	
	if (!QDMirroringNotSynchronizedWarningGiven &&
		ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED)
	{
		giveWarning = true;
		disabledReason = ftsQDMirrorInfo->disabledReason;
		lastLogTimeVal = ftsQDMirrorInfo->lastLogTimeVal;
	}
	
	LWLockRelease(ftsQDMirrorLock);

	if (giveWarning)
	{
		char logTimeStr[100];
		
		QDMirroringFormatTime(logTimeStr, sizeof(logTimeStr), 
					          &lastLogTimeVal);

		ereport(WARNING,
				(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
				 errSuppressOutputToLog(),	// Don't clutter up the system log with these warnings!
				 errmsg("Master mirroring is not synchronized as of %s (%s).  "
				        "The GPDB system is currently not highly-available",
				        logTimeStr,
				        QDMirroringDisabledReasonToString(disabledReason))));

		QDMirroringNotSynchronizedWarningGiven = true;
	}
	
	return false;
}

void
disableQDMirroring(char *detail, char *errorMessage, QDMIRRORDisabledReason disabledReason)
{
	struct timeval lastLogTimeVal;

	Assert(ftsQDMirrorInfo != NULL);
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	
	/*
	 * Don't overwrite.
	 */
	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_DISABLED)
	{
		ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
		ftsQDMirrorInfo->disabledReason = disabledReason;

		switch (disabledReason)
		{
			case QDMIRROR_DISABLEDREASON_TOOFARBEHIND:
			case QDMIRROR_DISABLEDREASON_CONNECTIONERROR:
			case QDMIRROR_DISABLEDREASON_WALSENDSERVERERROR:
			case QDMIRROR_DISABLEDREASON_ADMINISTRATORDISABLED:
			case QDMIRROR_DISABLEDREASON_UNEXPECTEDERROR:
				if (disabledReason == QDMIRROR_DISABLEDREASON_ADMINISTRATORDISABLED)
					ereport(NOTICE,
							(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("Master mirroring not synchronized because the standby master was not started by the administrator")));
				else
					ereport(WARNING,
							(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							 errmsg("Master mirroring synchronization lost"),
							 errdetail(detail)));
				GetLastLogTimeVal(&lastLogTimeVal);		// Get the log message time
				// from the WARNING we just issued.

				QDMirroringNotSynchronizedWarningGiven = true;
													
				if (ftsQDMirrorInfo->valid)
				{
					/*
					 * Only do the update if we need to.
					 */
					ftsQDMirrorInfo->updateMask |= QDMIRROR_UPDATEMASK_VALIDFLAG;
				}
				ftsQDMirrorInfo->updateMask |= QDMIRROR_UPDATEMASK_MASTERMIRRORING;

				ftsQDMirrorInfo->lastLogTimeVal = lastLogTimeVal;

				/*
				 * Not specified or too long error message string will end up NULL
				 * in gp_master_mirroring table.
				 */
				if (errorMessage == NULL || 
					strlen(errorMessage) + 1 >= sizeof(ftsQDMirrorInfo->errorMessage))
					strcpy(ftsQDMirrorInfo->errorMessage, "");
				else
					strcpy(ftsQDMirrorInfo->errorMessage, errorMessage);
				break;

			case QDMIRROR_DISABLEDREASON_SHUTDOWN:
				break;
			
			default:
				elog(ERROR, "Unknown disabled reason %d", (int)disabledReason);
		}
	}
	
	LWLockRelease(ftsQDMirrorLock);
}

// Helps avoid pulling in cdbfts.h in xlog.c
void
disableQDMirroring_WalSendServerError(char *detail)
{
	disableQDMirroring(detail, ServiceGetLastClientErrorString(),
		               QDMIRROR_DISABLEDREASON_WALSENDSERVERERROR);
}

// Helps avoid pulling in cdbfts.h in walsendserver.c
void
disableQDMirroring_TooFarBehind(char *detail)
{
	disableQDMirroring(detail, NULL, QDMIRROR_DISABLEDREASON_TOOFARBEHIND);
}

// Helps avoid pulling in cdbfts.h in walsendserver.c
void
disableQDMirroring_ConnectionError(char *detail, char *errorMessage)
{
	disableQDMirroring(detail, errorMessage, QDMIRROR_DISABLEDREASON_CONNECTIONERROR);
}

// Helps avoid pulling in cdbfts.h in walsendserver.c
void
disableQDMirroring_UnexpectedError(char *detail)
{
	disableQDMirroring(detail, NULL, QDMIRROR_DISABLEDREASON_UNEXPECTEDERROR);
}

// Helps avoid pulling in cdbfts.h in walsendserver.c
void
disableQDMirroring_ShutDown(void)
{
	disableQDMirroring(NULL, NULL, QDMIRROR_DISABLEDREASON_SHUTDOWN);
}

// Helps avoid pulling in cdbfts.h in walsendserver.c
void
disableQDMirroring_AdministratorDisabled(void)
{
	disableQDMirroring(NULL, NULL, QDMIRROR_DISABLEDREASON_ADMINISTRATORDISABLED);
}

void
enableQDMirroring(char *message, char *detail)
{
	Assert(ftsQDMirrorInfo != NULL);
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	
	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_CATCHINGUP)
	{
		/*
		 * We've changed state while unlocked.
		 */
		if (ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED)
		{
			LWLockRelease(ftsQDMirrorLock);
			
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), 
				 "Master mirroring already disabled");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Unexpected master mirror state '%s'",
						    QDMirroringStateToString(state))));
		}
	}
	
	ftsQDMirrorInfo->state = QDMIRROR_STATE_SYNCHRONIZED;
	ftsQDMirrorInfo->updateMask |= QDMIRROR_UPDATEMASK_MASTERMIRRORING;
	ereport(NOTICE,
			(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
			 errmsg(message),
			 errdetail(detail)));
	GetLastLogTimeVal(&ftsQDMirrorInfo->lastLogTimeVal);		
	// Get the log message time
	// from the NOTICE we just issued.
	
	if (!ftsQDMirrorInfo->valid)
	{
		ftsQDMirrorInfo->updateMask |= QDMIRROR_UPDATEMASK_VALIDFLAG;
	}
	
	LWLockRelease(ftsQDMirrorLock);
}


bool
isQDMirroringEnabled(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;	// Don't know yet.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_SYNCHRONIZED);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

bool
isQDMirroringCatchingUp(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;	// Don't know yet.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_CATCHINGUP);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

bool
isQDMirroringNotConfigured(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;	// Don't know yet.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_NOTCONFIGURED);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

bool
isQDMirroringNotKnownYet(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return true;	// Assume.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_NONE);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

bool
isQDMirroringDisabled(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;	// Don't know yet.
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

char *
QDMirroringStateToString(QDMIRRORState state)
{
	switch (state)
	{
		case QDMIRROR_STATE_NONE:
			return "None";

		case QDMIRROR_STATE_NOTCONFIGURED:
			return "Not Configured";

		case QDMIRROR_STATE_CONNECTINGWALSENDSERVER:
			return "Connecting to WAL Send server";

		case QDMIRROR_STATE_POSITIONINGTOEND:
			return "Positioning to End";

		case QDMIRROR_STATE_CATCHUPPENDING:
			return "Catchup Pending";

		case QDMIRROR_STATE_CATCHINGUP:
			return "Catching Up";

		case QDMIRROR_STATE_SYNCHRONIZED:
			return "Synchronized";

		case QDMIRROR_STATE_DISABLED:
			return "Disabled";

		default:
			return "unknown";
	}
}

char *
QDMirroringStateString(void)
{
	QDMIRRORState state;
	
	if (ftsQDMirrorInfo == NULL)
		return "don't know yet";

	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	state = ftsQDMirrorInfo->state;
	LWLockRelease(ftsQDMirrorLock);

	return QDMirroringStateToString(state);
}

static char *
QDMirroringDisabledReasonToString(QDMIRRORDisabledReason disabledReason)
{
	switch (disabledReason)
	{
		case QDMIRROR_DISABLEDREASON_NONE:
			return "None";

		case QDMIRROR_DISABLEDREASON_CONNECTIONERROR:
			return "Connection error";

		case QDMIRROR_DISABLEDREASON_TOOFARBEHIND:
			return "Standby master too far behind";

		case QDMIRROR_DISABLEDREASON_WALSENDSERVERERROR:
			return "WAL Send server error";

		case QDMIRROR_DISABLEDREASON_UNEXPECTEDERROR:
			return "Unexpected error";

		case QDMIRROR_DISABLEDREASON_SHUTDOWN:
			return "Shutdown";

		case QDMIRROR_DISABLEDREASON_ADMINISTRATORDISABLED:
			return "Standby master was not started by the administrator";

		default:
			return "unknown";
	}
}

char *
QDMirroringDisabledReasonString(void)
{
	QDMIRRORState state;
	QDMIRRORDisabledReason disabledReason = QDMIRROR_DISABLEDREASON_NONE;
	
	if (ftsQDMirrorInfo == NULL)
		return NULL;

	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	state = ftsQDMirrorInfo->state;
	if (state == QDMIRROR_STATE_DISABLED)
		disabledReason = ftsQDMirrorInfo->disabledReason;
	LWLockRelease(ftsQDMirrorLock);

	if (state == QDMIRROR_STATE_DISABLED)
		return QDMirroringDisabledReasonToString(disabledReason);
	else
		return NULL;
}

void
FtsHandleNetFailure(SegmentDatabaseDescriptor ** segDB, int numOfFailed)
{
	int i;
	SegmentDatabaseDescriptor *failedDB;

	elog(DEBUG3, "FtsHandleNetFailure: numOfFailed %d", numOfFailed);

	for (i = 0; i < numOfFailed; i++)
	{
		failedDB = segDB[i];

		if (FtsReConnectDB(failedDB))
			continue;

		if (!isFTSEnabled())
			continue;

		/* we have a real failure, if we're supposed to shutdown,
		 * that's all we need to do. Otherwise, we need to continue
		 * scanning and connecting as long as we have more failures */
		if (*ftsAction == FTS_SHUTDOWN)
		{
			elog(LOG, "MPP detects segment failures, system is shut down");
			CdbShutdown();
			break;
		}
		else
		{
			FtsReConfigureMPP();
		}
	}

	elog(ERROR, "MPP detected %d segment failures, system is reconnected", numOfFailed);
}

/*
 * FtsHandleGangConnectionFailure is called by createGang during
 * creating connections return true if error need to be thrown
 */
bool
FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor * segdbDesc, int size)
{
	int			i;
	bool		dtx_active;
	bool		reportError = false;
    bool		realFaultFound = false;
	bool		forceRescan=true;

    for (i = 0; i < size; i++)
    {
        if (PQstatus(segdbDesc[i].conn) != CONNECTION_OK)
        {
			CdbComponentDatabaseInfo *segInfo = segdbDesc[i].segment_database_info;

			elog(DEBUG2, "FtsHandleGangConnectionFailure: looking for real fault on segment dbid %d", segInfo->dbid);

			if (!FtsTestConnection(segInfo, forceRescan))
			{
				elog(DEBUG2, "found fault with segment dbid %d", segInfo->dbid);
				realFaultFound = true;

				/* that at least one fault exists is enough, for now */
				break;
			}
			forceRescan = false; /* only force the rescan on the first call. */
        }
    }
    
    if (!realFaultFound)
	{
		/* If we successfully tested the gang and didn't notice a
		 * failure, our caller must've seen some kind of transient
		 * failure when the gang was originally constructed ...  */
		elog(DEBUG2, "FtsHandleGangConnectionFailure: no real fault found!");
        return false;
	}

	if (!isFTSEnabled())
	{
		return false;
	}

	/* shutdown if action is set to shutdown */
	if (*ftsAction == FTS_SHUTDOWN)
	{
		elog(LOG, "MPP detects segment failures, system is shut down");
		CdbShutdown();
		return true;
	}

	elog(LOG, "FTS: reconfiguration is in progress");

	forceRescan = true;
	for (i = 0; i < size; i++)
	{
		CdbComponentDatabaseInfo *segInfo = segdbDesc[i].segment_database_info;

		if (PQstatus(segdbDesc[i].conn) != CONNECTION_OK)
		{
			if (!FtsTestConnection(segInfo, forceRescan))
			{
				elog(LOG, "FTS: found bad segment with dbid %d", segInfo->dbid);
				/* probe process has already marked segment down. */
			}
			forceRescan = false; /* only force rescan on first call. */
		}
	}

	if (gangsExist())
	{
		reportError = true;
		disconnectAndDestroyAllGangs();
	}

	/* 
	 * KLUDGE: Do not error out if we are attempting a DTM protocol retry 
	 */
	if (DistributedTransactionContext == DTX_CONTEXT_QD_RETRY_PHASE_2)
	{
		return false;
	}

	/* is there a transaction active ? */
	dtx_active = isCurrentDtxActive();

    /* When the error is raised, it will abort the current DTM transaction */
	if (dtx_active)
	{
		elog((Debug_print_full_dtm ? LOG : DEBUG5),
			 "FtsHandleGangConnectionFailure found an active DTM transaction (returning true).");
		return true;
	}

	/*
	 * error out if this sets read only flag, at this stage the read only
	 * transaction checking has passed, so error out, but do not error out if
	 * tm is in recovery
	 */
	if ((*ftsReadOnlyFlag && !isTMInRecovery()) || reportError)
		return true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "FtsHandleGangConnectionFailure returning false.");

	return false;
}

void
FtsCondSetTxnReadOnly(bool *XactFlag)
{
	if (!isFTSEnabled())
		return;

	if (*ftsReadOnlyFlag && Gp_role != GP_ROLE_UTILITY)
		*XactFlag = true;
}

/*
 * Set Read only. There are two ways we can transition to readonly: request
 * from a super-user, or the ftsProber.
 *
 * There are a bunch of possibilities here:
 * 1) asking for RO and we're already in RO
 * 2) asking for RO and we've already asked for RO.
 * 3) asking for RW and we're already RW.
 * 4) asking for RW and we're "down" so we're RO.
 * 5) asking for RW and we had asked to be RO.
 *
 * The ftsProber is the final arbiter of whether we're read-only or not.
 */
void
FtsUpdateROStatus(bool value)
{
	/* Nothing to do ? */
	if (ftsReadOnlyFlag == NULL || value == *ftsReadOnlyFlag)
		return;

	if (*ftsAction == FTS_READONLY || *ftsAdminRequestedRO)
	{
		/* we are going to change the value */
		if (value || *ftsAdminRequestedRO)
		{
			/* ftsProber wants to mark us RO, wins every time */
			*ftsReadOnlyFlag = true;
		}
		else
		{
			/* ftsProber wants to mark us RW, but if a user has requested RO too then we stay RO. */
			if (!*ftsAdminRequestedRO)
				*ftsReadOnlyFlag = false;
		}
	}

	return;
}

/*
 * Set Read only. This is the entry for the GUC to request we
 * transition to readonly mode.
 *
 * The ftsProber is the final arbiter of whether we're read-only or not.
 */
bool
assign_gp_set_read_only(bool newval, bool doit, GucSource source __attribute__((unused)) )
{
	if (ftsReadOnlyFlag == NULL || newval == *ftsReadOnlyFlag)
		return true;

	if (doit)
	{
		if (newval)
		{
			/* we're trying to mark things RO ? */
			*ftsAdminRequestedRO = true;

			FtsNotifyProber();
		}
		else
		{
			/* we're trying to mark things RW */
			*ftsAdminRequestedRO = false;

			FtsNotifyProber();

			/* may have deferred work ? */
			if (!*ftsReadOnlyFlag)
				cdbtm_performDeferredRecovery();
		}

		/* set current transaction read only */
		XactReadOnly = *ftsReadOnlyFlag;
	}

	return true;
}

int
convertFaultActionValue(char *action)
{
	if (pg_strcasecmp(action, "readonly") == 0)
		return FTS_READONLY;
	else if (pg_strcasecmp(action, "shutdown") == 0)
		return FTS_SHUTDOWN;
	else if (pg_strcasecmp(action, "continue") == 0)
		return FTS_CONTINUE;
	else if (pg_strcasecmp(action, "none") == 0)
		return FTS_NONE;
	else
	{
		elog(FATAL, "FTS: Invalid Fault Tolerance Action: %s (valid actions: \"readonly\" or \"continue\")", gp_fault_action_string);

		/* not reached */
		gp_fault_action_string = "continue";
		return FTS_CONTINUE;
	}
}

bool
verifyFtsSyncCount(void)
{
	if (ftsProbeInfo->fts_probePid == 0)
		return true;

	return (local_fts_statusVersion != ftsProbeInfo->fts_statusVersion);
}

/* apply transient state to segdb state */
CdbComponentDatabases *
applyTransientState(CdbComponentDatabases *cdb_component_dbs)
{
	int			i;
	CdbComponentDatabaseInfo *segDbInfo;

	ftsLock();
	if (ftsProbeInfo->fts_probePid == 0)
	{
		ftsUnlock();
		return cdb_component_dbs;
	}

	for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		segDbInfo = &cdb_component_dbs->segment_db_info[i];

		segDbInfo->valid = FTS_STATUS_ISALIVE(segDbInfo->dbid, ftsProbeInfo->fts_status);
		segDbInfo->isprimary = FTS_STATUS_ISPRIMARY(segDbInfo->dbid, ftsProbeInfo->fts_status);
	}
	ftsUnlock();

	return cdb_component_dbs;
}

/*
 * Re-test transient failed state:
 *
 * What this actually does is find the first segment that is down, if none are down
 * it just resets the transient state array, it tests the first one found if it is
 * still down, then it leaves things alone.
 *
 * If all segments are up, the transient state array gets cleared, and the readonly flag
 * is set to "not read only" (read-write).
 */
void
FtsEvaluateTransientState(void)
{
	int			i;
	CdbComponentDatabases *cdb_component_dbs;
	CdbComponentDatabaseInfo *segDbInfo;

	cdb_component_dbs = getComponentDatabases();

	for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		segDbInfo = &cdb_component_dbs->segment_db_info[i];

		if (!FtsTestConnection(segDbInfo, false))
		{
			return;
		}
	}

	/* 
	 * If things have recently been marked read-write we may have
	 * deferred recovery work that needs doing.
	 */
	if (!*ftsReadOnlyFlag)
		cdbtm_performDeferredRecovery();
}

bool
isFtsReadOnlySet(void)
{
	return *ftsReadOnlyFlag;
}

bool
isMirrorQDConfigurationChecked(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = ftsQDMirrorInfo->configurationChecked;
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

bool
isQDMirroringPendingCatchup(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_CATCHUPPENDING);
	LWLockRelease(ftsQDMirrorLock);

	return result;
}

/*
 * While checking the mirroring configuration, call this procedure to set
 * that we are going to attempt connect and mirror to the standby.
 */
void
FtsSetQDMirroring(struct CdbComponentDatabaseInfo *dbinfo)
{
	int len;
	bool connectedToWalSendServer;
	bool walSendServerOk;

	Assert(dbinfo != NULL);

	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->configurationChecked)
	{
		// Someone else finished the configuration check work first.
		LWLockRelease(ftsQDMirrorLock);
		return;
	}

	Assert(ftsQDMirrorInfo->state == QDMIRROR_STATE_NONE);

	len = strlen(dbinfo->hostname);
	if (len >= sizeof(ftsQDMirrorInfo->name))
	{
		LWLockRelease(ftsQDMirrorLock);
		elog(ERROR, "hostname too long for fts master mirror configuration storage");
	}
	memcpy(ftsQDMirrorInfo->name, dbinfo->hostname, len);
	ftsQDMirrorInfo->name[len] = 0;
	ftsQDMirrorInfo->port = dbinfo->port;
	
	ftsQDMirrorInfo->dbid = dbinfo->dbid;
	ftsQDMirrorInfo->valid = dbinfo->valid;

	/*
	 * Connect to our WAL Send server, but not under lock.
	 * 
	 */
	ftsQDMirrorInfo->configurationChecked = true;
	if (Master_mirroring_administrator_disable)
	{
		LWLockRelease(ftsQDMirrorLock);
		disableQDMirroring_AdministratorDisabled();
		return;
	}
	
	ftsQDMirrorInfo->state = QDMIRROR_STATE_CONNECTINGWALSENDSERVER;
	ftsQDMirrorInfo->updateMask |= QDMIRROR_UPDATEMASK_MASTERMIRRORING;
	elog(NOTICE, "Master mirroring synchronizing");
	GetLastLogTimeVal(&ftsQDMirrorInfo->lastLogTimeVal);		
											// Get the log message time
											// from the NOTICE we just issued.
	LWLockRelease(ftsQDMirrorLock);

	connectedToWalSendServer = WalSendServerClientConnect(/* complain */ true);
	if (!connectedToWalSendServer)
	{
		disableQDMirroring_WalSendServerError("Cannot connect to the WAL Send server");
		return;
	}

	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_CONNECTINGWALSENDSERVER)
	{
		/*
		 * We've changed state while unlocked.
		 */
		if (ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED)
		{
			LWLockRelease(ftsQDMirrorLock);
			
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), 
				 "Master mirroring already disabled (connecting)");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			elog(ERROR,"Unexpected master mirroring state '%s'",
				 QDMirroringStateToString(state));
		}
	}
	
	ftsQDMirrorInfo->state = QDMIRROR_STATE_POSITIONINGTOEND;
	LWLockRelease(ftsQDMirrorLock);

	walSendServerOk = XLogQDMirrorPositionToEnd();
	if (!walSendServerOk)
	{
		disableQDMirroring_WalSendServerError("Error occurred during the PositionToEnd request to the WAL Send server");
		return;
	}
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_POSITIONINGTOEND)
	{
		/*
		 * We've changed state while unlocked.
		 */
		if (ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED)
		{
			LWLockRelease(ftsQDMirrorLock);
			
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), 
				 "Master mirroring already disabled (request PositionToEnd -- could not send to WAL Send server)");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			elog(ERROR,"Unexpected master mirror state '%s'",
				 QDMirroringStateToString(state));
		}
	}
	ftsQDMirrorInfo->state = QDMIRROR_STATE_CATCHUPPENDING;
	LWLockRelease(ftsQDMirrorLock);
	elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
		 "positioning master mirror to end complete -- next step is catch-up");
}

/*
 * Initiate catchup.  Called by XLogWrite when the write and flushed locations
 * are the same and a lock is held so those locations are momentarily stable.
 *
 * We queue the catchup request to the WAL Send server under the lock so the
 * server will get that request from the fifo queue before new log write 
 * requests (since we are enabling mirroring here).
 */
void
FtsQDMirroringCatchup(XLogRecPtr *flushedLocation)
{
	bool walSendServerOk;

	Assert(flushedLocation != NULL);

	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);

	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_CATCHUPPENDING)
	{
		LWLockRelease(ftsQDMirrorLock);
		return;
	}
	
	ftsQDMirrorInfo->state = QDMIRROR_STATE_CATCHINGUP;
	LWLockRelease(ftsQDMirrorLock);
	
	walSendServerOk = XLogQDMirrorCatchup(flushedLocation);
	if (!walSendServerOk)
	{
		disableQDMirroring_WalSendServerError("Error occurred during the Catchup request to the WAL Send server");
		return;
	}
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_SYNCHRONIZED)
	{
		/*
		 * We've changed state while unlocked.
		 */
		if (ftsQDMirrorInfo->state == QDMIRROR_STATE_DISABLED)
		{
			LWLockRelease(ftsQDMirrorLock);
			
			elog((Debug_print_qd_mirroring ? LOG : DEBUG5), 
				 "Master mirroring already disabled (request Catchup -- could not send to WAL Send server)");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			elog(ERROR,"Unexpected master mirror state '%s'",
				 QDMirroringStateToString(state));
		}
	}
	
	LWLockRelease(ftsQDMirrorLock);
	elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
		 "finished catching up master mirror");
}

/*
 * Save a new checkpoint location that the QD mirror can safely reply 
 * through and release any XLOG files freed up by reply.
 *
 * The location will be saved and sent to the QD mirror on the WAL send
 * request.
 */
void
FtsQDMirroringNewCheckpointLoc(XLogRecPtr *newCheckpointLocation)
{
	Assert(newCheckpointLocation != NULL);
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);

	if(ftsQDMirrorInfo->state != QDMIRROR_STATE_SYNCHRONIZED)
	{
		LWLockRelease(ftsQDMirrorLock);
		return;
	}

	if (ftsQDMirrorInfo->haveNewCheckpointLocation &&
		XLByteLT(*newCheckpointLocation, ftsQDMirrorInfo->newCheckpointLocation))
	{
		LWLockRelease(ftsQDMirrorLock);
		return;
	}

	ftsQDMirrorInfo->haveNewCheckpointLocation = true;
	ftsQDMirrorInfo->newCheckpointLocation = *newCheckpointLocation;
	
	LWLockRelease(ftsQDMirrorLock);
}

/*
 * Used to see if sending to the QD mirror is enabled.
 *
 */
void
FtsCheckReadyToSend(bool *ready, bool *haveNewCheckpointLocation, XLogRecPtr *newCheckpointLocation)
{
	Assert(ready != NULL);
	Assert(haveNewCheckpointLocation != NULL);
	Assert(newCheckpointLocation != NULL);

	*haveNewCheckpointLocation = false;
	memset(newCheckpointLocation, 0, sizeof(XLogRecPtr));
	
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);

	if(ftsQDMirrorInfo->state != QDMIRROR_STATE_SYNCHRONIZED)
	{
		LWLockRelease(ftsQDMirrorLock);
		
		*ready = false;
		return;
	}

	if (ftsQDMirrorInfo->haveNewCheckpointLocation)
	{
		/*
		 * Pass new checkpoint back and forget.
		 */
		*haveNewCheckpointLocation = true;
		memcpy(newCheckpointLocation, &ftsQDMirrorInfo->newCheckpointLocation, sizeof(XLogRecPtr));

		ftsQDMirrorInfo->haveNewCheckpointLocation = false;
	}
	
	LWLockRelease(ftsQDMirrorLock);
	
	*ready = true;
}

/*
 * While checking the mirroring configuration, call this procedure to set
 * that we are NOT going do mirroring.
 */
void
FtsSetNoQDMirroring(void)
{
	LWLockAcquire(ftsQDMirrorLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->configurationChecked)
	{
		// Someone else finished the configuration check work first.
		LWLockRelease(ftsQDMirrorLock);
		return;
	}

	Assert(ftsQDMirrorInfo->state == QDMIRROR_STATE_NONE);

	ftsQDMirrorInfo->configurationChecked = true;
	ftsQDMirrorInfo->state = QDMIRROR_STATE_NOTCONFIGURED;
	
	LWLockRelease(ftsQDMirrorLock);
}

void
FtsDumpQDMirrorInfo(void)
{
	if (!isQDMirroringEnabled() ||
		ftsQDMirrorInfo->name[0] == 0 || ftsQDMirrorInfo->port == 0)
	{
		elog(LOG, "Master mirroring not enabled");
		return;
	}

	elog(LOG, "Master mirror active name (%s:%d), port %d",
		 ftsQDMirrorInfo->name, 
		 (int)strlen(ftsQDMirrorInfo->name), 
		 ftsQDMirrorInfo->port);
	return;
}

void
FtsGetQDMirrorInfo(char **hostname, uint16 *port)
{
	Assert(hostname != NULL);
	Assert(port != NULL);

	if (ftsQDMirrorInfo->name[0] == 0 || ftsQDMirrorInfo->port == 0)
	{
		*hostname = NULL;
		*port = 0;
		return;
	}

	*hostname = ftsQDMirrorInfo->name;
	*port = ftsQDMirrorInfo->port;
	return;
}
