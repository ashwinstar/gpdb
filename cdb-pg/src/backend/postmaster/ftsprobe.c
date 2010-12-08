/*-------------------------------------------------------------------------
 *
 * ftsprobe.c
 *	  Process under QD postmaster polls the segments on a periodic basis
 * or at the behest of QEs.
 *
 * Maintains a bitmap in shared memory of the up-ness of each segment.
 *
 * Copyright (c) 2006-2011, Greenplum inc
 *
 * $Id: //cdb2/private/tkordas/fts_cleanup/cdb-pg/src/backend/postmaster/ftsprobe.c#1 $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <pthread.h>
#include <limits.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"
#include "utils/memutils.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"		/* gp_pthread_create */
#include "commands/sequence.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "postmaster/ftsprobe.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/pmsignal.h"			/* PostmasterIsAlive */
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/ps_status.h"

#include "utils/builtins.h"

#include "utils/relcache.h"
#include "utils/syscache.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_filespace_entry.h"
#include "catalog/pg_tablespace.h"
#include "catalog/catalog.h"

#include "catalog/gp_san_config.h"
#include "catalog/gp_segment_config.h"

#include "storage/backendid.h"

#include "pgtime.h"

#include "executor/spi.h"
#include "postmaster/primary_mirror_mode.h"

#include "cdb/ml_ipc.h" /* gettime_elapsed_ms */

#include "tcop/tcopprot.h" /* quickdie() */

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

/*
 * CONSTANTS
 */

/* maximum number of segments */
#define MAX_NUM_OF_SEGMENTS  32768

/* buffer size for timestamp */
#define TIMESTAMP_BUF_SIZE   128

/* buffer size for SQL command */
#define SQL_CMD_BUF_SIZE     1024

#define GpConfigHistoryRelationName    "gp_configuration_history"

#define MAX_PROBE_RESPONSE_LEN	20

/*
 * Enum for probe result deciphering - use one byte to store results
 */
enum probe_result_e
{
	PROBE_DEAD            = 0x00,
	PROBE_ALIVE           = 0x01,
	PROBE_FAULT_CRASH     = 0x02,
	PROBE_FAULT_MIRROR    = 0x04,
	PROBE_RESYNC_COMPLETE = 0x08,
	PROBE_SEGMENT         = 0x10,
};

#define PROBE_CHECK_FLAG(result, flag) ((result & flag) == flag)

#define PROBE_IS_ALIVE(dbInfo) \
	PROBE_CHECK_FLAG(probe_results[(dbInfo)->dbid], PROBE_ALIVE)
#define PROBE_HAS_FAULT_CRASH(dbInfo) \
	PROBE_CHECK_FLAG(probe_results[(dbInfo)->dbid], PROBE_FAULT_CRASH)
#define PROBE_HAS_FAULT_MIRROR(dbInfo) \
	PROBE_CHECK_FLAG(probe_results[(dbInfo)->dbid], PROBE_FAULT_MIRROR)
#define PROBE_IS_RESYNC_COMPLETE(dbInfo) \
	PROBE_CHECK_FLAG(probe_results[(dbInfo)->dbid], PROBE_RESYNC_COMPLETE)

/* segment probing state */
enum probe_state_t
{
	CONNECTING = 0,
	CONNECTED,
	REQUEST_SENT,
	RESPONSE_READY
};

/*
 * primary/mirror state after probing;
 * this is used to transition the segment pair to next state;
 * we ignore the case where both segments are down, as no transition is performed;
 * each segment can be:
 *    1) (U)p or (D)own
 */
enum probe_transition_e
{
	TRANS_D_D = 0x01,	/* not used for state transitions */
	TRANS_D_U = 0x02,
	TRANS_U_D = 0x04,
	TRANS_U_U = 0x08,

	TRANS_SENTINEL
};

#define IS_VALID_TRANSITION(trans) \
	(trans == TRANS_D_D || trans == TRANS_D_U || trans == TRANS_U_D || trans == TRANS_U_U)


/*
 * primary/mirror valid states for filerep;
 * we enumerate all possible combined states for two segments
 * before and after state transition, assuming that the first
 * is the old primary and the second is the old mirror;
 * each segment can be:
 *    1) (P)rimary or (M)irror
 *    2) (U)p or (D)own
 *    3) in (S)ync, (R)esync or (C)hangetracking mode -- see filerep's DataState_e
 */
enum filerep_segment_pair_state_e
{
	FILEREP_PUS_MUS = 0,
	FILEREP_PUR_MUR,
	FILEREP_PUC_MDX,
	FILEREP_MDX_PUC,

	FILEREP_SENTINEL
};

 /* we always assume that primary is up */
#define IS_VALID_OLD_STATE_FILEREP(state) \
	(state >= FILEREP_PUS_MUS && state <= FILEREP_PUC_MDX)

#define IS_VALID_NEW_STATE_FILEREP(state) \
	(state >= FILEREP_PUS_MUS && state < FILEREP_SENTINEL)

/*
 * state machine matrix for filerep;
 * we assume that the first segment is the old primary;
 * transitions from "down" to "resync" and from "resync" to "sync" are excluded;
 */
const uint32 state_machine_filerep[][FILEREP_SENTINEL] =
{
/* new: PUS_MUS,   PUR_MUR,               PUC_MDX,   MDX_PUC */
	{         0,         0,             TRANS_U_D, TRANS_D_U },  /* old: PUS_MUS */
	{         0,         0, TRANS_U_D | TRANS_D_U,         0 },  /* old: PUR_MUR */
	{         0,         0,                     0,         0 },  /* old: PUC_MDX */
};


/*
 * primary/mirror valid states for SAN;
 * we enumerate all possible combined states for two segments
 * before and after state transition, assuming that the first
 * is the old primary and the second is the old mirror;
 * each segment can be:
 *    1) (P)rimary or (M)irror
 *    2) (U)p or (D)own
 */
enum san_segment_pair_state_e
{
	SAN_PU_MU = 0,
	SAN_PU_MD,
	SAN_MD_PU,

	SAN_SENTINEL
};

#define IS_VALID_OLD_STATE_SAN(state) \
	(state >= SAN_PU_MU && state <= SAN_PU_MD)

#define IS_VALID_NEW_STATE_SAN(state) \
	(state >= SAN_PU_MU && state < SAN_SENTINEL)

/*
 * state machine matrix for SAN;
 * we assume that the first segment is the old primary
 * transition from "down" to "up" is excluded;
 */
const uint32 state_machine_san[][SAN_SENTINEL] =
{
/* new:   PU_MU,     PU_MD,     MD_PU */
	{         0, TRANS_U_D, TRANS_D_U },  /* old: PU_MU */
	{         0,         0,         0 },  /* old: PU_MD */
};


/*
 * STRUCTURES
 */

struct segmentStatusChange {
	int	dbid;
	int16 segindex;
	uint8 oldStatus;
	uint8 newStatus;
};

struct threadWorkerInfo {
	uint8		*scan_status;
};


/*
 * STATIC VARIABLES
 */

/* one byte of status for each segment */
static uint8 scan_status[MAX_NUM_OF_SEGMENTS];

static bool am_ftsprobe = false;

static volatile bool shutdown_requested = false;
static volatile bool rescan_requested = false;

static char *probeUser = NULL;
static char *probeDatabase = "postgres";

static char failover_strategy='n';

/* struct holding segment configuration */
static CdbComponentDatabases *cdb_component_dbs = NULL;

/* mutex used for pthread synchronization in parallel probing */
static pthread_mutex_t worker_thread_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * FUNCTION PROTOTYPES
 */

#ifdef EXEC_BACKEND
static pid_t ftsprobe_forkexec(void);
#endif
NON_EXEC_STATIC void FtsProbeMain(int argc, char *argv[]);

static void FtsProbeLoop(void);

static void markSegmentInSyncInConfiguration(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror);
static char FtsTestOneSegment(CdbComponentDatabaseInfo *dbInfo);

static void retrieveUserAndDb(char **probeUser);
static void executeEntrySQL(PGconn *entryConn, const char *query);

static bool probeSegments(void);

static void *probeSegmentFromThread(void *arg);

extern bool FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace);

static void readCdbComponentInfoAndUpdateStatus(void);
static bool probePublishUpdate(uint8 *scan_status);
static CdbComponentDatabaseInfo *getMirrorEntryByContent(int content, int dbid);

static uint32 getTransition(bool isPrimaryAlive, bool isMirrorAlive);
static uint32 getPairStateFilerep(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror);
static uint32 getPairStateSAN(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror);

static void
buildSegmentStateChange(CdbComponentDatabaseInfo *segInfo, struct segmentStatusChange *change, uint8 statusNew);

static uint32 transition
	(
	uint32 stateOld,
	uint32 trans,
	CdbComponentDatabaseInfo *primary,
	CdbComponentDatabaseInfo *mirror,
	struct segmentStatusChange *changesPrimary,
	struct segmentStatusChange *changesMirror
	)
	;

static uint32 transitionFilerep(uint32 stateOld, uint32 trans);
static uint32 transitionSAN(uint32 stateOld, uint32 trans);

static void resolveStateFilerep
	(
	CdbComponentDatabaseInfo *primary,
	CdbComponentDatabaseInfo *mirror,
	uint32 stateNew,
	uint32 *statePrimary,
	uint32 *stateMirror
	)
	;

static void resolveStateSAN
	(
	CdbComponentDatabaseInfo *primary,
	CdbComponentDatabaseInfo *mirror,
	uint32 stateNew,
	uint32 *statePrimary,
	uint32 *stateMirror
	)
	;

typedef enum FilerepModeUpdateLoggingEnum
{
	FilerepModeUpdateLoggingEnum_MirrorToChangeTracking,
	FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking,
} FilerepModeUpdateLoggingEnum;

static void updateConfiguration(struct segmentStatusChange *changes, int changeEntries);

static PGconn *openSelfConn(CdbComponentDatabaseInfo *entryDB);
static void closeSelfConn(PGconn *conn);
static void probeUpdateConfig(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

static void getGpFailover_strategy(char *strategy);
static void SAN_Failover(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

static void Filerep_mode_update(int dbid, char *mode, char status, FilerepModeUpdateLoggingEnum logMsgToSend);
static void Filerep_Failover(struct segmentStatusChange *changes, int changeCount);

static void Filerep_preprocess_probe_results(uint8 *probe_results);

static void Null_Failover(struct segmentStatusChange *changes);

static void dumpChanges(struct segmentStatusChange *changes, int changeEntries);

static void getMounts_forDbid(int dbid, List **mountp);
static char mountMaintenance_forMpid(int mpid);
static void startSegment_forDbid(int dbid);

/*
 * Main entry point for ftsprobe process.
 *
 * This code is heavily based on pgarch.c, q.v.
 */
int
ftsprobe_start(void)
{
	pid_t		FtsProbePID;

#ifdef EXEC_BACKEND
	switch ((FtsProbePID = ftsprobe_forkexec()))
#else
	switch ((FtsProbePID = fork_process()))
#endif
	{
		case -1:
			ereport(LOG,
					(errmsg("could not fork ftsprobe process: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(false);

			FtsProbeMain(0, NULL);
			break;
#endif
		default:
			return (int)FtsProbePID;
	}

	
	/* shouldn't get here */
	return 0;
}


/*=========================================================================
 * HELPER FUNCTIONS
 */


#ifdef EXEC_BACKEND
/*
 * ftsprobe_forkexec()
 *
 * Format up the arglist for the ftsprobe process, then fork and exec.
 */
static pid_t
ftsprobe_forkexec(void)
{
	char	   *av[10];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkftsprobe";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif   /* EXEC_BACKEND */

static void
RequestShutdown(SIGNAL_ARGS)
{
	shutdown_requested = true;
}

/* SIGINT: set flag to run an fts full-scan */
static void
ReqFtsFullScan(SIGNAL_ARGS)
{
	rescan_requested = true;
}

/*
 * FtsProbeMain
 */
NON_EXEC_STATIC void
FtsProbeMain(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;
	char	   *fullpath;

	IsUnderPostmaster = true;
	am_ftsprobe = true;
	
	/* reset MyProcPid */
	MyProcPid = getpid();
	
	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

	/* Identify myself via ps */
	init_ps_display("ftsprobe process", "", "", "");

	SetProcessingMode(InitProcessing);

	/*
	 * Set up signal handlers.	We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 *
	 * Currently, we don't pay attention to postgresql.conf changes that
	 * happen during a single daemon iteration, so we can ignore SIGHUP.
	 */
	pqsignal(SIGHUP, SIG_IGN);

	/*
	 * Presently, SIGINT will lead to autovacuum shutdown, because that's how
	 * we handle ereport(ERROR).  It could be improved however.
	 */
	pqsignal(SIGINT, ReqFtsFullScan);		/* request full-scan */
	pqsignal(SIGTERM, die);
	pqsignal(SIGQUIT, quickdie); /* we don't do any ftsprobe specific cleanup, just use the standard. */
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	/* We don't listen for async notifies */
	pqsignal(SIGUSR2, RequestShutdown);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Copied from bgwriter
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "FTS Probe");

	/* Early initialization */
	BaseInit();

	/* See InitPostgres()... */
	InitProcess();	
	InitBufferPoolBackend();
	InitXLOGAccess();

	SetProcessingMode(NormalProcessing);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * We can now go away.	Note that because we'll call InitProcess, a
		 * callback will be registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	/*
	 * In order to access the catalog, we need a database, and a
	 * tablespace; our access to the heap is going to be slightly
	 * limited, so we'll just use some defaults.
	 */
	MyDatabaseId = TemplateDbOid;
	MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;

	if (!FindMyDatabase(probeDatabase, &MyDatabaseId, &MyDatabaseTableSpace))
		ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE),
			errmsg("database 'postgres' does not exist")));

	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	SetDatabasePath(fullpath);

	/*
	 * Finish filling in the PGPROC struct, and add it to the ProcArray. (We
	 * need to know MyDatabaseId before we can do this, since it's entered
	 * into the PGPROC struct.)
	 *
	 * Once I have done this, I am visible to other backends!
	 */
	InitProcessPhase2();

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.
	 *
	 * Sets up MyBackendId, a unique backend identifier.
	 */
	MyBackendId = InvalidBackendId;

	InitBackendSharedInvalidationState();

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad backend id: %d", MyBackendId);

	/*
	 * bufmgr needs another initialization call too
	 */
	InitBufferPoolBackend();

	/* heap access requires the rel-cache */
	RelationCacheInitialize();
	InitCatalogCache();

	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Load relcache entries for the system catalogs.  This must create at
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase2();

	/* shmem: publish probe pid */
	ftsProbeInfo->fts_probePid = MyProcPid;

	/*
	 * Before we can open probe connections, we need a username. (This
	 * will access catalog tables).
	 */
	retrieveUserAndDb(&probeUser);

	elog(LOG, "probeUser %s probeDatabase %s", probeUser, probeDatabase);
	
	/* main loop */
	FtsProbeLoop();

	/* One iteration done, go away */
	proc_exit(0);
}

static void
readCdbComponentInfoAndUpdateStatus(void)
{
	int i;

	cdb_component_dbs = getCdbComponentInfo(false);

	ftsLock();
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];
		uint8	segStatus;

		segStatus = 0;

		if (SEGMENT_IS_ALIVE(segInfo))
			segStatus |= FTS_STATUS_ALIVE;

		if (SEGMENT_IS_ACTIVE_PRIMARY(segInfo))
			segStatus |= FTS_STATUS_PRIMARY;

		if (segInfo->preferred_role == 'p')
			segStatus |= FTS_STATUS_DEFINEDPRIMARY;

		if (segInfo->mode == 's')
			segStatus |= FTS_STATUS_SYNCHRONIZED;

		if (segInfo->mode == 'c')
			segStatus |= FTS_STATUS_CHANGELOGGING;

		ftsProbeInfo->fts_status[segInfo->dbid] = segStatus;
	}
	ftsUnlock();
}

void FtsProbeLoop(void)
{
	bool	updated_bitmap, processing_fullscan;
	MemoryContext probeContext = NULL, oldContext = NULL;
	time_t elapsed,	probe_start_time;

	probe_start_time = time(NULL);

	probeContext = AllocSetContextCreate(TopMemoryContext,
										 "FtsProbeMemCtxt",
										 ALLOCSET_DEFAULT_INITSIZE,	/* always have some memory */
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
	
	readCdbComponentInfoAndUpdateStatus();

	for (;;)
	{
		if (shutdown_requested)
			break;
		
		/* no need to live on if postmaster has died */
		if (!PostmasterIsAlive(true))
			exit(1);

		if (ftsProbeInfo->fts_pauseProbes)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				elog(LOG, "skipping probe, we're paused.");
			goto prober_sleep;
		}

		if (cdb_component_dbs != NULL)
		{
			freeCdbComponentDatabases(cdb_component_dbs);
			cdb_component_dbs = NULL;
		}

		if (ftsProbeInfo->fts_probeScanRequested == ftsProbeInfo->fts_statusVersion)
			processing_fullscan = true;
		else
			processing_fullscan = false;

		readCdbComponentInfoAndUpdateStatus();
		getGpFailover_strategy(&failover_strategy);

		elog(DEBUG3, "Ftsprober: starting %sscan with %d dbids with %d content-ids",
			 (processing_fullscan ? "full " : ""),
			 cdb_component_dbs->total_segment_dbs,
			 cdb_component_dbs->total_segments);

		/*
		 * We probe in a special context, some of the heap access
		 * stuff palloc()s internally
		 */
		oldContext = MemoryContextSwitchTo(probeContext);

		/* Start a scan of the segments */
		updated_bitmap = probeSegments();

		MemoryContextSwitchTo(oldContext);

		/* free any pallocs we made inside probeSegments() */
		MemoryContextReset(probeContext);

		if (ftsProbeInfo->fts_pauseProbes)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				elog(LOG, "skipping probe, we're paused.");
			goto prober_sleep;
		}

		/*
		 * If we're not processing a full-scan, but one has been requested; we start over.
		 */
		if (!processing_fullscan &&
			ftsProbeInfo->fts_probeScanRequested == ftsProbeInfo->fts_statusVersion)
			continue;

		/*
		 * bump the version (this also serves as an acknowledgement to
		 * a probe-request).
		 */
		if (updated_bitmap || processing_fullscan)
		{
			ftsProbeInfo->fts_statusVersion = ftsProbeInfo->fts_statusVersion + 1;
			rescan_requested = false;
		}

		/* if no full-scan has been requested, we can sleep. */
		if (ftsProbeInfo->fts_probeScanRequested >= ftsProbeInfo->fts_statusVersion)
		{
			/* we need to do a probe immediately */
			elog(LOG, "skipping sleep -- rescanning (%d < %d)",
				 (int)ftsProbeInfo->fts_probeScanRequested, (int)ftsProbeInfo->fts_statusVersion);
			continue;
		}

	prober_sleep:
		{
			/* check if we need to sleep before starting next iteration */
			elapsed = time(NULL) - probe_start_time;
			if (elapsed < gp_fts_probe_interval)
			{
				pg_usleep((gp_fts_probe_interval - elapsed) * USECS_PER_SEC);
				probe_start_time = time(NULL);
			}
		}
	} /* end server loop */

	return;
}

/*
 * Wrapper for catalog lookup for a super user appropriate for FTS probing. We
 * want to use the bootstrap super user as a priority, because it seems more
 * obvious for users and probably hasn't been messed with. If it has been (might
 * have been removed, or modified in other ways), find another which can login
 * and which has not expired yet.
 */
char *
find_superuser_name(bool try_bootstrap)
{
	char *suser = NULL;
	Relation auth_rel;
	HeapTuple	auth_tup;
	HeapScanDesc auth_scan;
	ScanKeyData key[3];
	bool	isNull;

	ScanKeyInit(&key[0],
				Anum_pg_authid_rolsuper,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));

	ScanKeyInit(&key[1],
				Anum_pg_authid_rolcanlogin,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));

	if (try_bootstrap)
	{
		ScanKeyInit(&key[2],
					ObjectIdAttributeNumber,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID));
	}

	auth_rel = heap_open(AuthIdRelationId, AccessShareLock);
	auth_scan = heap_beginscan(auth_rel, SnapshotNow, try_bootstrap ? 3 : 2, key);
	while (HeapTupleIsValid(auth_tup = heap_getnext(auth_scan, ForwardScanDirection)))
	{
		Datum	attrName;
		Datum	attrNameOid;
		Oid		userOid;
		Datum	validuntil;

		validuntil = heap_getattr(auth_tup, Anum_pg_authid_rolvaliduntil, auth_rel->rd_att, &isNull);
		/* we actually want it to be NULL, that means always valid */
		if (!isNull)
			continue;

		attrName = heap_getattr(auth_tup, Anum_pg_authid_rolname, auth_rel->rd_att, &isNull);
		Assert(!isNull);
		suser = pstrdup(DatumGetCString(attrName));

		attrNameOid = heap_getattr(auth_tup, ObjectIdAttributeNumber, auth_rel->rd_att, &isNull);
		Assert(!isNull);
		userOid = DatumGetObjectId(attrNameOid);

		SetSessionUserId(userOid, true);

		break;
	}

	heap_endscan(auth_scan);
	heap_close(auth_rel, AccessShareLock);
	return suser;
}

/*
 * Get a user for the FTS prober to connect as
 */
static void
retrieveUserAndDb(char **probeUser)
{
	Assert(probeUser != NULL);

	/* first, let's try the bootstrap super user */
	*probeUser = find_superuser_name(true);
	if (!(*probeUser))
		*probeUser = find_superuser_name(false);

	Assert(*probeUser != NULL);
}

/*
 * This is called from several different threads: ONLY USE THREADSAFE FUNCTIONS INSIDE.
 */
static char
FtsTestOneSegment(CdbComponentDatabaseInfo *dbInfo)
{
	bool	done=false;
	char	ret = PROBE_DEAD;
	enum	probe_state_t probe_state = CONNECTING;

	int		n, fd;
	struct sockaddr_in  saddr;

	struct timeval	startTime;
	uint64			elapsed_ms = 0;

	char	buf[32]; /* big enough for the message we're expecting */
	int		offset = 0;
	int		remainder = sizeof(buf);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		/* something happened ? */
		write_log("Couldn't get socket to start probe.");
		return PROBE_ALIVE;
	}

	if (!pg_set_noblock(fd))
	{
		/* something happened ? */
		write_log("Couldn't mark socket non-blocking.");
		close(fd);
		return PROBE_ALIVE;
	}

	/*
	 * Get ipaddr to connect to.
 	 */
	MemSet(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	if ((n = inet_net_pton(AF_INET, dbInfo->hostip, &saddr.sin_addr, sizeof(saddr.sin_addr))) < 1)
	{
		/* something happened ? */
		write_log("Couldn't get IP-address of probe target.");
		close(fd);
		return PROBE_ALIVE;
	}
	saddr.sin_port = htons((unsigned short)dbInfo->port);

	/* Connect to dbInfo->hostip */
	n = connect(fd, (struct sockaddr *) &saddr, sizeof(saddr));
	if (n == 0 ||
		(n < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)))
	{
		/* normal */
	}
	else
	{
		/* something happened ? */
		write_log("Connect() failure. %d  (dbid %d content %d host is %s)",
				  errno, dbInfo->dbid, dbInfo->segindex, dbInfo->hostip);
		close(fd);

		return PROBE_DEAD;
	}

  	gettimeofday(&startTime, NULL);

	for (;;)
	{
		int		timeout;
		struct pollfd nfd;

		switch (probe_state)
		{
			case CONNECTING:
				nfd.fd = fd;
				nfd.events = POLLOUT;
				timeout = 10000;

				n = poll(&nfd, 1, timeout);
				if (n == 0)
				{
					break;
				}
				else if (n == 1 && (nfd.events & POLLOUT))
				{
					/* advance state, send request */
					probe_state = CONNECTED;
					continue;
				}
				else /* if (n < 0) */
				{
					/* error on poll ? EINTR ? */
					if (errno == EINTR)
					{
						/*
						 * assume that our target is up -- we'll have
						 * to pick up any failure on the next probe
						 */
						ret = PROBE_ALIVE;
						done = true;
					}
				}
				break;

			case CONNECTED:
			{
				/* Send the status request-startup-packet (see pqcomm.h, and bin/gpmirrortransition.c) */
				int sent=0;
				char *sendp;

				struct
				{
					uint32		packetlen;
					PrimaryMirrorTransitionPacket payload;
				} tqp;

				tqp.packetlen = htonl((uint32) sizeof(tqp));
				tqp.payload.protocolCode = (MsgType) htonl(PRIMARY_MIRROR_TRANSITION_QUERY_CODE);
				tqp.payload.dataLength = 0;

				sendp = (char *)&tqp;
				while (sent < sizeof(tqp))
				{
					n = send(fd, sendp, sizeof(tqp) - sent, 0);
					if (n < 0)
					{
						if (errno == EINTR)
						{
							/*
							 * assume that our target is up -- we'll have
							 * to pick up any failure on the next probe
							 */
							ret = PROBE_ALIVE;
						}
						else
						{
							/* errors other than signals: assume that the target is down. */
							ret = PROBE_DEAD;
						}
						break;
					}
					else
					{
						sendp += n;
						sent += n;
					}
				}
				if (sent == sizeof(tqp))
				{
					/* advance state, sent request, wait on response */
					probe_state = REQUEST_SENT;
					continue;
				}
				else
				{
					/* any failure to advance means that we must be done. */
					done = true;
				}
				break;
			}

			case REQUEST_SENT:
				nfd.fd = fd;
				nfd.events = POLLIN;
				timeout = 10000;

				n = poll(&nfd, 1, timeout);
				if (n == 0)
				{
					/* timeout */
					break;
				}
				else if (n == 1 && (nfd.events & POLLIN))
				{
					/* advance state, send request */
					probe_state = RESPONSE_READY;
					continue;
				}
				else /* if (n < 0) */
				{
					/* error on poll ? EINTR ? */
					if (errno == EINTR)
					{
						/*
						 * assume that our target is up -- we'll have
						 * to pick up any failure on the next probe
						 */
						ret = PROBE_ALIVE;
						done = true;
					}
				}
				break;
			case RESPONSE_READY:
			{
				/* Read a message back from our peer. */

				/* If status is OK, then we're all done. */
				n = recv(fd, buf + offset, remainder, 0 /* flags */);

				if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
					write_log("RESPONSE_READY: read %d offset %d remainder %d", n, offset, remainder);

				if (n == 0)
				{
					/* peer gave up on us ? */
					ret = PROBE_DEAD;
					done = true;
					break;
				}
				else if (n < 0)
				{
					if (errno == EAGAIN)
					{
						/* need to wait on more data ? (essentially we're rewinding our state) */
						probe_state = REQUEST_SENT;
						continue;
					}
					ret = PROBE_DEAD;
					done = true;
					break;
				}
				else
				{
					PrimaryMirrorMode pm_mode;
					SegmentState_e s_state;
					DataState_e d_state;
					FaultType_e f_type;
					uint32 msg_int;

					offset += n;
					remainder -= n;

					/* got legit data */
					if (offset < MAX_PROBE_RESPONSE_LEN)
					{
						/* need to wait on more data ? (essentially we're rewinding our state) */
						probe_state = REQUEST_SENT;
						continue;
					}

					memcpy(&msg_int, buf, 4);
					if (ntohl(msg_int) != MAX_PROBE_RESPONSE_LEN)
					{
						write_log("Probe protocol violation got length %d", ntohl(msg_int));
						ret = PROBE_DEAD;
						done = true;
						break;
					}

					memcpy(&msg_int, buf+4, 4);
					if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
						write_log("first message int %d", ntohl(msg_int));
					pm_mode = (PrimaryMirrorMode)ntohl(msg_int);

					memcpy(&msg_int, buf+8, 4);
					if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
						write_log("second message int %d", ntohl(msg_int));
					s_state = (SegmentState_e)ntohl(msg_int);

					memcpy(&msg_int, buf+12, 4);
					if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
						write_log("third message int %d", ntohl(msg_int));
					d_state = (DataState_e)ntohl(msg_int);
					

					memcpy(&msg_int, buf+16, 4);
					if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
						write_log("fourth message int %d", ntohl(msg_int));
					f_type = (FaultType_e)ntohl(msg_int);					

					if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
						write_log("Probe result for dbid %d content %d: %s %s %s %s",
								  dbInfo->dbid, dbInfo->segindex,
								  getMirrorModeLabel(pm_mode),
								  getSegmentStateLabel(s_state), 
								  getDataStateLabel(d_state),
								  getFaultTypeLabel(f_type));

					ret = PROBE_ALIVE;

					if (s_state == SegmentStateFault)
					{
						/* get fault type - this will be used to decide the next segment state */
						switch(f_type)
						{
							case FaultTypeMirror:
								ret |= PROBE_FAULT_MIRROR;
								break;
							case FaultTypeDB:
							case FaultTypeIO:
								ret |= PROBE_FAULT_CRASH;
								break;
							case FaultTypePostmasterReset:
								ret = PROBE_ALIVE;
								break;
							default:
								Assert(!"Unexpected segment fault type");
						}
						write_log("Segment with dbid %d content %d reported fault %s to the prober",
								  dbInfo->dbid, dbInfo->segindex, getFaultTypeLabel(f_type));
					}
					
					if (d_state == DataStateInSync && dbInfo->mode == 'r')
					{
						ret |= PROBE_RESYNC_COMPLETE;
					}

					done = true;
					break;
				}

				/*
				 * If status is not OK, then we need to act on the result (either
				 * this segment is down, or its mirror is down)
				 */
			}
			default:
				ret = PROBE_DEAD;
				done = true;
				break;
		}

		elapsed_ms = gettime_elapsed_ms(startTime);

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			write_log("Probe elapsed time: " UINT64_FORMAT " milliseconds", elapsed_ms);

		if (done)
		{
			break;
		}

		/* If connection takes more than the gp_segment_connect_timeout, we fail. */
		if (elapsed_ms > gp_segment_connect_timeout * 1000)
		{
			ret = PROBE_DEAD;
			break;
		}
	}

	/* we're done, slam everything shut. */
	shutdown(fd, SHUT_RDWR);
	close(fd);

	return (ret);
}

static inline bool
segmentToProbe(CdbComponentDatabaseInfo *segInfo, char failover_strategy)
{
	switch (failover_strategy)
	{
		case 'f':
			if (SEGMENT_IS_ACTIVE_MIRROR(segInfo) && SEGMENT_IS_ALIVE(segInfo))
				return true;
			/* fallthrough */
		case 'n':
		case 's':
			if (SEGMENT_IS_ACTIVE_PRIMARY(segInfo))
				return true;
			break;
		default:			
			write_log("segmentToProbe: invalid failover strategy (%c)", failover_strategy);
			break;
	}

	return false;
}

/*
 * probe segments to check if they are alive and if any failure has occurred
 */
static bool
probeSegments()
{
	int i;
	bool updated_bitmap = false;

	struct threadWorkerInfo worker_info;
	int pthread_err;
	int workers = gp_fts_probe_threadcount;
	pthread_t *threads = NULL;

	if (cdb_component_dbs == NULL)
		return false;

	memset(scan_status, 0, sizeof(scan_status));

	/* figure out which segments to include in the scan. */
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (segmentToProbe(segInfo, failover_strategy))
		{
			/* mark segment for probing */
			scan_status[segInfo->dbid] = PROBE_SEGMENT;
		}
		else
		{
			/* consider segment dead */
			scan_status[segInfo->dbid] = PROBE_DEAD;
		}
	}

	worker_info.scan_status = scan_status;

	threads = (pthread_t *)palloc(workers * sizeof(pthread_t));
	for (i = 0; i < workers; i++)
	{
		pthread_err = gp_pthread_create(&threads[i], probeSegmentFromThread, &worker_info, "probeSegments");
		/* XXX: deal with error */
	}

	/* we have nothing left to do but wait */
	for (i = 0; i < workers; i++)
	{
		pthread_join(threads[i], NULL);
		/* XXX: deal with error */
	}

	pfree(threads);
	threads = NULL;

	/* if we're shutting down, just exit. */
	if (shutdown_requested)
		return false;

	if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
	{
		elog(LOG, "probeSegments, complete results look like:");
		for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			CdbComponentDatabaseInfo *segInfo = NULL;

			segInfo = &cdb_component_dbs->segment_db_info[i];

			elog(LOG, "segment dbid %d status 0x%x", segInfo->dbid, scan_status[segInfo->dbid]);
		}
	}

	/*
	 * Now we've completed the scan, update shared-memory. if we
	 * change anything, we return true.
	 */
	updated_bitmap = probePublishUpdate(scan_status);

	return updated_bitmap;
}

static void *
probeSegmentFromThread(void *arg)
{
	struct threadWorkerInfo *worker_info;
	int i;

	worker_info = (struct threadWorkerInfo *)arg;

	i = 0;
	
	for (;;)
	{
		CdbComponentDatabaseInfo *primary = NULL;
		CdbComponentDatabaseInfo *mirror = NULL;

		char probe_result_primary = PROBE_DEAD;
		char probe_result_mirror = PROBE_DEAD;

		/*
		 * find untested primary, mark primary and mirror "tested" and unlock.
		 */
		pthread_mutex_lock(&worker_thread_mutex);
		for (; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			primary = &cdb_component_dbs->segment_db_info[i];

			/* check segments in pairs of primary-mirror */
			if (!SEGMENT_IS_ACTIVE_PRIMARY(primary))
			{
				continue;
			}

			if (PROBE_CHECK_FLAG(worker_info->scan_status[primary->dbid], PROBE_SEGMENT))
			{
				/* prevent re-checking this pair */
				worker_info->scan_status[primary->dbid] &= ~PROBE_SEGMENT;

				mirror = getMirrorEntryByContent(primary->segindex, primary->dbid);

				/* check if mirror is marked for probing */
				if (mirror != NULL &&
					PROBE_CHECK_FLAG(worker_info->scan_status[mirror->dbid], PROBE_SEGMENT))
				{
					worker_info->scan_status[mirror->dbid] &= ~PROBE_SEGMENT;
				}
				else
				{
					mirror = NULL;
				}

				break;
			}
		}
		pthread_mutex_unlock(&worker_thread_mutex);

		/* check if all segments were probed */
		if (i == cdb_component_dbs->total_segment_dbs || primary == NULL)
		{
			break;
		}

		/* if we've gotten a pause or shutdown request, we ignore probe results. */
		if (ftsProbeInfo->fts_pauseProbes || shutdown_requested)
		{
			break;
		}

		/* probe primary */
		probe_result_primary = FtsTestOneSegment(primary);
		Assert(!PROBE_CHECK_FLAG(probe_result_primary, PROBE_SEGMENT));

		if ((probe_result_primary & PROBE_ALIVE) == 0)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				write_log("Segment %d segment-dbid %d (status 0x%x) was found dead",
				          primary->segindex, primary->dbid, probe_result_primary);
		}

		/* probe mirror */
		if (mirror != NULL)
		{
			probe_result_mirror = FtsTestOneSegment(mirror);
			Assert(!PROBE_CHECK_FLAG(probe_result_mirror, PROBE_SEGMENT));

			if ((probe_result_mirror & PROBE_ALIVE) == 0)
			{
				if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
					write_log("Segment %d segment-dbid %d (status 0x%x) was found dead",
							  mirror->segindex, mirror->dbid, probe_result_mirror);
			}
		}

		/* update results */
		pthread_mutex_lock(&worker_thread_mutex);
		worker_info->scan_status[primary->dbid] = probe_result_primary;
		if (mirror != NULL)
		{
			worker_info->scan_status[mirror->dbid] = probe_result_mirror;
		}
		pthread_mutex_unlock(&worker_thread_mutex);
	}

	return NULL;
}

static CdbComponentDatabaseInfo *getMirrorEntryByContent(int content, int dbid)
{
	int i;

	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (segInfo->segindex == content && segInfo->dbid != dbid)
		{
			/* found it */
			return segInfo;
		}
	}

	return NULL;
}

/*
 * Build a set of changes, based on our current state, and the probe results.
 */
static bool
probePublishUpdate(uint8 *probe_results)
{
	bool update_found = false;
	int i;

	if (failover_strategy == 'f')
	{
		/* preprocess probe results to decide what is the current segment state */
		Filerep_preprocess_probe_results(probe_results);
	}

	for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		/* if we've gotten a pause or shutdown request, we ignore our probe results. */
		if (ftsProbeInfo->fts_pauseProbes || shutdown_requested)
		{
			return false;
		}

		/* we check segments in pairs of primary-mirror */
		if (!SEGMENT_IS_ACTIVE_PRIMARY(segInfo))
		{
			continue;
		}

		CdbComponentDatabaseInfo *primary = segInfo;
		CdbComponentDatabaseInfo *mirror = getMirrorEntryByContent(segInfo->segindex, segInfo->dbid);

		if (failover_strategy == 'n')
		{
			Assert(SEGMENT_IS_ACTIVE_PRIMARY(segInfo));
			Assert(FTS_STATUS_ISALIVE(segInfo->dbid, ftsProbeInfo->fts_status));
			Assert(mirror == NULL);

			/* no mirror available to failover */
			if (!PROBE_IS_ALIVE(segInfo))
			{
				struct segmentStatusChange changes;
				uint8 statusOld = ftsProbeInfo->fts_status[segInfo->dbid];
				uint8 statusNew = statusOld & ~FTS_STATUS_ALIVE;

				buildSegmentStateChange(segInfo, &changes, statusNew);

				Null_Failover(&changes);
			}
			continue;
		}

		Assert(failover_strategy == 'f' || failover_strategy == 's');
		Assert(mirror != NULL);

		/* changes required for primary and mirror */
		struct segmentStatusChange changes[2];

		uint32 stateOld = 0;
		uint32 stateNew = 0;

		bool isPrimaryAlive = PROBE_IS_ALIVE(primary);
		bool isMirrorAlive = PROBE_IS_ALIVE(mirror);

		/* get transition type */
		uint32 trans = getTransition(isPrimaryAlive, isMirrorAlive);

		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: primary found %s, mirror found %s, transition %d",
				 (isPrimaryAlive ? "alive" : "dead"), (isMirrorAlive ? "alive" : "dead"), trans);
		}

		if (failover_strategy == 'f')
		{
			/* get current state */
			stateOld = getPairStateFilerep(primary, mirror);

			/* get new state */
			stateNew = transition(stateOld, trans, primary, mirror, &changes[0], &changes[1]);
		}
		else
		{
			Assert(failover_strategy == 's');

			/* get current state */
			stateOld = getPairStateSAN(primary, mirror);

			/* get new state */
			stateNew = transition(stateOld, trans, primary, mirror, &changes[0], &changes[1]);
		}

		/* check if transition is required */
		if (stateNew != stateOld)
		{
			update_found = true;
			updateConfiguration(changes, ARRAY_SIZE(changes));
		}
	}

	if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
	{
		elog(LOG, "probePublishUpdate: probe result processing is complete");
	}

	return update_found;
}


/*
 * Build struct with segment changes
 */
static void
buildSegmentStateChange(CdbComponentDatabaseInfo *segInfo, struct segmentStatusChange *change, uint8 statusNew)
{
	change->dbid = segInfo->dbid;
	change->segindex = segInfo->segindex;
	change->oldStatus = ftsProbeInfo->fts_status[segInfo->dbid];
	change->newStatus = statusNew;
}

/*
 * get transition type - derived from probed primary/mirror state
 */
static uint32
getTransition(bool isPrimaryAlive, bool isMirrorAlive)
{
	uint32 state = (isPrimaryAlive ? 2 : 0) + (isMirrorAlive ? 1 : 0);

	switch (state)
	{
		case (0):
			/* primary and mirror dead */
			return TRANS_D_D;
		case (1):
			/* primary dead, mirror alive */
			return TRANS_D_U;
		case (2):
			/* primary alive, mirror dead */
			return TRANS_U_D;
		case (3):
			/* primary and mirror alive */
			return TRANS_U_U;
		default:
			Assert(!"Invalid transition for FTS state machine");
			return 0;
	}
}


/*
 * Get combined state of primary and mirror for filerep
 */
static uint32
getPairStateFilerep(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror)
{
	Assert(FTS_STATUS_ISALIVE(primary->dbid, ftsProbeInfo->fts_status));
	Assert(FTS_STATUS_ISPRIMARY(primary->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_ISPRIMARY(mirror->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_IS_CHANGELOGGING(mirror->dbid, ftsProbeInfo->fts_status));

	if (FTS_STATUS_IS_SYNCED(primary->dbid, ftsProbeInfo->fts_status))
	{
		/* Primary: Up, Sync - Mirror: Up, Sync */

		Assert(!FTS_STATUS_IS_CHANGELOGGING(primary->dbid, ftsProbeInfo->fts_status));
		Assert(FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));
		Assert(FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status));

		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: last state: primary dbid=%d sync, mirror dbid=%d sync",
				 primary->dbid, mirror->dbid);
		}

		return FILEREP_PUS_MUS;
	}
	else if (FTS_STATUS_IS_CHANGELOGGING(primary->dbid, ftsProbeInfo->fts_status))
	{
		/* Primary: Up, Changetracking - Mirror: Down */

		Assert(!FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));

		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: last state: primary dbid=%d change-tracking, mirror dbid=%d dead",
				 primary->dbid, mirror->dbid);
		}

		return FILEREP_PUC_MDX;
	}
	else
	{
		/* Primary: Up, Resync - Mirror: Up, Resync */

		Assert(FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));
		Assert(!FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status));

		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: last state: primary dbid=%d resync, mirror dbid=%d resync",
				 primary->dbid, mirror->dbid);
		}

		return FILEREP_PUR_MUR;
	}
}


/*
 * Get combined state of primary and mirror for SAN
 */
static uint32
getPairStateSAN(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror)
{
	Assert(FTS_STATUS_ISALIVE(primary->dbid, ftsProbeInfo->fts_status));
	Assert(FTS_STATUS_ISPRIMARY(primary->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_ISPRIMARY(mirror->dbid, ftsProbeInfo->fts_status));

	if (FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status))
	{
		/* Primary: Up - Mirror: Up */

		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: last state: primary dbid=%d up, mirror dbid=%d up",
				 primary->dbid, mirror->dbid);
		}

		return SAN_PU_MU;
	}
	else
	{
		/* Primary: Up - Mirror: Down */
		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			elog(LOG, "probePublishUpdate: last state: primary dbid=%d up, mirror dbid=%d dead",
				 primary->dbid, mirror->dbid);
		}

		return SAN_PU_MD;
	}
}


/*
 * find new state for primary and mirror
 */
static uint32
transition
	(
	uint32 stateOld,
	uint32 trans,
	CdbComponentDatabaseInfo *primary,
    CdbComponentDatabaseInfo *mirror,
    struct segmentStatusChange *changesPrimary,
    struct segmentStatusChange *changesMirror
    )
{
	Assert(IS_VALID_TRANSITION(trans));

	/* reset changes */
	memset(changesPrimary, 0, sizeof(*changesPrimary));
	memset(changesMirror, 0, sizeof(*changesMirror));

	uint32 stateNew = stateOld;

	/* in case of a double failure we don't do anything */
	if (trans == TRANS_D_D)
	{
		return stateOld;
	}

	/* get new state for primary and mirror */
	if (failover_strategy == 'f')
	{
		stateNew = transitionFilerep(stateOld, trans);
	}
	else
	{
		stateNew = transitionSAN(stateOld, trans);
	}

	/* check if transition is required */
	if (stateNew != stateOld)
	{
		uint32 stateNewPrimary = 0;
		uint32 stateNewMirror = 0;

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
		{
			elog(LOG, "probePublishUpdate: state machine transition from %d to %d", stateOld, stateNew);
		}

		if (failover_strategy == 'f')
		{
			resolveStateFilerep(primary, mirror, stateNew, &stateNewPrimary, &stateNewMirror);
		}
		else
		{
			resolveStateSAN(primary, mirror, stateNew, &stateNewPrimary, &stateNewMirror);
		}

		buildSegmentStateChange(primary, changesPrimary, stateNewPrimary);
		buildSegmentStateChange(mirror, changesMirror, stateNewMirror);

		dumpChanges(changesPrimary, 1);
		dumpChanges(changesMirror, 1);
	}

	return stateNew;
}


/*
 *  get new state for primary and mirror using filerep state machine
 */
static uint32
transitionFilerep(uint32 stateOld, uint32 trans)
{
	Assert(failover_strategy == 'f');
	Assert(IS_VALID_OLD_STATE_FILEREP(stateOld));

	int i = 0;



	/* check state machine for transition */
	for (i = 0; i < FILEREP_SENTINEL; i++)
	{
		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
		{
			elog(LOG, "ftsProbe: state machine: row=%d column=%d val=%d trans=%d comp=%d",
				 stateOld,
				 i,
				 state_machine_filerep[stateOld][i],
				 trans,
				 state_machine_filerep[stateOld][i] & trans);
		}

		if ((state_machine_filerep[stateOld][i] & trans) > 0)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
			{
				elog(LOG, "ftsProbe: state machine: match found, new state: %d", i);
			}
			return i;
		}
	}

	return stateOld;
}


/*
 *  get new state for primary and mirror using SAN state machine
 */
static uint32
transitionSAN(uint32 stateOld, uint32 trans)
{
	Assert(failover_strategy == 's');
	Assert(IS_VALID_OLD_STATE_SAN(stateOld));

	int i = 0;

	/* check state machine for transition */
	for (i = 0; i < SAN_SENTINEL; i++)
	{
		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
		{
			elog(LOG, "ftsProbe: state machine: row=%d column=%d val=%d trans=%d comp=%d",
				 stateOld, i, state_machine_san[stateOld][i], trans, state_machine_san[stateOld][i] & trans);
		}

		if ((state_machine_san[stateOld][i] & trans) > 0)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
			{
				elog(LOG, "ftsProbe: state machine: match found, new state: %d", i);
			}
			return i;
		}
	}

	return stateOld;
}


/*
 * resolve new filerep state for primary and mirror
 */
static void resolveStateFilerep
	(
	CdbComponentDatabaseInfo *primary,
	CdbComponentDatabaseInfo *mirror,
	uint32 stateNew,
	uint32 *stateNewPrimary,
	uint32 *stateNewMirror
	)
{
	Assert(failover_strategy == 'f');
	Assert(IS_VALID_NEW_STATE_FILEREP(stateNew));

	switch (stateNew)
	{
		case (FILEREP_PUC_MDX):

			*stateNewPrimary = ftsProbeInfo->fts_status[primary->dbid];
			*stateNewMirror = ftsProbeInfo->fts_status[mirror->dbid];

			/* primary: up, change-tracking */
			*stateNewPrimary &= ~FTS_STATUS_SYNCHRONIZED;
			*stateNewPrimary |= FTS_STATUS_CHANGELOGGING;

			/* mirror: down */
			*stateNewMirror &= ~FTS_STATUS_ALIVE;

			break;

		case (FILEREP_MDX_PUC):

			Assert(FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));
			Assert(FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status));

			*stateNewPrimary = ftsProbeInfo->fts_status[primary->dbid];
			*stateNewMirror = ftsProbeInfo->fts_status[mirror->dbid];

			/* primary: down, becomes mirror */
			*stateNewPrimary &= ~FTS_STATUS_PRIMARY;
			*stateNewPrimary &= ~FTS_STATUS_ALIVE;

			/* mirror: up, change-tracking, promoted to primary */
			*stateNewMirror |= FTS_STATUS_PRIMARY;
			*stateNewMirror &= ~FTS_STATUS_SYNCHRONIZED;
			*stateNewMirror |= FTS_STATUS_CHANGELOGGING;

			break;

		case (FILEREP_PUS_MUS):
		case (FILEREP_PUR_MUR):
			Assert(!"FTS is not responsible for bringing segments back to life");
		default:
			Assert(!"Invalid transition in filerep state machine");
	}
}


/*
 * resolve new SAN state for primary and mirror
 */
static void resolveStateSAN
	(
	CdbComponentDatabaseInfo *primary,
	CdbComponentDatabaseInfo *mirror,
	uint32 stateNew,
	uint32 *stateNewPrimary,
	uint32 *stateNewMirror
	)
{
	Assert(failover_strategy == 's');
	Assert(IS_VALID_NEW_STATE_SAN(stateNew));

	switch (stateNew)
	{
		case (SAN_PU_MD):

			*stateNewPrimary = ftsProbeInfo->fts_status[primary->dbid];
			*stateNewMirror = ftsProbeInfo->fts_status[mirror->dbid];

			/* mirror: down */
			*stateNewMirror &= ~FTS_STATUS_ALIVE;

			break;

		case (SAN_MD_PU):

			Assert(FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));

			*stateNewPrimary = ftsProbeInfo->fts_status[primary->dbid];
			*stateNewMirror = ftsProbeInfo->fts_status[mirror->dbid];

			/* primary: down, becomes mirror */
			*stateNewPrimary &= ~FTS_STATUS_PRIMARY;
			*stateNewPrimary &= ~FTS_STATUS_ALIVE;

			/* mirror: up, change-tracking, promoted to primary */
			*stateNewMirror |= FTS_STATUS_PRIMARY;

			break;

		case (SAN_PU_MU):
			Assert(!"FTS is not responsible for bringing segments back to life");
		default:
			Assert(!"Invalid transition in filerep state machine");
	}
}


/*
 * Apply requested segment transitions
 */
static void
updateConfiguration(struct segmentStatusChange *changes, int changeEntries)
{
	Assert(changes != NULL);

	PGconn *entryConn = NULL;
	char timestamp_str[TIMESTAMP_BUF_SIZE];

	CdbComponentDatabaseInfo *entryDB = &cdb_component_dbs->entry_db_info[0];

	/* if we've gotten a pause or shutdown request, we ignore our probe results. */
	if (ftsProbeInfo->fts_pauseProbes || shutdown_requested)
	{
		return;
	}

	get_timestamp(timestamp_str, sizeof(timestamp_str));

	if (entryDB->dbid != GpIdentity.dbid)
	{
		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "ftsProbe: advancing to second entryDB.");
		entryDB = entryDB + 1;
	}

	entryConn = openSelfConn(entryDB);
	Assert(entryConn != NULL);

	/* TODO: investigate if lightweight lock is necessary */
	ftsLock();

	/* update segment configuration */
	probeUpdateConfig(entryConn, changes, changeEntries, timestamp_str);

	if (failover_strategy == 'f')
	{
		/* commit our config/history changes */
		closeSelfConn(entryConn);

		/* FILEREP response */
		Filerep_Failover(changes, changeEntries);
	}
	else if (failover_strategy == 's')
	{
		/* SAN response */
		SAN_Failover(entryConn, changes, changeEntries, timestamp_str);

		/* commit our config/history changes */
		closeSelfConn(entryConn);
	}

	if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
	{
		elog(LOG, "probePublishUpdate: finished segment modifications");
	}

	ftsUnlock();
}


/*
 * Obtains utility-mode self-connection to master-DB, and opens a transaction.
 */
static PGconn *
openSelfConn(CdbComponentDatabaseInfo *entryDB)
{
	PQExpBuffer entryBuffer = NULL;
	PGconn *entryConn = NULL;

	PGresult   *rs;

	Assert(entryDB->dbid == -1);

	entryBuffer = createPQExpBuffer();
	if (PQExpBufferBroken(entryBuffer))
	{
		destroyPQExpBuffer(entryBuffer);
		elog(FATAL, "FTSPROBE: failed to create self-connection buffer.");
		/* never reached */
		return NULL;
	}

	appendPQExpBuffer(entryBuffer, "options='-c gp_session_role=UTILITY' ");

	/*
	 * Do not need to set host, since FTS is running on the same host
	 * as the master node.
	 */
	appendPQExpBuffer(entryBuffer, "port=%u ", entryDB->port);
	appendPQExpBuffer(entryBuffer, "dbname=%s ", probeDatabase);
	appendPQExpBuffer(entryBuffer, "user=%s ", probeUser);
	appendPQExpBuffer(entryBuffer, "connect_timeout=%d ", gp_segment_connect_timeout);

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
		char	*error_message = strdup(PQerrorMessage(entryConn));

		if (!error_message)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("FTS: out of memory")));

		destroyPQExpBuffer(entryBuffer);
		PQfinish(entryConn);
		entryConn = NULL;

		elog(FATAL, "FTS: setting segDB state failed, error connecting to entry db, error: %s", error_message);
	}

	rs = PQexec(entryConn, "BEGIN");

	if (PQresultStatus(rs) != PGRES_COMMAND_OK)
	{
		/*
		 * When we get an error, we strdup it here.  When the main thread
		 * checks for errors, it makes a palloc copy of this, and frees
		 * this.
		 */
		char	*error_message = strdup(PQerrorMessage(entryConn));

		if (!error_message)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("FTS: out of memory")));

		destroyPQExpBuffer(entryBuffer);
		PQfinish(entryConn);
		entryConn = NULL;

		elog(FATAL, "FTS: error connecting to entry db, could not start transaction error: %s", error_message);
	}

	destroyPQExpBuffer(entryBuffer);

	return entryConn;
}

/*
 * Commit and close self-connection to master-DB.
 */
static void
closeSelfConn(PGconn *conn)
{
	PGresult   *rs;

	rs = PQexec(conn, "COMMIT");
	if (PQresultStatus(rs) != PGRES_COMMAND_OK)
	{
		/*
		 * When we get an error, we strdup it here.  When the main thread
		 * checks for errors, it makes a palloc copy of this, and frees
		 * this.
		 */
		char	*error_message = strdup(PQerrorMessage(conn));

		if (!error_message)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("FTS: out of memory")));

		PQclear(rs);
		PQfinish(conn);
		conn = NULL;

		elog(FATAL, "FTS: connection to entry-DB could not commit: %s", error_message);
	}

	/* No error, we're ready to go */
	PQclear(rs);
	PQfinish(conn);
	return;
}


/*
 * update segment configuration in catalog and shared memory
 */
static void
probeUpdateConfig(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str)
{
	int i;

	int count;
	char cmd[SQL_CMD_BUF_SIZE];

	bool	valid;
	bool	primary;
	bool	changelogging;

	Assert(changes != NULL);
	Assert(changeCount > 0);

	for (i = 0; i < changeCount; i++)
	{
		struct segmentStatusChange *change = &changes[i];
		valid   = (changes[i].newStatus & FTS_STATUS_ALIVE) ? true : false;
		primary = (changes[i].newStatus & FTS_STATUS_PRIMARY) ? true : false;
		changelogging = (changes[i].newStatus & FTS_STATUS_CHANGELOGGING) ? true : false;

		if (changelogging)
		{
			Assert(failover_strategy == 'f');
			Assert(primary && valid);
		}

		Assert((valid || !primary) && "Primary cannot be down");

		/*
		 * Log change to segment configuration
		 */
		count = snprintf(cmd, sizeof(cmd),
						 "insert into %s values ('%s', %d, 'FTSPROBE: content %d fault marking status %s%s role %c')",
						 GpConfigHistoryRelationName,
						 time_str, change->dbid, change->segindex,
						 valid ? "UP" : "DOWN",
						 (changelogging) ? " mode: change-tracking" : "",
						 primary ? 'p' : 'm');

		Assert(count < sizeof(cmd));

		executeEntrySQL(entryConn, cmd);

		/*
		 * Update segment configuration in catalog
		 */
		count = snprintf(cmd, sizeof(cmd),
						 "update %s set role='%c', status='%c'%s where dbid=CAST(%d AS SMALLINT)",
						 GpSegmentConfigRelationName,
						 primary ? 'p' : 'm',
						 valid ? 'u' : 'd',
						 (changelogging) ? ", mode = 'c'" : "",
						 changes[i].dbid);

		Assert(count < sizeof(cmd));

		executeEntrySQL(entryConn, cmd);

		/*
		 * Update shared memory
		 */
		ftsProbeInfo->fts_status[changes[i].dbid] = changes[i].newStatus;
	}
}


static void
getGpFailover_strategy(char *strategy)
{
	Relation	strategy_rel;
	HeapTuple	strategy_tup;
	HeapScanDesc strategy_scan;
	bool	isNull=true;

	Assert(strategy != NULL);

	strategy_rel = heap_open(GpFaultStrategyRelationId, AccessShareLock);
	strategy_scan = heap_beginscan(strategy_rel, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(strategy_tup = heap_getnext(strategy_scan, ForwardScanDirection)))
	{
		Datum	strategy_datum;

		strategy_datum = heap_getattr(strategy_tup, Anum_gp_fault_strategy_fault_strategy, strategy_rel->rd_att, &isNull);

		if (isNull)
			break;

		*strategy = DatumGetChar(strategy_datum);
	}

	heap_endscan(strategy_scan);
	heap_close(strategy_rel, AccessShareLock);

	return;
}

/*
 * Dump out the changes to our logfile.
 */
static void
dumpChanges(struct segmentStatusChange *changes, int changeEntries)
{
	Assert(changes != NULL);
	int i = 0;

	for (i = 0; i < changeEntries; i++)
	{
		bool new_alive, old_alive;
		bool new_pri, old_pri;

		new_alive = (changes[i].newStatus & FTS_STATUS_ALIVE ? true : false);
		old_alive = (changes[i].oldStatus & FTS_STATUS_ALIVE ? true : false);

		new_pri = (changes[i].newStatus & FTS_STATUS_PRIMARY ? true : false);
		old_pri = (changes[i].oldStatus & FTS_STATUS_PRIMARY ? true : false);

		elog(LOG, "change: dbid %d content %d alive %d/%d primary %d/%d",
			 changes[i].dbid, changes[i].segindex, new_alive, old_alive, new_pri, old_pri);
	}
}

static void
Null_Failover(struct segmentStatusChange *changePrimary)
{
	if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
	{
		dumpChanges(changePrimary, 1);
	}
}

/*
 * pre-process probe results to take into account some special
 * state-changes that Filerep uses: when the segments have completed
 * re-sync, or when they report an explicit fault.
 *
 * NOTE: we examine pairs of primary-mirror segments; this is requiring
 * for reasoning about state changes.
 */
static void
Filerep_preprocess_probe_results(uint8 *probe_results)
{
	int i = 0;

	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];
		CdbComponentDatabaseInfo *primary = NULL, *mirror = NULL;

		if (!SEGMENT_IS_ACTIVE_PRIMARY(segInfo))
		{
			continue;
		}

		primary = segInfo;
		mirror = getMirrorEntryByContent(primary->segindex, primary->dbid);
		Assert(mirror != NULL && "mirrors should always be there in filerep mode");

		/*
		 * We've completed resync ? NOTE: we've only completed resync if *both* report completion.
		 */
		if (PROBE_IS_RESYNC_COMPLETE(primary) &&
			PROBE_IS_RESYNC_COMPLETE(mirror))
		{
			/*
			 * if primary and mirror are both in-sync then update configuration: because
			 * they will not necessarily reach in-sync internally at the same point, we
			 * make sure both are in-sync before updating configuration
			 */
			markSegmentInSyncInConfiguration(primary, mirror);
		}

		/*
		 * Decide which segments to consider "down."
		 *
		 * There are a few possibilities here:
		 *    1) primary in crash fault
		 *             primary considered dead
		 *    2) mirror in crash fault
		 *             mirror considered dead
		 *    3) primary in mirroring fault
		 *             primary considered alive, mirror considered dead
		 */
		if (PROBE_HAS_FAULT_CRASH(primary))
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported crash, considered dead.",
				 primary->dbid, probe_results[primary->dbid]);

			/* consider primary dead -- case (1) */
			probe_results[primary->dbid] &= ~PROBE_ALIVE;
		}
		else if (PROBE_HAS_FAULT_MIRROR(primary))
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported mirroring fault with mirror %d/0x%x, mirror considered dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);

			/* consider mirror dead -- case (3) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}

		if (PROBE_HAS_FAULT_CRASH(mirror))
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported crash, considered dead.",
				 mirror->dbid, probe_results[mirror->dbid]);

			/* consider mirror dead -- case (2) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}

		/* clear resync and fault flags because they are not needed after this processing */
		probe_results[primary->dbid] &= ~PROBE_FAULT_CRASH;
		probe_results[primary->dbid] &= ~PROBE_FAULT_MIRROR;
		probe_results[primary->dbid] &= ~PROBE_RESYNC_COMPLETE;
		probe_results[mirror->dbid] &= ~PROBE_FAULT_CRASH;
		probe_results[mirror->dbid] &= ~PROBE_FAULT_MIRROR;
		probe_results[mirror->dbid] &= ~PROBE_RESYNC_COMPLETE;
	}
}

/*
 *
primary
c
cwhipkey-mbp.local
55001
cwhipkey-mbp.local
65001
Which means:

primary            : segment should be a primary (mirror is the other relevant option for prober, I think, but you
can see the full list in primary_mirror_mode.*'s decipherPrimaryMirrorModeArgument).  Note that if other modes are
passed, the fields below should not be specified because they are not relevant.

c                  : corresponds to status field in gp_configuration (in this case, changelogging)
cwhipkey-mbp.local : host address
55001              : host port
cwhipkey-mbp.local : peer's address (mirror's address if I'm primary, or vice versa)
65001              : peer's port

 * gp_primarymirror -h <hostname> -p <postmaster_port> 
 */
static void
Filerep_Failover(struct segmentStatusChange *changes, int changeCount)
{
	int i;

	if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		dumpChanges(changes, changeCount);

	/*
	 * For each failed primary segment:
	 *   1) Convert the mirror into primary.
	 *
	 * For each failed mirror segment:
	 *   1) Convert the primary into change-logging.
	 */
	for (i=0; i < changeCount; i++)
	{
		bool new_alive, old_alive;
		bool new_pri, old_pri;

		new_alive = (changes[i].newStatus & FTS_STATUS_ALIVE ? true : false);
		old_alive = (changes[i].oldStatus & FTS_STATUS_ALIVE ? true : false);

		new_pri = (changes[i].newStatus & FTS_STATUS_PRIMARY ? true : false);
		old_pri = (changes[i].oldStatus & FTS_STATUS_PRIMARY ? true : false);

		if (!new_alive && old_alive)
		{
			/*
			 * this is a segment that went down, nothing to tell it, it doesn't
			 * matter whether it was primary or mirror here.
			 *
			 * Nothing to do here.
			 */
			if (new_pri)
			{
				elog(LOG, "Segment with dbid %d failed *and* was marked as the new primary! Dumping changes.", changes[i].dbid);
				dumpChanges(changes, changeCount);
				/* NOTE: we don't apply the state change here. */
			}
		}
		else if (new_alive && !old_alive)
		{
			/* this is a segment that came back up ? Nothing to do here. */
			if (old_pri)
			{
				elog(LOG, "Segment with dbid %d came back from the dead *and* was marked as the old primary! Dumping changes", changes[i].dbid);
				dumpChanges(changes, changeCount);
				/* NOTE: we don't apply the state change here. */
			}
		}
		else if (new_alive && old_alive)
		{
			Assert(changes[i].newStatus & FTS_STATUS_CHANGELOGGING);

			/* this is a segment that may require a mode-change */
			if (old_pri && !new_pri)
			{
				/* demote primary to mirror ?! Nothing to do here. */
			}
			else if (new_pri && !old_pri)
			{
				/* promote mirror to primary */
				Filerep_mode_update(changes[i].dbid, "primary", 'c', FilerepModeUpdateLoggingEnum_MirrorToChangeTracking);
			}
			else if (old_pri && new_pri)
			{
				/* convert primary to changetracking */
				Filerep_mode_update(changes[i].dbid, "primary", 'c', FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking);
			}
		}
	}
}

static void
Filerep_get_hosts_by_dbid(int dbid, char **hostname_p, int *host_port_p, int *host_filerep_port_p, char **peer_name_p,
			int *peer_pm_port_p, int *peer_filerep_port_p)
{
	bool found;
	int i;
	int content_id=-1;

	Assert(dbid >= 0);
	Assert(hostname_p != NULL);
	Assert(host_port_p != NULL);
	Assert(host_filerep_port_p != NULL);
	Assert(peer_name_p != NULL);
	Assert(peer_pm_port_p != NULL);
	Assert(peer_filerep_port_p != NULL);

	found = false;

	/*
	 * We're going to scan the segment-dbs array twice.
	 *
	 * On the first pass we get our dbid, on the second pass we get our mirror-peer.
	 */
 	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (segInfo->dbid == dbid)
		{
			*hostname_p = segInfo->address;
			*host_port_p = segInfo->port;
			*host_filerep_port_p = segInfo->filerep_port;
			content_id = segInfo->segindex;
			found = true;
			break;
		}
	}

	if (!found)
	{
		elog(FATAL, "Filerep_get_hosts_by_dbid: could not find entry for dbid %d", dbid);
	}

	found = false;

	/* second pass, find the mirror-peer */
 	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (segInfo->segindex == content_id && segInfo->dbid != dbid)
		{
			*peer_name_p = segInfo->address;
			*peer_pm_port_p = segInfo->port;
			*peer_filerep_port_p = segInfo->filerep_port;
			found = true;
			break;
		}
	}

	if (!found)
	{
		elog(LOG, "Could not find mirror-peer for dbid %d", dbid);
		*peer_name_p = NULL;
		*peer_pm_port_p = -1;
		*peer_filerep_port_p = -1;
	}
}

static void
Filerep_mode_update(int dbid, char *mode, char status, FilerepModeUpdateLoggingEnum logMsgToSend)
{
	int f = -1;
	char tmp_filename[35];

	ssize_t res;

	char port_buf[10];
	char *seg_addr = NULL;
	char *peer_addr = NULL;
	int seg_pm_port = -1, seg_rep_port = -1, peer_rep_port = -1, peer_pm_port;

	Assert(dbid >= 0);
	Assert(mode != NULL);

	Filerep_get_hosts_by_dbid(dbid, &seg_addr, &seg_pm_port, &seg_rep_port, &peer_addr, &peer_pm_port, &peer_rep_port);

	snprintf(tmp_filename, sizeof(tmp_filename), "/tmp/ftsprobe_command_XXXXXX");
	f = mkstemp(tmp_filename);
	if (f < 0)
	{
		elog(FATAL, "Failed to create command file '%s': %m", tmp_filename);
	}

	switch (logMsgToSend)
	{
		case FilerepModeUpdateLoggingEnum_MirrorToChangeTracking:
			ereport(LOG,
					(errmsg("Mirror segment on %s:%d is taking over as primary in change tracking mode.",
							seg_addr, seg_pm_port ),
					 errSendAlert(true)));
			break;
		case FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking:
			ereport(LOG,
					(errmsg("Primary segment on %s:%d is transitioning into change tracking mode (peer marked down).",
							seg_addr, seg_pm_port ),
					 errSendAlert(true)));
			break;
	}

	if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
		elog(LOG, "Filerep_Failover: tmp filename is '%s'", tmp_filename);

	do
	{
#define FILEREP_WRITE(res, f, buf) {int len = strlen(buf); res = write(f, buf, len); if (res != len) break;}

		FILEREP_WRITE(res, f, mode);
		FILEREP_WRITE(res, f, "\n");

		res = write(f, &status, 1);
		if (res != 1)
			break;

		FILEREP_WRITE(res, f, "\n");
		FILEREP_WRITE(res, f, seg_addr);
		FILEREP_WRITE(res, f, "\n");

		snprintf(port_buf, sizeof(port_buf), "%d", seg_rep_port);

		FILEREP_WRITE(res, f, port_buf);
		FILEREP_WRITE(res, f, "\n");

		FILEREP_WRITE(res, f, peer_addr);
		FILEREP_WRITE(res, f, "\n");

		snprintf(port_buf, sizeof(port_buf), "%d", peer_rep_port);
		FILEREP_WRITE(res, f, port_buf);
		FILEREP_WRITE(res, f, "\n");

		snprintf(port_buf, sizeof(port_buf), "%d", peer_pm_port);
		FILEREP_WRITE(res, f, port_buf);
		FILEREP_WRITE(res, f, "\n");

		close(f);

		/* issue the command to change modes */
		{
			char	cmd[1024];
			int		cmd_status;

			snprintf(cmd, sizeof(cmd), "gp_primarymirror -h %s -p %d -i %s", seg_addr, seg_pm_port, tmp_filename);

			if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
				elog(LOG, "gp_primarymirror command is [%s]", cmd);

			cmd_status = system(cmd);

			unlink(tmp_filename);

			if (cmd_status == -1)
			{
				elog(ERROR, "Could not issue command: %m");
			}
			else if (cmd_status != 0)
			{
				elog(ERROR, "gp_primarymirror failed!");
			}
		}

		switch (logMsgToSend)
		{
			case FilerepModeUpdateLoggingEnum_MirrorToChangeTracking:
				ereport(LOG,
						(errmsg("Mirror segment on %s:%d promoted to primary in change tracking mode.",
								seg_addr, seg_pm_port ),
						 errSendAlert(true)));
				break;
			case FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking:
				ereport(LOG,
						(errmsg("Primary segment on %s:%d transitioned to change tracking mode successfully.",
								seg_addr, seg_pm_port ),
						 errSendAlert(true)));
				break;
		}

		return;
	} while (0);

	/* An error occurred while writing the command-file */

	close(f);
	unlink(tmp_filename);
	elog(FATAL, "Could not write to %s", tmp_filename);
}

/*
 * Run SQL query on a previously established self-connection.
 */
static void
executeEntrySQL(PGconn *entryConn, const char *query)
{
	PGresult   *rs = NULL;

	rs = PQexec(entryConn, query);
	if (PQresultStatus(rs) != PGRES_COMMAND_OK)
	{
		char	*error_message = strdup(PQerrorMessage(entryConn));

		if (!error_message)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("FTS: out of memory")));

		PQclear(rs);
		PQfinish(entryConn);
		entryConn = NULL;

		elog(FATAL, "FTS: connection to entry-DB could not commit: %s", error_message);
	}
}

/**
 * Marks the given db as in-sync in the segment configuration.
 */
static void
markSegmentInSyncInConfiguration(CdbComponentDatabaseInfo *primary, CdbComponentDatabaseInfo *mirror)
{
	Assert(FTS_STATUS_ISALIVE(primary->dbid, ftsProbeInfo->fts_status));
	Assert(FTS_STATUS_ISPRIMARY(primary->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_ISPRIMARY(mirror->dbid, ftsProbeInfo->fts_status));
	Assert(FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_IS_SYNCED(primary->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_IS_CHANGELOGGING(primary->dbid, ftsProbeInfo->fts_status));
	Assert(!FTS_STATUS_IS_CHANGELOGGING(mirror->dbid, ftsProbeInfo->fts_status));

	char cmd[SQL_CMD_BUF_SIZE];
	char timestamp_str[TIMESTAMP_BUF_SIZE];

	get_timestamp(timestamp_str, sizeof(timestamp_str));

	CdbComponentDatabaseInfo *entryDB = &cdb_component_dbs->entry_db_info[0];

	if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
		elog(LOG, "markSegmentInSyncInConfiguration: Updating segment to in-sync");

	if (entryDB->dbid != GpIdentity.dbid)
	{
		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "ftsProbe: advancing to second entryDB.");
		entryDB = entryDB + 1;
	}

	PGconn *entryConn = openSelfConn(entryDB);
	Assert(entryConn != NULL);

	ftsLock();

	if (ftsProbeInfo->fts_pauseProbes)
	{
		/* we've gotten a pause request --> ignore our probe results. */
	}
	else
	{
		uint8	segStatus=0;

		/* update primary */
		segStatus = ftsProbeInfo->fts_status[primary->dbid];
		segStatus |= FTS_STATUS_SYNCHRONIZED;
		ftsProbeInfo->fts_status[primary->dbid] = segStatus;

		/* update mirror */
		segStatus = ftsProbeInfo->fts_status[mirror->dbid];
		segStatus |= FTS_STATUS_SYNCHRONIZED;
		ftsProbeInfo->fts_status[mirror->dbid] = segStatus;

		/* update gp_segment_configuration to insync */
		snprintf(cmd, sizeof(cmd),
				 "update %s set mode='s' where dbid=CAST(%d AS SMALLINT)",
				 GpSegmentConfigRelationName,
				 primary->dbid);
		executeEntrySQL(entryConn, cmd);

		snprintf(cmd, sizeof(cmd),
				 "update %s set mode='s' where dbid=CAST(%d AS SMALLINT)",
				 GpSegmentConfigRelationName,
				 mirror->dbid);
		executeEntrySQL(entryConn, cmd);

		/* update configuration history */
		snprintf(cmd, sizeof(cmd),
				 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: changed segment to insync from resync.')",
				 timestamp_str, primary->dbid);
		executeEntrySQL(entryConn, cmd);

		snprintf(cmd, sizeof(cmd),
				 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: changed segment to insync from resync.')",
				 timestamp_str, mirror->dbid);
		executeEntrySQL(entryConn, cmd);

		ereport(LOG,
				(errmsg("Resynchronization of mirror segment on %s:%d has completed.",
						mirror->address, mirror->port ),
				 errSendAlert(true)));
	}

	closeSelfConn(entryConn);
	ftsUnlock();
}

static void
SAN_Failover(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str)
{
	char cmd[256];
	List *mps=NIL;
	ListCell *cell;
	int i;

	if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
	{
		elog(LOG, "SAN_Failover: got %d changes", changeCount);

		dumpChanges(changes, changeCount);
	}

	/*
	 * For each failed segment:
	 *   1) Find the mount-point.
	 *   2) Clean it up on the primary (if possible).
	 *   3) Mount it on the failover.
	 *   4) Start all segments using the mount-point on their failover location.
	 */
	for (i=0; i < changeCount; i++)
	{
		bool new_alive, old_alive, new_pri;

		new_alive = (changes[i].newStatus & FTS_STATUS_ALIVE ? true : false);
		old_alive = (changes[i].oldStatus & FTS_STATUS_ALIVE ? true : false);

		new_pri = (changes[i].newStatus & FTS_STATUS_PRIMARY ? true : false);

		if (!new_alive && old_alive)
		{
			/* dead marked as new primary means double-failure -- skip */
			if (new_pri)
				continue;

			getMounts_forDbid(changes[i].dbid, &mps);
			/* mps is our deduped list of mountpoints */

			Assert(list_length(mps) > 0);
		}
	}

	/* fix up the mountpoints (the list has no duplicates) */
	foreach(cell, mps)
	{
		char	new_active_host;
		int		mpid = lfirst_int(cell);

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "mpid %d", mpid);

		new_active_host = mountMaintenance_forMpid(mpid);

		snprintf(cmd, sizeof(cmd), "update %s set active_host = '%c' where mountid=%d",
				 GpSanConfigRelationName, new_active_host, mpid);

		executeEntrySQL(entryConn, cmd);

		snprintf(cmd, sizeof(cmd),
				 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: changed active host for mount-id %d to %c')",
				 time_str, -1, mpid, new_active_host);

		executeEntrySQL(entryConn, cmd);
	}

	/* Now we're ready to start up the failover segments */
	for (i=0; i < changeCount; i++)
	{
		bool new_alive, new_pri, old_pri;

		new_pri = (changes[i].newStatus & FTS_STATUS_PRIMARY ? true : false);
		old_pri = (changes[i].oldStatus & FTS_STATUS_PRIMARY ? true : false);

		new_alive = (changes[i].newStatus & FTS_STATUS_ALIVE ? true : false);

		/* dead marked as new primary means double-failure -- skip */
		if (new_pri && !new_alive)
			continue;

		if (new_pri && !old_pri)
		{
			startSegment_forDbid(changes[i].dbid);

			snprintf(cmd, sizeof(cmd),
					 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: started segment moving mountpoints.')",
					 time_str, changes[i].dbid);
			executeEntrySQL(entryConn, cmd);
		}
	}
}

/*
 * Generate a list of the mountpoints that we have to work on.
 * Duplicates are eliminated by using list_append_unique_int()
 */
static void
getMounts_forDbid(int dbid, List **mountp)
{
	Relation	conf_rel;
	HeapTuple	conf_tup;
	HeapScanDesc conf_scan;
	bool	isNull=true;

	ScanKeyData key[1];

	Assert(dbid > 0);

	ScanKeyInit(&key[0],
				Anum_gp_segment_configuration_dbid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum((int16)dbid));

	conf_rel = heap_open(GpSegmentConfigRelationId, AccessShareLock);
	conf_scan = heap_beginscan(conf_rel, SnapshotNow, 1, key);

	while (HeapTupleIsValid(conf_tup = heap_getnext(conf_scan, ForwardScanDirection)))
	{
		int i;
		int2vector *san_mounts_v;
		Datum	tup_dbid;
		Datum	conf_datum;

		tup_dbid = heap_getattr(conf_tup, Anum_gp_segment_configuration_dbid, conf_rel->rd_att, &isNull);
		Assert(!isNull); /* primary key better not be null ! */

		if ((int)DatumGetInt16(tup_dbid) != dbid)
		{
			elog(ERROR, "getMounts_forDbid: failed dbid lookup in segment configuration");
		}

		/* got our dbid, get our array of mountpoints. */
		conf_datum = heap_getattr(conf_tup, Anum_gp_segment_configuration_san_mounts, conf_rel->rd_att, &isNull);

		if (isNull)
			continue; /* Can this happen ? */

		san_mounts_v = DatumGetPointer(conf_datum);
		Assert(san_mounts_v != NULL);

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "Got our vector of mountpoints length %d", san_mounts_v->dim1);
		for (i=0; i < san_mounts_v->dim1; i++)
		{
			*mountp = list_append_unique_int(*mountp, san_mounts_v->values[i]);
		}
		break;
	}

	heap_endscan(conf_scan);
	heap_close(conf_rel, AccessShareLock);
}

/* convenience macro for sucking catalog info out of our table */
#define get_heap_attr(datum, prefix, ATTR, tuple, rel)					\
	{																	\
		bool isNull=true;												\
																		\
		datum = heap_getattr((tuple), prefix ## ATTR , (rel)->rd_att, &isNull); \
		if (isNull)														\
			elog(FATAL, "Fault reponse error: %s IS NULL", #ATTR);		\
	}

#define get_config_attr(datum, ATTR, tuple, rel) get_heap_attr(datum, Anum_gp_segment_configuration_, ATTR, tuple, rel)

static void
startSegment_forDbid(int dbid)
{
	Relation	conf_rel, fse_rel;
	HeapTuple	conf_tup, fse_tup;
	HeapScanDesc conf_scan, fse_scan;
	ScanKeyData  fse_scankey[2];

	ScanKeyData key[1];

	Assert(dbid > 0);

	ScanKeyInit(&key[0],
				Anum_gp_segment_configuration_dbid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum((int16)dbid));

	conf_rel = heap_open(GpSegmentConfigRelationId, AccessShareLock);
	fse_rel  = heap_open(FileSpaceEntryRelationId, AccessShareLock);
	conf_scan = heap_beginscan(conf_rel, SnapshotNow, 1, key);

	while (HeapTupleIsValid(conf_tup = heap_getnext(conf_scan, ForwardScanDirection)))
	{
		Datum	conf_datum;
		char	*gphome_env=NULL;
		char	*host;
		char	*dir;
		int4	port;
		int2	content_id;
		char	cmd[1024];

		get_config_attr(conf_datum, dbid, conf_tup, conf_rel);

		if ((int)DatumGetInt16(conf_datum) != dbid)
		{
			elog(ERROR, "startSegment_forDbid: failed dbid lookup in segment configuration");
		}

		/* got our dbid, get our array of mountpoints. */
		get_config_attr(conf_datum, address, conf_tup, conf_rel);
		host = DatumGetCString(DirectFunctionCall1(textout, conf_datum));

		get_config_attr(conf_datum, content, conf_tup, conf_rel);
		content_id = DatumGetInt16(conf_datum);

		get_config_attr(conf_datum, port, conf_tup, conf_rel);
		port = DatumGetInt32(conf_datum);

		/* Find the set of data-directories for all filespaces */
		/* Note: currently only fetches the pg_system filespace */
		ScanKeyInit(&fse_scankey[0], 
					Anum_pg_filespace_entry_fsedbid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int16GetDatum((int2)dbid));  /* Current dbid */
		ScanKeyInit(&fse_scankey[1], 
					Anum_pg_filespace_entry_fsefsoid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(SYSTEMFILESPACE_OID));  /* pg_system filespace */
		fse_scan = heap_beginscan(fse_rel, SnapshotNow, 2, fse_scankey);
		fse_tup = heap_getnext(fse_scan, ForwardScanDirection);

		if (!HeapTupleIsValid(fse_tup))
			elog(ERROR, "filespace lookup failure for dbid %d", dbid);

		get_heap_attr(conf_datum, Anum_pg_filespace_entry_, fselocation, fse_tup, fse_rel);
		dir = DatumGetCString(DirectFunctionCall1(textout, conf_datum));

		heap_endscan(fse_scan);

		/* Startup the segment */
		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
			elog(LOG, "Starting %s:%s:%d", host, dir, port);

		/*
		 * NOTE: we expect every segment to have GPHOME set to the
		 * same directory as the master -- this is also assumed by the
		 * management scripts.
		 */
		/*
		  GPHOME=os.environ.get('GPHOME')
		  SRC_GPPATH=". %s/greenplum_path.sh;" % GPHOME
		*/
		gphome_env = getenv("GPHOME");

		if (gphome_env == NULL)
		{
			elog(LOG, "Failed to find GPHOME, sending our pg_ctl without path.");
			/* we don't have "gphome" ? let's try this anyhow and hope for the best. */
			snprintf(cmd, sizeof(cmd), "ssh %s \"pg_ctl -w -D %s -l %s/pg_log/startup.log -o '-i -p %d --silent-mode=true -M mirrorless -b %d -C %d -z %d' start 2>&1 \" ",
					 host, dir, dir, port, dbid, content_id, GpIdentity.numsegments);
		}
		else
		{
			snprintf(cmd, sizeof(cmd), "ssh %s \"source %s/greenplum_path.sh; %s/bin/pg_ctl -w -D %s -l %s/pg_log/startup.log -o '-i -p %d --silent-mode=true -M mirrorless -b %d -C %d -z %d' start 2>&1 \" ",
					 host, gphome_env, gphome_env, dir, dir, port, dbid, content_id, GpIdentity.numsegments);
		}

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "command is [%s]", cmd);
		system(cmd);

		ereport(LOG,
			(errmsg("SAN mirror segment on %s:%d is taking over as primary.",
				host, port ),
			 errSendAlert(true)));

		break;
	}

	heap_endscan(conf_scan);
	heap_close(conf_rel, AccessShareLock);
}

static char
mountMaintenance_forMpid(int mpid)
{
	char		ret='p';
	Relation	mount_rel;
	HeapTuple	mount_tup;
	HeapScanDesc mount_scan;

	ScanKeyData key[1];

	ScanKeyInit(&key[0],
				Anum_gp_san_configuration_mountid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum((int16)mpid));

	mount_rel = heap_open(GpSanConfigRelationId, AccessShareLock);
	mount_scan = heap_beginscan(mount_rel, SnapshotNow, 1, key);

	while (HeapTupleIsValid(mount_tup = heap_getnext(mount_scan, ForwardScanDirection)))
	{
		Datum	mount_datum;

		char	active_host;
		char	san_type;

		char	*p_host, *p_mp, *p_dev;
		char	*m_host, *m_mp, *m_dev;

#define get_mount_attr(datum, ATTR, tuple, rel) get_heap_attr(datum, Anum_gp_san_configuration_, ATTR, tuple, rel)

		get_mount_attr(mount_datum, mountid, mount_tup, mount_rel);
		if ((int)DatumGetInt16(mount_datum) != mpid)
		{
			elog(ERROR, "getMounts_forDbid: failed dbid lookup in segment configuration");
		}

		/* got our row, now pull out our columns */
		get_mount_attr(mount_datum, active_host, mount_tup, mount_rel);
		active_host = DatumGetChar(mount_datum);

		get_mount_attr(mount_datum, san_type, mount_tup, mount_rel);
		san_type = DatumGetChar(mount_datum);

		/* get info for the primary */
		get_mount_attr(mount_datum, primary_host, mount_tup, mount_rel);
		p_host = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		get_mount_attr(mount_datum, primary_mountpoint, mount_tup, mount_rel);
		p_mp = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		get_mount_attr(mount_datum, primary_device, mount_tup, mount_rel);
		p_dev = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		/* get info for the mirror */
		get_mount_attr(mount_datum, mirror_host, mount_tup, mount_rel);
		m_host = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		get_mount_attr(mount_datum, mirror_mountpoint, mount_tup, mount_rel);
		m_mp = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		get_mount_attr(mount_datum, mirror_device, mount_tup, mount_rel);
		m_dev = DatumGetCString(DirectFunctionCall1(textout, mount_datum));

		elog(LOG, "active %c type %c primary %s:%s:'%s' mirror %s:%s:'%s'", active_host, san_type,
			 p_host, p_mp, p_dev, m_host, m_mp, m_dev);

		{
			char	*gphome_env=NULL;
			char	cmd[1024];
			int		cmd_status;

			/*
			 * NOTE: we expect every segment to have GPHOME set to the
			 * same directory as the master -- this is also assumed by the
			 * management scripts.
			 */
			/*
			  GPHOME=os.environ.get('GPHOME')
			  SRC_GPPATH=". %s/greenplum_path.sh;" % GPHOME
			*/
			gphome_env = getenv("GPHOME");

			if (gphome_env == NULL)
			{
				snprintf(cmd, sizeof(cmd), "gp_mount_agent --agent -t %c -a %c -p %s -d %s -m %s -q %s -e %s -n %s",
						 san_type, active_host, p_host, p_dev, p_mp, m_host, m_dev, m_mp);
			}
			else
			{
				snprintf(cmd, sizeof(cmd), "%s/bin/gp_mount_agent --agent -t %c -a %c -p %s -d %s -m %s -q %s -e %s -n %s",
						 gphome_env, san_type, active_host, p_host, p_dev, p_mp, m_host, m_dev, m_mp);
			}

			elog(LOG, "Mount agent command is [%s]", cmd);

			cmd_status = system(cmd);

			if (cmd_status == -1)
			{
				elog(ERROR, "Could not issue command: %m");
				/* not reached */
			}
			else if (cmd_status != 0)
			{
				elog(ERROR, "gp_mount_agent failed!");
				/* not reached */
			}
		}

		ret = (active_host == 'p') ? 'm' : 'p';

		break;
	}

	heap_endscan(mount_scan);
	heap_close(mount_rel, AccessShareLock);

	return ret;
}
