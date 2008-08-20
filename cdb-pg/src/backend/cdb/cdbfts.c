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

bool	   *ftsEnabled;
static LWLockId ftsControlLock;
static int *ftsSyncCount;
int		   *ftsAction;
static bool *ftsReadOnlyFlag;

static struct FtsSegDBState *ftsSegDBTransientState;
static int *ftsSegDBTransientCount;

static int	localFtsSyncCount = -1;
static bool InReConfigure = false;

static LWLockId ftsQDMirrorLock;
static LWLockId ftsQDMirrorUpdateConfigLock;
static struct FtsQDMirrorInfo *ftsQDMirrorInfo=NULL;

/*
 * This is a per-process global to indicate if that backend has been warned
 * about master mirroring not being synchronized.
 */
static bool QDMirroringNotSynchronizedWarningGiven = false;

static void CdbShutdown(void);
static void initPQConnectionBuffer(CdbComponentDatabaseInfo *, char *, char *, PQExpBuffer, bool);
static void markSegDBTransientState(int, bool);
static void markSegDBPersistentState(int, bool);
static char *QDMirroringDisabledReasonToString(QDMIRRORDisabledReason disabledReason);

#define incFtsSyncCount() (localFtsSyncCount = ++*ftsSyncCount)

/* get fts share memory size */
int
FtsShmemSize(void)
{
	return MAXALIGN(sizeof(FtsControlBlock));
}


void
FtsShmemInit(void)
{
	bool		found;
	FtsControlBlock *shared;

	shared = (FtsControlBlock *) ShmemInitStruct("Fault Tolerance manager", FtsShmemSize(), &found);
	if (!shared)
		elog(FATAL, "FTS: could not initialize fault tolerance manager share memory");

	/* Initialize locks and shared memory area */

	ftsEnabled = &shared->ftsEnabled;
	ftsControlLock = shared->ControlLock;
	ftsSyncCount = &shared->ftsSyncCount;
	ftsAction = &shared->ftsAction;
	ftsReadOnlyFlag = &shared->ftsReadOnlyFlag;

	ftsQDMirrorLock = shared->ftsQDMirrorLock;
	ftsQDMirrorUpdateConfigLock = shared->ftsQDMirrorUpdateConfigLock;
	ftsQDMirrorInfo = &shared->ftsQDMirror;
	ftsSegDBTransientState = shared->ftsSegDBTransientState;
	ftsSegDBTransientCount = &shared->ftsSegDBTransientCount;

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
		shared->ftsSyncCount = 0;
		shared->ftsSegDBTransientCount = 0;
		shared->ftsReadOnlyFlag = gp_set_read_only;

		if (Gp_role == GP_ROLE_DISPATCH)
		{
			shared->ftsAction = convertFaultActionValue(gp_fault_action_string);
			elog(LOG, "FTS: Fault Tolerance Action is set to : %s", gp_fault_action_string);
		}
		shared->ftsEnabled = (*ftsAction != FTS_NONE);
	}
}

/*
 * Set System Read Only
 */
void
CondSetMPPReadonly(void)
{
	if (*ftsAction == FTS_READONLY)
	{
		elog(LOG, "FTS: Greenplum system is set read only");
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "Count of DTX transactions between 'PREPARING' and confirmed 'COMMIT' or 'ABORT' is %d",
			 dtxCurrentPhase1Count());

		assign_gp_set_read_only(true, true, PGC_S_DEFAULT);
	}
}

/*
 * Re-Connect segDB
 */
bool
FtsReConnectDB(void *failedDB)
{
	return reconnectSegDB(failedDB);
}

/*
 * Test-Connection: This is called from the threaded context inside the
 * dispatcher: ONLY CALL THREADSAFE FUNCTIONS -- elog() is NOT threadsafe.
 */
bool
FtsTestConnection(CdbComponentDatabaseInfo *failedDBInfo)
{
   	PGconn		*conn;
	bool		ret = true;
	PQExpBuffer	buffer = createPQExpBuffer();
	char		*errorMsg;

	/* XXX: this connects in UTILITY mode, are UTILITY mode errors
	 * different from EXECUTE mode ? */
	initPQConnectionBuffer(failedDBInfo, MyProcPort->user_name, MyProcPort->database_name, buffer, true);

	conn = PQconnectdb(buffer->data);

	do
	{
		if (PQstatus(conn) == CONNECTION_OK)
		{
			ret = true;
			break;
		} 
		else if (PQstatus(conn) != CONNECTION_BAD)
		{
			if (LOG >= log_min_messages)
			{
				/* Don't use elog, it's not thread-safe */
				write_log("FTS: test connection status unknown, dbid %d dbname %s error message %s (%d)",
							 failedDBInfo->dbid, MyProcPort->database_name, PQerrorMessage(conn), PQstatus(conn));
			}
			ret = true;
			break;
		}

		errorMsg = PQerrorMessage(conn);
		if (errorMsg == NULL)
		{
			if (LOG >= log_min_messages)
				write_log("FTS: test connection can't parse error message");
		}

		if (DEBUG3 >= log_min_messages)
			write_log("FtsTestConnection: connection failed with message (%s)", errorMsg);

		/* If we get a network error (do we really need a
		 * comprehensive list here ?) or a "FATAL" database error,
		 * then it is a real error, otherwise it isn't. See
		 * fe-connect.c for the details: all fatal network error start
		 * with "could not":
		 *
		 * "could not create" -- socket error
		 * "could not translate" -- DNS error
		 * "could not connect" -- network error
		 * "could not get" -- getsockopt error
		 * "could not set" -- setsockopt error
		 * "could not send" -- libpq protocol error
		 */
		if (strstr(errorMsg, "FATAL:") == NULL &&
			strstr(errorMsg, "could not ") == NULL &&
			strcmp(errorMsg, "timeout expired\n") != 0)
		{
			/* non-fatal connection errors are ignored */
			if (DEBUG3 >= log_min_messages)
				write_log("FtsTestConnection: nonfatal error, ignoring (%d, %s)", PQsocket(conn), errorMsg);
			ret = true;
			break;
		}

		if (strstr(errorMsg, "too many clients already") != NULL)
		{
			/* don't consider this a failure, essentially we want to force a retry here */
			ret = true;
			break;
		}

		if (strstr(errorMsg, "the database system is starting up") != NULL)
		{
			/* don't consider this a failure, essentially we want to force a retry here */
			ret = true;
			break;
		}

		/* This is a real failure for which a reconfigure may be warranted */
		if (LOG >= log_min_messages)
			write_log("FTS: test connection failed, dbid %d dbname %s error message %s",
						 failedDBInfo->dbid, MyProcPort->database_name, PQerrorMessage(conn));
		ret = false;
	} while (0);

	PQfinish(conn);
	free(buffer->data);
	return ret;
}

/*
 * Re-Configure the system
 */
bool
FtsReConfigureMPP(void)
{
	/* synchronzie fts */
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	InReConfigure = true;

	/* this code copied from verifyFtsSyncCount to avoid lock deadlock */
	if (localFtsSyncCount == -1)
	{
		localFtsSyncCount = *ftsSyncCount;
	}
	else if (localFtsSyncCount != *ftsSyncCount)
	{
		localFtsSyncCount = *ftsSyncCount;
	}
	else
	{
		incFtsSyncCount();
		detectFailedConnections();
	}

	elog(LOG, "FTS: reconfiguration is in progress");
	disconnectAndDestroyAllGangs();

	allocateWriterGang(GANGTYPE_PRIMARY_WRITER);
	allocateWriterGang(GANGTYPE_MIRROR_WRITER);

	InReConfigure = false;
	LWLockRelease(ftsControlLock);

	/* rollback current DTM transaction */
	AbortCurrentTransaction();

	return true;
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
				 "QD mirroring already disabled");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Unexpected QD Mirror state '%s'",
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

    for (i = 0; i < size; i++)
    {
        if (PQstatus((segdbDesc + i)->conn) != CONNECTION_OK)
        {
			elog(DEBUG2, "FtsHandleGangConnectionFailure: looking for real fault on segment %d", i);
            if (!FtsTestConnection((segdbDesc + i)->segment_database_info))
			{
				elog(DEBUG2, "found fault with segment %d", i);
				realFaultFound = true; 

				/* that at least one fault exists is enough, for now */
				break;
			}
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

	/* set MPP to be read only if necessary */
	CondSetMPPReadonly();

	elog(LOG, "FTS: reconfiguration is in progress");
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	incFtsSyncCount();
	for (i = 0; i < size; i++)
	{
		if (PQstatus((segdbDesc + i)->conn) != CONNECTION_OK &&
			!FtsTestConnection((segdbDesc + i)->segment_database_info))
			markSegDBStatus((segdbDesc + i)->segment_database_info->dbid, false);
	}
	if (gangsExist())
	{
		reportError = true;
		disconnectAndDestroyAllGangs();
	}

	LWLockRelease(ftsControlLock);

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
		elog((Debug_print_full_dtm ? LOG : DEBUG5), "FtsHandleGangConnectionFailure found an active DTM transaction (returning true).");
		return true;
	}

	/*
	 * error out if this sets read only flag, at this stage the read only
	 * transaction checking has passed, so error out, but do not error out if
	 * tm is in recovery
	 */
	if ((*ftsReadOnlyFlag && !isTMInRecovery()) || reportError)
		return true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5), "FtsHandleGangConnectionFailure returning false.");

	return false;
}

/*
 * If calling from a thread it is illegal to leave username or dbName NULL.
 */
static void
initPQConnectionBuffer(CdbComponentDatabaseInfo *q, char *username, char *dbName, PQExpBuffer buffer, bool utility)
{
	char *user = username;

	if (user == NULL)
		user = MyProcPort->user_name;


	initPQExpBuffer(buffer);

	if (dbName == NULL)
		dbName = MyProcPort->database_name;
	
	/*
	 * Build the connection string
	 */
	if (utility)
		appendPQExpBuffer(buffer,
						  "options='-c gp_session_role=UTILITY' ");

	if (q->hostname == NULL)
		appendPQExpBufferStr(buffer, "host='' ");
	else if (isdigit(q->hostname[0]))
		appendPQExpBuffer(buffer, "hostaddr=%s ", q->hostname);
	else
		appendPQExpBuffer(buffer, "host=%s ", q->hostname);

	appendPQExpBuffer(buffer, "port=%u ", q->port);
	appendPQExpBuffer(buffer, "dbname=%s ", dbName);
	appendPQExpBuffer(buffer, "user=%s ", user);
	appendPQExpBuffer(buffer, "connect_timeout=%d ", gp_segment_connect_timeout);
}

/* set seg db states */
void
markStandbyStatus(int dbid, bool state)
{
	markSegDBPersistentState(dbid, state);
}

/* set seg db states */
void
markSegDBStatus(int dbid, bool state)
{
	if (*ftsAction == FTS_READONLY)
		markSegDBTransientState(dbid, state);
	else
		markSegDBPersistentState(dbid, state);
}

/* set segdb state transiently */
static void
markSegDBTransientState(int dbid, bool state)
{
	int			i,
		segindex;
	CdbComponentDatabaseInfo *segDb = NULL;
	CdbComponentDatabases *cdb_component_dbs = NULL;

	/*
	 * this checking should be taken of care by synchronization, but check again
	 * for safety, may be removed in the future
	 */
	if (state)
	{
		/* we do not support automatic "revalidation" of segments */
		elog(ERROR, "FTS: caller requesting automatic reset of segment to 'valid': operation unsupported.");
	}
	else
	{
		/* set transient state if state is false */

		if (*ftsSegDBTransientCount >= FTS_MAX_TRANSIENT_STATE)
			elog(ERROR, "FTS: the number of failed segdb exceeds the GP limit");

		cdb_component_dbs = getComponentDatabases();
		ftsSegDBTransientState[*ftsSegDBTransientCount].valid = false;
		ftsSegDBTransientState[*ftsSegDBTransientCount].id = dbid;

		elog(LOG, "FTS: mark segDB %d as invalid", dbid);

		(*ftsSegDBTransientCount)++;
		for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			segDb = cdb_component_dbs->segment_db_info + i;
			if (segDb->dbid == dbid)
				break;
		}

		/* assert on segdb not found */
		assert(i != cdb_component_dbs->total_segment_dbs);

		if (!segDb->isprimary)
		{
			/* we are done */
			return;
		}

		/* need to change isPrimary flag */
		segindex = segDb->segindex;
		for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			segDb = cdb_component_dbs->segment_db_info + i;
			if (segDb->segindex == segindex && segDb->dbid != dbid)
				break;
		}

		/* assert on segdb not found */
		assert(i != cdb_component_dbs->total_segment_dbs);

		if (segDb->valid && !segDb->isprimary)
		{
			ftsSegDBTransientState[*ftsSegDBTransientCount].primary = true;
			ftsSegDBTransientState[*ftsSegDBTransientCount].valid = true;
			ftsSegDBTransientState[*ftsSegDBTransientCount].id = segDb->dbid;

			if (*ftsSegDBTransientCount >= FTS_MAX_TRANSIENT_STATE)
				elog(ERROR, "FTS: the number of failed segdb exceeds the GP limit");

			(*ftsSegDBTransientCount)++;
		}
		return;
	}
}

/* 
 * Set persistent segdb state: we MUST NOT be called from anything
 * other than our main thread. (SPI isn't thread safe)
 */
static void
markSegDBPersistentState(int dbid, bool state)
{
	TupleDesc tupdesc;
	char *tmp, cmd[200];
	int contentid, hasmirror=0;
	int partnerid = 0, partnervalid = 0, partnerdefinedprimary, 
			partnerisprimary;

	int			count;
	PGresult   *rs;
	CdbComponentDatabaseInfo *q;

	char		*user;

	PQExpBuffer entryBuffer = NULL;
	PGconn *entryConn = NULL;

	if (state)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: can only automatically invalidate segments")));
	}

	if (SPI_OK_CONNECT != SPI_connect())
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SPI_connect failed in FTS")));
	}

	/* We're going to use the content id for some sanity checking, and
	 * to retrieve the "partner" database. We could get the partner
	 * database directly: 
	 * select dbid, valid, isprimary, definedprimary from gp_configuration
	 *   where content = 
	 *			(select content from gp_configuration where dbid = 2)
	 *			and dbid != 2;
	 * but the sanity checks seem worthwhile */
	if (snprintf(cmd, sizeof(cmd),
				 "select content from pg_catalog.gp_configuration where dbid=CAST(%d AS SMALLINT)",
				 dbid) >= sizeof(cmd))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: could not format command string (get dbid)")));
	}

	if (SPI_OK_SELECT != SPI_execute(cmd, false, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: SPI_execute failed (%s)", cmd)));
	}

	if (SPI_processed != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: found %d rows for dbid %d", SPI_processed, dbid)));
	}

	tupdesc = SPI_tuptable->tupdesc;
	contentid = atoi(SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 1));

	if (contentid < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: data mirror cannot modify qd-mirror attributes")));
	}

	if (snprintf(cmd, sizeof(cmd),
				 "select dbid, isprimary, valid, definedprimary from pg_catalog.gp_configuration where content = %d and dbid != CAST(%d AS SMALLINT)",
				 contentid, dbid) >= sizeof(cmd))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: could not format command string (get partners)")));
	}

	if (SPI_OK_SELECT != SPI_execute(cmd, false, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: SPI_execute failed (%s)", cmd)));
	}

	/* again we should have exactly one row (or zero if no mirror exists) */
	if (SPI_processed > 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: found %d rows for content %d dbid != %d",
						SPI_processed, contentid, dbid)));
	}

	hasmirror = SPI_processed;

	if (hasmirror)
	{
		tupdesc = SPI_tuptable->tupdesc;
		partnerid = atoi(SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 1));
		if (partnerid < 1)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("FTS: invalid partner id for db %d", dbid)));
		}

		tmp = SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 2);
		partnerisprimary = (tmp[0] == 't') ? 1 : 0;

		tmp = SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 3);
		partnervalid = (tmp[0] == 't') ? 1 : 0;

		tmp = SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 4);
		partnerdefinedprimary = (tmp[0] == 't') ? 1 : 0;
	}

	/* As a final step: we need to re-connect with privileges which
	 * allow catalog updates */
	if (snprintf(cmd, sizeof(cmd),
				 "select rolname from pg_catalog.pg_roles where rolcatupdate ='t' and rolsuper = 't' limit 1;") >= sizeof(cmd))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: could not format command string (get rolname)")));
	}

	if (SPI_OK_SELECT != SPI_execute(cmd, false, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: SPI_execute failed (%s)", cmd)));
	}

	if (SPI_processed != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: found %d rows with limit 1!", SPI_processed)));
	}

	tupdesc = SPI_tuptable->tupdesc;
	user = strdup(SPI_getvalue(SPI_tuptable->vals[0], tupdesc, 1));
	if(!user)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("FTS: out of memory")));

	SPI_finish();

	/* We now have the full row of the partner db, all of the
	 * information that we need to update out catalog */

	/* We can't use the SPI interface to update the database
	 * persistently (we got here because a transaction is in the
	 * middle of ABORTing.
	 * 
	 * The hack we use is to establish a utility mode connection to
	 * ourselves to fix things up. */

	q = getEntrySegDB(&count);

	if (count > 1)
		if (q->dbid != GpIdentity.dbid)
			q = q + 1;

	entryBuffer = createPQExpBuffer();

	/*
	 * initialize libpq connection buffer, we only need to initialize it
	 * once.
	 */
	initPQConnectionBuffer(q, user, NULL, entryBuffer, true);

	free(user);

	if (hasmirror && !partnervalid)
	{
		elog(LOG, "FTS: mirror partner is already invalid.");
		return;
	}

	/*
	 * Call libpq to connect
	 */
	entryConn = PQconnectdb(entryBuffer->data);

	if (PQstatus(entryConn) == CONNECTION_BAD)
	{
		/*
		 * When we get an error, we strdup it here.  When the main thread
		 * checks for errors, it makes a palloc copy of this, and frees
		 * this.
		 */
		char	   *error_message = strdup(PQerrorMessage(entryConn));
		if(!error_message)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
					errmsg("FTS: out of memory")));

		free(entryBuffer->data);
		PQfinish(entryConn);
		entryConn = NULL;
		elog(FATAL, "FTS: setting segDB state failed, error connecting to entry db, error: %s", error_message);
	}

	/* finally, we're ready to actually get some stuff done. */

	do {
		rs = PQexec(entryConn, "BEGIN");
		if (PQresultStatus(rs) != PGRES_COMMAND_OK)
			break;

		elog(DEBUG1, "marking dbid %d as invalid for contentid %d", dbid, contentid);

		/* mark failed db as 'invalid' */
		if (snprintf(cmd, sizeof(cmd),
					 "update gp_configuration set valid='f', isprimary='f' where dbid = CAST(%d AS SMALLINT)",
					 dbid) >= sizeof(cmd))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("FTS: could not invalidate invalid node in config: format command string failure")));
			break;
		}

		rs = PQexec(entryConn, cmd);
		if (PQresultStatus(rs) != PGRES_COMMAND_OK)
			break;

		/* Mark mirror db 'primary' */
		if (hasmirror && partnervalid)
		{
			elog(DEBUG1, "marking mirror partner (%d) as primary for contentid %d", partnerid, contentid);

			if (snprintf(cmd, sizeof(cmd),
						 "update gp_configuration set isprimary='t' where dbid = CAST(%d AS SMALLINT)",
						 partnerid) >= sizeof(cmd))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("FTS: could not format command string")));
				break;
			}

			rs = PQexec(entryConn, cmd);
			if (PQresultStatus(rs) != PGRES_COMMAND_OK)
				break;
		}

		rs = PQexec(entryConn, "COMMIT");
		if (PQresultStatus(rs) != PGRES_COMMAND_OK)
			break;

		/* No error, we're ready to go */

		elog(LOG, "FTS: mark segDB %d as %s", dbid, state ? "valid" : "invalid");
		PQclear(rs);
		PQfinish(entryConn);
		free(entryBuffer->data);
		return;
	} while (0);

	/* Something horrible happened while marking our database ? */

	PQclear(rs);
	PQfinish(entryConn);
	free(entryBuffer->data);

	elog(FATAL, "FTS: setting segDB state failed");
}

/*
 * Show hook routine for "gp_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_gp_set_read_only(void)
{
	return *ftsReadOnlyFlag ? "TRUE" : "FALSE";
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
 * Set Read only
 */
bool
assign_gp_set_read_only(bool newval, bool doit, GucSource source __attribute__((unused)) )
{

#if FALSE
	elog(DEBUG1, "gp_set_read_only: %s, doit=%s",
		 newval, (doit ? "true" : "false"));
#endif

	if (ftsReadOnlyFlag == NULL || newval == *ftsReadOnlyFlag)
		return true;

	if (doit)
	{
		*ftsReadOnlyFlag = newval;

		/* set current transaction read only */
		XactReadOnly = newval;


		/*
		if (Gp_role == GP_ROLE_DISPATCH)
		{
			char cmdbuf[30];
			strcpy( cmdbuf, "set gp_set_read_only=" );
			strcat( cmdbuf, newval?"true":"false");

			if ( !CdbDoCommandNoTxn(cmdbuf, false, NULL) )
				elog(PANIC, "gp_set_read_only failed on QE");
		}
		*/
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
	bool		ret = true;

	/* synchronize fts */
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

	if (localFtsSyncCount == -1)
		localFtsSyncCount = *ftsSyncCount;
	else if (localFtsSyncCount != *ftsSyncCount)
	{
		elog(LOG, "FTS: detects re-configuration required");
		ret = false;
	}

	LWLockRelease(ftsControlLock);
	return ret;
}

/* apply transient state to segdb state */
CdbComponentDatabases *
applyTransientState(CdbComponentDatabases *cdb_component_dbs)
{
	int			i,
				j,
				dbid;
	CdbComponentDatabaseInfo *segDb;

	/* do not synchronize here, it may cause sync deadlock */
	/*
	  LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	*/

	for (i = 0; i < *ftsSegDBTransientCount; i++)
	{
		dbid = ftsSegDBTransientState[i].id;

		for (j = 0; j < cdb_component_dbs->total_segment_dbs; j++)
		{
			segDb = cdb_component_dbs->segment_db_info + j;
			if (segDb->dbid == dbid)
			{
				segDb->valid = ftsSegDBTransientState[i].valid;
				segDb->isprimary = ftsSegDBTransientState[i].primary;
				break;
			}
		}
	}

	/*
	  LWLockRelease(ftsControlLock);
	*/
	return cdb_component_dbs;
}

/* re-test transient failed state */
void
FtsEvaluateTransientState(void)
{
	int			i,
				j,
				dbid;
	CdbComponentDatabases *cdb_component_dbs;
	CdbComponentDatabaseInfo *segDbInfo;

	if (*ftsSegDBTransientCount <= 0 || InReConfigure)
		return;

	cdb_component_dbs = getComponentDatabases();

	/* synchronize fts */
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

	for (i = 0; i < *ftsSegDBTransientCount; i++)
	{
		if (!ftsSegDBTransientState[i].valid)
		{
			dbid = ftsSegDBTransientState[i].id;
			for (j = 0; j < cdb_component_dbs->total_segment_dbs; j++)
			{
				segDbInfo = cdb_component_dbs->segment_db_info + j;
				if (segDbInfo->dbid == dbid)
				{
					if (!FtsTestConnection(segDbInfo))
					{
						/* reconnect failed, keep current state */
						LWLockRelease(ftsControlLock);
						return;
					}
					break;
				}
			}
		}
	}

	/* retest ok, reset all states */
	MemSet(ftsSegDBTransientState, 0, *ftsSegDBTransientCount * sizeof(struct FtsSegDBState));
	*ftsSegDBTransientCount = 0;

	incFtsSyncCount();
	assign_gp_set_read_only(false, true, PGC_S_DEFAULT);
	setRecoverTMOn();

	LWLockRelease(ftsControlLock);
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
		elog(ERROR, "hostname too long for fts QD mirror configuration storage");
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
				 "QD mirroring already disabled (connecting)");
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
				 "QD mirroring already disabled (request PositionToEnd -- could not send to WAL Send server)");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			elog(ERROR,"Unexpected QD Mirror state '%s'",
				 QDMirroringStateToString(state));
		}
	}
	ftsQDMirrorInfo->state = QDMIRROR_STATE_CATCHUPPENDING;
	LWLockRelease(ftsQDMirrorLock);
	elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
		 "positioning QD mirror to end complete -- next step is catch-up");
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
				 "QD mirroring already disabled (request Catchup -- could not send to WAL Send server)");
			return;
		}
		else
		{
			QDMIRRORState state = ftsQDMirrorInfo->state;
			
			LWLockRelease(ftsQDMirrorLock);
			elog(ERROR,"Unexpected QD Mirror state '%s'",
				 QDMirroringStateToString(state));
		}
	}
	
	LWLockRelease(ftsQDMirrorLock);
	elog((Debug_print_qd_mirroring ? LOG : DEBUG5),
		 "finished catching up QD mirror");
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
		elog(LOG, "QD mirroring not enabled");
		return;
	}

	elog(LOG, "QD mirror active name (%s:%d), port %d",
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
