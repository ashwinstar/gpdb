/*-------------------------------------------------------------------------
 *
 * cdbfts.c
 *	  Provides fault tolerance service routines for mpp.
 *
 * Copyright (c) 2003-2006, Greenplum
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

#include "executor/spi.h"

bool	   *ftsEnabled;
static LWLockId ftsControlLock;
static int *ftsSyncCount;
int		   *ftsAction;
static bool *ftsReadOnlyFlag;

static struct FtsSegDBState *ftsSegDBTransientState;
static int *ftsSegDBTransientCount;

static int	localFtsSyncCount = -1;
static bool InReConfigure = false;

static struct FtsQDMirrorInfo *ftsQDMirrorInfo=NULL;

static void CdbShutdown(void);
static void initPQConnectionBuffer(CdbComponentDatabaseInfo *, char *, char *, PQExpBuffer, bool);
static void markSegDBTransientState(int, bool);
static void markSegDBPersistentState(int, bool);

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

	ftsQDMirrorInfo = &shared->ftsQDMirror;
	ftsSegDBTransientState = shared->ftsSegDBTransientState;
	ftsSegDBTransientCount = &shared->ftsSegDBTransientCount;

	if (!IsUnderPostmaster)
	{
		shared->ControlLock = LWLockAssign();
		ftsControlLock = shared->ControlLock;

		ftsQDMirrorInfo->configurationChecked = false;
		ftsQDMirrorInfo->state = QDMIRROR_STATE_NONE;
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

	allocatePrimaryWriterGang();
	allocateMirrorWriterGang();

	/* rollback current DTM transaction */
	AbortCurrentTransaction();

	InReConfigure = false;
	LWLockRelease(ftsControlLock);

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

void
disableQDMirroring(void)
{
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
	LWLockRelease(ftsControlLock);
	
	elog(LOG, "FTS: QD mirroring disabled");
}

bool
isQDMirroringEnabled(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_ENABLED);
	LWLockRelease(ftsControlLock);

	return result;
}

bool
isQDMirroringCatchingUp(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_CATCHINGUP);
	LWLockRelease(ftsControlLock);

	return result;
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
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	result = ftsQDMirrorInfo->configurationChecked;
	LWLockRelease(ftsControlLock);

	return result;
}

bool
isQDMirroringPendingCatchup(void)
{
	bool result;
	
	if (ftsQDMirrorInfo == NULL)
		return false;
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	result = (ftsQDMirrorInfo->state == QDMIRROR_STATE_CATCHUPPENDING);
	LWLockRelease(ftsControlLock);

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
	int serialNumber;
	bool serverAcceptingNewRequests;

	Assert(dbinfo != NULL);

	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->configurationChecked)
	{
		// Someone else finished the configuration check work first.
		LWLockRelease(ftsControlLock);
		return;
	}

	Assert(ftsQDMirrorInfo->state == QDMIRROR_STATE_NONE);

	len = strlen(dbinfo->hostname);
	if (len >= sizeof(ftsQDMirrorInfo->name))
	{
		LWLockRelease(ftsControlLock);
		elog(ERROR, "hostname too long for fts QD mirror configuration storage");
	}
	memcpy(ftsQDMirrorInfo->name, dbinfo->hostname, len);
	ftsQDMirrorInfo->name[len] = 0;
	ftsQDMirrorInfo->port = dbinfo->port;

	/*
	 * Tell the WalSendServer (queue while under lock) that it can
	 * connect to the standby and position to the end of its WAL
	 * 
	 * We do NOT wait for a response while under the lock.  We'll pick
	 * up the response at the end of this procedure.
	 */
	ftsQDMirrorInfo->configurationChecked = true;
	ftsQDMirrorInfo->state = QDMIRROR_STATE_POSITIONINGTOEND;

	serverAcceptingNewRequests = XLogQDMirrorQueuePositionToEnd(&serialNumber);
	if (!serverAcceptingNewRequests)
	{
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "WAL Send server not accepting new requests!");

		ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
		LWLockRelease(ftsControlLock);
		return;
	}
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"positioning standby master WAL to end");
	LWLockRelease(ftsControlLock);

	/*
	 * Wait (without holding lock) for the position operation to complete.
	 */
	serverAcceptingNewRequests = XLogQDMirrorWaitForResponse(serialNumber);
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	if (!serverAcceptingNewRequests)
	{
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "WAL Send server not accepting new requests!");

		ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
		LWLockRelease(ftsControlLock);
		return;
	}

	ftsQDMirrorInfo->state = QDMIRROR_STATE_CATCHUPPENDING;
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"positioning QD mirror to end complete -- next step is catch-up");
	LWLockRelease(ftsControlLock);
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
	bool serverAcceptingNewRequests;
	int serialNumber;

	Assert(flushedLocation != NULL);

	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

	if (ftsQDMirrorInfo->state != QDMIRROR_STATE_CATCHUPPENDING)
	{
		LWLockRelease(ftsControlLock);
		return;
	}
	
	serverAcceptingNewRequests = XLogQDMirrorQueueCatchup(flushedLocation, &serialNumber);
	if (!serverAcceptingNewRequests)
	{
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "WAL Send server not accepting new requests!");

		ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
		LWLockRelease(ftsControlLock);
		return;
	}
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"catching up QD mirror");

	Assert(ftsQDMirrorInfo->state = QDMIRROR_STATE_CATCHINGUP);
	LWLockRelease(ftsControlLock);
	
	/*
	 * Wait (without holding lock) for the catchup operation to complete.
	 */
	serverAcceptingNewRequests = XLogQDMirrorWaitForResponse(serialNumber);
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	if (!serverAcceptingNewRequests)
	{
		elog((Debug_print_qd_mirroring ? LOG : DEBUG1), "WAL Send server not accepting new requests!");

		ftsQDMirrorInfo->state = QDMIRROR_STATE_DISABLED;
		LWLockRelease(ftsControlLock);
		return;
	}

	ftsQDMirrorInfo->state = QDMIRROR_STATE_ENABLED;
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"finished catching up QD mirror");
	LWLockRelease(ftsControlLock);
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
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

	if(ftsQDMirrorInfo->state != QDMIRROR_STATE_ENABLED)
	{
		LWLockRelease(ftsControlLock);
		return;
	}

	if (ftsQDMirrorInfo->haveNewCheckpointLocation &&
		XLByteLT(*newCheckpointLocation, ftsQDMirrorInfo->newCheckpointLocation))
	{
		LWLockRelease(ftsControlLock);
		return;
	}

	ftsQDMirrorInfo->haveNewCheckpointLocation = true;
	ftsQDMirrorInfo->newCheckpointLocation = *newCheckpointLocation;
	
	LWLockRelease(ftsControlLock);
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
	
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

	if(ftsQDMirrorInfo->state != QDMIRROR_STATE_ENABLED)
	{
		LWLockRelease(ftsControlLock);
		
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
	
	LWLockRelease(ftsControlLock);
	
	*ready = true;
}

/*
 * While checking the mirroring configuration, call this procedure to set
 * that we are NOT going do mirroring.
 */
void
FtsSetNoQDMirroring(void)
{
	LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
	if (ftsQDMirrorInfo->configurationChecked)
	{
		// Someone else finished the configuration check work first.
		LWLockRelease(ftsControlLock);
		return;
	}

	Assert(ftsQDMirrorInfo->state == QDMIRROR_STATE_NONE);

	ftsQDMirrorInfo->configurationChecked = true;
	ftsQDMirrorInfo->state = QDMIRROR_STATE_NOTCONFIGURED;
	
	LWLockRelease(ftsControlLock);
	elog((Debug_print_qd_mirroring ? LOG : DEBUG1),"set QD mirroring to disabled");
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
		 ftsQDMirrorInfo->name, (int)strlen(ftsQDMirrorInfo->name), ftsQDMirrorInfo->port);
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
