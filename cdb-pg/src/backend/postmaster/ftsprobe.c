/*-------------------------------------------------------------------------
 *
 * ftsprobe.c
 *	  Process under QD postmaster polls the segments on a periodic basis
 * or at the behest of QEs.
 *
 * Maintains a bitmap in shared memory of the up-ness of each segment.
 *
 * Copyright (c) 2006-2008, Greenplum inc
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
#include "utils/ps_status.h"

#include "utils/relcache.h"
#include "utils/syscache.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "catalog/catalog.h"

#include "pgtime.h"

/*
 * FUNCTION PROTOTYPES
 */

#ifdef EXEC_BACKEND
static pid_t ftsprobe_forkexec(void);
#endif
NON_EXEC_STATIC void FtsProbeMain(int argc, char *argv[]);

static void FtsProbeLoop(void);
static bool FtsTestOneSegment(CdbComponentDatabaseInfo *failedDBInfo, char *dbName, char *userName);

static void retrieveUserAndDb(char **probeUser, char **probeDatabase);
static bool probeSegments(CdbComponentDatabases *cdb_component_dbs);

static void *probeSegmentFromThread(void *arg);

static bool am_ftsprobe = false;

static volatile bool shutdown_requested = false;
static volatile bool rescan_requested = false;

static char *probeUser = NULL;
static char *probeDatabase = NULL;

/*
 * macros for manipulations of fts-probe bitmaps
 * bitmaps are based on uint64.
 *
 * Note: it would be nice if we had atomic bit-ops for the
 * multithreaded probe, but we when manipulating these from
 * multithreaded code it is important to have a lock.
 */
#define FTS_BM_WORD_COUNT ((FTS_MAX_DBS) / 64) /* one bit per db, 64-bit "words" */
#define FTS_BM_WORD(id) ((id)>>6)
#define FTS_BM_BIT(id) ((uint64)1 << ((id) & 0x3f))

#if 0
#define FTS_BM_SET(id, bm)							\
	do {											\
		uint64 *bma;								\
		uint64 w;									\
		int index;									\
													\
		index = FTS_BM_WORD(id);					\
		bma = (uint64 *)(bm);						\
		w = bma[index];								\
		w |= FTS_BM_BIT(id);						\
		bma[index] = w;								\
	} while (0)

#define FTS_BM_CLR(id, bm)							\
	do {											\
		uint64 *bma;								\
		uint64 w;									\
		int index;									\
													\
		index = FTS_BM_WORD(id);					\
		bma = (uint64 *)(bm);						\
		w = bma[index];								\
		w &= ~FTS_BM_BIT(id);						\
		bma[index] = w;								\
	} while (0)
#endif

#define FTS_BM_SET(id, bm) ((bm)[FTS_BM_WORD(id)] = ((bm)[FTS_BM_WORD(id)]) | FTS_BM_BIT(id))
#define FTS_BM_CLR(id, bm) ((bm)[FTS_BM_WORD(id)] &= ~FTS_BM_BIT(id))

#define FTS_BM_TEST(id, bm) (((bm)[FTS_BM_WORD(id)] & FTS_BM_BIT(id)) ? true : false)

typedef uint64 fts_bitmap_t[FTS_BM_WORD_COUNT];

static bool probePublishUpdateToSharedMemory(CdbComponentDatabases *cdb_component_dbs,  fts_bitmap_t map);

struct segmentStatusChange {
	int	dbid;
	uint8 oldStatus;
	uint8 newStatus;
	bool persist;
};

static PGconn *openSelfConn(CdbComponentDatabaseInfo *entryDB);
static void closeSelfConn(PGconn *conn);
static void addChangeHistory(PGconn *conn, struct segmentStatusChange *change, char *time_str);
static void probeUpdateConfig(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

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
 * fp_quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
fp_quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * DO NOT proc_exit() -- we're here because shared memory may be
	 * corrupted, so we don't want to try to clean up our transaction. Just
	 * nail the windows shut and get out of town.
	 *
	 * Note we do exit(2) not exit(0).	This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.
	 */
	exit(2);
}

/*
 * AutoVacMain
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
	pqsignal(SIGQUIT, fp_quickdie);
	pqsignal(SIGALRM, handle_sig_alarm);

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, CatchupInterruptHandler);
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

	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	SetDatabasePath(fullpath);

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
	retrieveUserAndDb(&probeUser, &probeDatabase);

	elog(LOG, "probeUser %s probeDatabase %s", probeUser, probeDatabase);
	
	/* main loop */
	FtsProbeLoop();

	/* One iteration done, go away */
	proc_exit(0);
}

void FtsProbeLoop(void)
{
	struct timeval tv;
	int		i;
	bool	updated_bitmap, processing_fullscan;
	CdbComponentDatabases *cdb_component_dbs = NULL;

	tv.tv_usec = 0;
	tv.tv_sec = gp_fts_probe_interval;

	cdb_component_dbs = getCdbComponentInfo(false);
	ftsLock();
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];
		uint8	segStatus;

		segStatus = 0;
		if (segInfo->valid)
			segStatus |= FTS_STATUS_ALIVE;
		if (segInfo->isprimary)
			segStatus |= FTS_STATUS_PRIMARY;
		if (segInfo->definedprimary)
			segStatus |= FTS_STATUS_DEFINEDPRIMARY;

		ftsProbeInfo->fts_status[segInfo->dbid] = segStatus;
	}
	ftsUnlock();

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (shutdown_requested)
			break;
		
		/* no need to live on if postmaster has died */
		if (!PostmasterIsAlive(true))
			exit(1);

		if (cdb_component_dbs != NULL)
		{
			freeCdbComponentDatabases(cdb_component_dbs);
			cdb_component_dbs = NULL;
		}

		if (ftsProbeInfo->fts_probeScanRequested == ftsProbeInfo->fts_statusVersion)
			processing_fullscan = true;
		else
			processing_fullscan = false;

		cdb_component_dbs = getCdbComponentInfo(false);

		elog(DEBUG3, "Ftsprober: starting %sscan with %d dbids with %d content-ids",
			 (processing_fullscan ? "full " : ""),
			 cdb_component_dbs->total_segment_dbs,
			 cdb_component_dbs->total_segments);

		/* Start a sequential, slow scan of the segments */
		updated_bitmap = probeSegments(cdb_component_dbs);

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
		if (ftsProbeInfo->fts_probeScanRequested < ftsProbeInfo->fts_statusVersion)
		{
			tv.tv_usec = 0;
			tv.tv_sec = gp_fts_probe_interval;

			select(0, NULL, NULL, NULL, &tv); /* don't care about return value. */
		}
		else
			elog(LOG, "skipping sleep -- rescanning (%d < %d)",
				 (int)ftsProbeInfo->fts_probeScanRequested, (int)ftsProbeInfo->fts_statusVersion);

	} /* end server loop */

	return;
}

/*
 * This is called from several different threads: ONLY USE THREADSAFE FUNCTIONS INSIDE.
 */
static bool
FtsTestOneSegment(CdbComponentDatabaseInfo *dbInfo, char *dbName, char *userName)
{
   	PGconn		*conn;
	char		*errorMsg;
	int			msgLen;
	bool		ret = true;
	PQExpBuffer	buffer = createPQExpBuffer();

	/*
	 * This connects in UTILITY mode: we assume that if we detect a
	 * fault using a utility-mode connection that we're detecting
	 * something that the execute connections would also care about.
	 */
	initPQExpBuffer(buffer);

	/*
	 * Build the connection string
	 */
	appendPQExpBuffer(buffer, "options='-c gp_session_role=UTILITY' ");

	if (dbInfo->hostname == NULL)
		appendPQExpBufferStr(buffer, "host='' ");
	else if (isdigit(dbInfo->hostname[0]))
		appendPQExpBuffer(buffer, "hostaddr=%s ", dbInfo->hostname);
	else
		appendPQExpBuffer(buffer, "host=%s ", dbInfo->hostname);

	appendPQExpBuffer(buffer, "port=%u ", dbInfo->port);
	appendPQExpBuffer(buffer, "dbname=%s ", dbName);
	appendPQExpBuffer(buffer, "user=%s ", userName);
	appendPQExpBuffer(buffer, "connect_timeout=%d ", gp_segment_connect_timeout);

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
						  dbInfo->dbid, dbName, PQerrorMessage(conn), PQstatus(conn));
			}
			ret = true;
			break;
		}

		errorMsg = PQerrorMessage(conn);
		if (errorMsg == NULL)
		{
			if (LOG >= log_min_messages)
				write_log("FTS: test connection can't parse error message");
			errorMsg = "";
		}

		msgLen = strlen(errorMsg);

		if (DEBUG3 >= log_min_messages)
		{
			write_log("FtsTestConnection: connection failed with message ('%s':%d)",
					  errorMsg, msgLen);
		}

		/*
		 * If we get a network error (do we really need a
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
		 *
		 * MPP-5751: some timeouts (real failures) fail to set a real
		 * message, so we check msgLen for that case. If we don't have
		 * a message, assume that this is a real failure.
		 */
		if (msgLen > 0 &&
			strstr(errorMsg, "FATAL:") == NULL &&
			strstr(errorMsg, "could not ") == NULL &&
			strcmp(errorMsg, "timeout expired\n") != 0)
		{
			/* non-fatal connection errors are ignored */
			if (DEBUG3 >= log_min_messages)
				write_log("FtsTestConnection: nonfatal error, ignoring (%d, '%s':%d)", PQsocket(conn), errorMsg, msgLen);

			ret = true;
			break;
		}

		if (strstr(errorMsg, "the database system is starting up") != NULL)
		{
			/* don't consider this a failure, essentially we want to force a retry here */
			ret = true;
			break;
		}

		/*
		 * MPP-4980 (and others): These are just indications that the
		 * system is busy, not that we're down.
		 */
		if (strstr(errorMsg, "Out of memory") != NULL ||
			strstr(errorMsg, "connection limit exceeded for non-superusers") != NULL ||
			strstr(errorMsg, "too many clients already") != NULL)
		{
			/* don't consider this a failure, essentially we want to force a retry here */
			ret = true;
			break;
		}

		/* This is a real failure for which a reconfigure may be warranted */
		if (LOG >= log_min_messages)
			write_log("FTS: test connection failed, dbid %d dbname %s error message '%s':%d",
					  dbInfo->dbid, dbName, errorMsg, msgLen);

		ret = false;
	} while (0);

	PQfinish(conn);
	free(buffer->data);
	return ret;
}

static void
retrieveUserAndDb(char **probeUser, char **probeDatabase)
{
	Relation auth_rel;
	HeapTuple	auth_tup;
	HeapScanDesc auth_scan;

	Relation db_rel;
	HeapTuple	db_tup;
	HeapScanDesc db_scan;

	bool	isNull;

	Assert(probeUser != NULL);
	Assert(probeDatabase != NULL);

	auth_rel = heap_open(AuthIdRelationId, AccessShareLock);
	auth_scan = heap_beginscan(auth_rel, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(auth_tup = heap_getnext(auth_scan, ForwardScanDirection)))
	{
		Datum	attrName;
		Datum	attrNameOid;
		Datum	attrSuper;
		bool	is_super;
		Oid		userOid;

		attrSuper = heap_getattr(auth_tup, Anum_pg_authid_rolsuper, auth_rel->rd_att, &isNull);
		Assert(!isNull);
		is_super = DatumGetBool(attrSuper);

		if (is_super)
		{
			attrName = heap_getattr(auth_tup, Anum_pg_authid_rolname, auth_rel->rd_att, &isNull);
			Assert(!isNull);
			*probeUser = pstrdup(DatumGetCString(attrName));

			attrNameOid = heap_getattr(auth_tup, ObjectIdAttributeNumber, auth_rel->rd_att, &isNull);
			Assert(!isNull);
			userOid = DatumGetObjectId(attrNameOid);

			SetSessionUserId(userOid, true);

			break;
		}
	}
	heap_endscan(auth_scan);
	heap_close(auth_rel, AccessShareLock);

	Assert(*probeUser != NULL);

	db_rel = heap_open(DatabaseRelationId, AccessShareLock);
	db_scan = heap_beginscan(db_rel, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(db_tup = heap_getnext(db_scan, ForwardScanDirection)))
	{
		Datum	attrOid;
		Datum	attrName;
		Oid		db_oid;

		attrOid = heap_getattr(db_tup, ObjectIdAttributeNumber, db_rel->rd_att, &isNull);
		Assert(!isNull);
		db_oid = DatumGetObjectId(attrOid);

		if (db_oid == TemplateDbOid)
		{
			attrName = heap_getattr(db_tup, Anum_pg_database_datname, db_rel->rd_att, &isNull);
			Assert(!isNull);
			*probeDatabase = pstrdup(DatumGetCString(attrName));
		}
	}
	heap_endscan(db_scan);
	heap_close(db_rel, AccessShareLock);

	Assert(*probeDatabase != NULL);
}

pthread_t *threads=NULL;
static pthread_mutex_t	worker_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

struct threadWorkerInfo {
	uint64 *workToDo;
	uint64 *segments_alive;
	CdbComponentDatabases *dbs;
};

static uint64 segments_to_scan[2048]; /* dbid bitmap 64k segments, with mirrors */
static uint64 segments_alive[2048]; /* dbid bitmap 64k segments, with mirrors */

static bool
probeSegments(CdbComponentDatabases *cdb_component_dbs)
{
	int i;
	bool updated_bitmap = false;

	memset(segments_alive, 0, sizeof(segments_alive));

	/* single threaded */
	if (gp_fts_probe_threadcount == 1)
	{
		for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

			/* if this is worth scanning, then scan it. */
			if (segInfo->valid)
			{
				if (!FtsTestOneSegment(segInfo, probeDatabase, probeUser))
				{
					elog(LOG, "Found a fault with a segment %d segment-dbid %d", segInfo->segindex, segInfo->dbid);
					FTS_BM_CLR(segInfo->dbid, segments_alive);
				}
				else
					FTS_BM_SET(segInfo->dbid, segments_alive);
			}
		}
	}
	else
	{
		struct threadWorkerInfo worker_info;
		int pthread_err;
		int workers = gp_fts_probe_threadcount - 1;

		CdbComponentDatabaseInfo *segInfo = NULL;

		Assert(threads == NULL);

		/* figure out which segments to include in the scan. */
		for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			segInfo = &cdb_component_dbs->segment_db_info[i];

			if (segInfo->valid)
				FTS_BM_SET(segInfo->dbid, segments_to_scan);
			else
				FTS_BM_CLR(segInfo->dbid, segments_to_scan);
		}

		worker_info.workToDo = segments_to_scan;
		worker_info.segments_alive = segments_alive;
		worker_info.dbs = cdb_component_dbs;

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
	}

	/* if we're shutting down, just exit. */
	if (shutdown_requested)
		return false;

	/*
	 * Now we've completed the scan, update shared-memory. if we
	 * change anything, we return true.
	 */
	updated_bitmap = probePublishUpdateToSharedMemory(cdb_component_dbs, segments_alive);

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
		CdbComponentDatabaseInfo *segInfo = NULL;

		/*
		 * Get a work item: find first un-tested segment, mark it
		 * "tested" and unlock.
		 */
		pthread_mutex_lock(&worker_thread_mutex);
		for (; i < worker_info->dbs->total_segment_dbs; i++)
		{
			segInfo = &worker_info->dbs->segment_db_info[i];

			if (FTS_BM_TEST(segInfo->dbid, worker_info->workToDo))
			{
				FTS_BM_CLR(segInfo->dbid, worker_info->workToDo);
				break;
			}
		}
		pthread_mutex_unlock(&worker_thread_mutex);

		if (i == worker_info->dbs->total_segment_dbs)
		{
			/* no work left to do */
			break;
		}

		/* Got a segment to test, test it. */
		if (!FtsTestOneSegment(segInfo, probeDatabase, probeUser))
		{
			write_log("Found a fault with a segment %d segment-dbid %d", segInfo->segindex, segInfo->dbid);

			/* the FTS_BM_CLR/FTS_BM_SET macros potentially can stomp on each other. */
			pthread_mutex_lock(&worker_thread_mutex);

			FTS_BM_CLR(segInfo->dbid, worker_info->segments_alive);

			pthread_mutex_unlock(&worker_thread_mutex);
		}
		else
		{
			pthread_mutex_lock(&worker_thread_mutex);

			FTS_BM_SET(segInfo->dbid, worker_info->segments_alive);

			pthread_mutex_unlock(&worker_thread_mutex);
		}
	}

	return NULL;
}

static CdbComponentDatabaseInfo *
getMirrorEntryByContent(CdbComponentDatabases *cdb_component_dbs, int content, int dbid)
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

static bool
probePublishUpdateToSharedMemory(CdbComponentDatabases *cdb_component_dbs,  fts_bitmap_t map)
{
	bool at_least_one_failure=false;
	bool updated_bitmap=false;
	int i;

	struct segmentStatusChange *changes=NULL;
	int changeEntries=0;

	PGconn *entryConn = NULL;

	CdbComponentDatabaseInfo *primary;
	CdbComponentDatabaseInfo *mirror;

	char timestamp_str[100];

	/* collect status changes */
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (segInfo->valid)
		{
			int primaryChange;
			int mirrorChange;
			uint8 segStatus;

			if (!FTS_BM_TEST(segInfo->dbid, segments_alive))
			{
				at_least_one_failure = true;

				/* segment is down. was it already marked down ? */
				if (FTS_STATUS_ISALIVE(segInfo->dbid, ftsProbeInfo->fts_status))
				{
					/* we're going to change the status */
					primaryChange = changeEntries;
					if (changes)
						changes = (struct segmentStatusChange *)
							repalloc(changes, (changeEntries + 1)*sizeof(struct segmentStatusChange));
					else
						changes = (struct segmentStatusChange *)palloc(sizeof(struct segmentStatusChange));
					changeEntries++;

					primary = segInfo;
					mirror = getMirrorEntryByContent(cdb_component_dbs, segInfo->segindex, segInfo->dbid);

					segStatus = ftsProbeInfo->fts_status[segInfo->dbid];
					changes[primaryChange].dbid = segInfo->dbid;
					changes[primaryChange].oldStatus = segStatus;
					changes[primaryChange].newStatus = segStatus & ~FTS_STATUS_ALIVE;

					if (*ftsAction == FTS_READONLY)
					{
						/*
						 * In readonly mode, we never persistently mark segments down
						 */
						changes[primaryChange].persist = false;
					}
					else
					{
						/*
						 * MPP-5135, MPP-1601: we don't want to mark the segment down if:
						 *   1) there is no mirror
						 *   2) there is a mirror, but the mirror is already marked down.
						 */
						if (mirror != NULL &&
							FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status))
						{
							changes[primaryChange].persist = true;
						}
						else
							changes[primaryChange].persist = false;
					}

					if (mirror != NULL &&
						segStatus & FTS_STATUS_PRIMARY &&
						segStatus & FTS_STATUS_DEFINEDPRIMARY)
					{
						if (!FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status))
						{
							/* mirror is down! */
						}
						else
						{
							/* update the mirror too */
							mirrorChange = changeEntries;
							changes = (struct segmentStatusChange *)
								repalloc(changes, (changeEntries + 1)*sizeof(struct segmentStatusChange));
							changeEntries++;

							segStatus = ftsProbeInfo->fts_status[mirror->dbid];
							changes[mirrorChange].dbid = mirror->dbid;
							changes[mirrorChange].oldStatus = segStatus;
							changes[mirrorChange].newStatus = segStatus | FTS_STATUS_PRIMARY;

							if (*ftsAction == FTS_READONLY)
								changes[mirrorChange].persist = false;
							else
							{
								changes[mirrorChange].persist = true;
							}

							/* update primary */
							changes[primaryChange].newStatus &= ~FTS_STATUS_PRIMARY;
						}
					}

					updated_bitmap = true;
				}
				/* was already marked down, no change. */
			}
			else
			{
				/* segment is up. if marked down in shmem, need to revert (failback to primary) */
				if (!FTS_STATUS_ISALIVE(segInfo->dbid, ftsProbeInfo->fts_status))
				{
					/*
					 * A segment is *up* but is marked down in shmem: this
					 * can only happen with fault-action readonly.
					 */
					if (*ftsAction != FTS_READONLY)
					{
						elog(LOG, "segment with dbid %d is up, but marked down in shmem (prober %d me %d)",
							 segInfo->dbid, ftsProbeInfo->fts_probePid, MyProcPid);
					}
					Assert(*ftsAction == FTS_READONLY);

					primaryChange = changeEntries;
					if (changes)
						changes = (struct segmentStatusChange *)
							repalloc(changes, (changeEntries + 1)*sizeof(struct segmentStatusChange));
					else
						changes = (struct segmentStatusChange *)palloc(sizeof(struct segmentStatusChange));
					changeEntries++;

					segStatus = ftsProbeInfo->fts_status[segInfo->dbid];
					changes[primaryChange].dbid = segInfo->dbid;
					changes[primaryChange].oldStatus = segStatus;
					changes[primaryChange].newStatus = segStatus | FTS_STATUS_ALIVE;
					changes[primaryChange].persist = false;

					primary = segInfo;
					mirror = getMirrorEntryByContent(cdb_component_dbs, segInfo->segindex, segInfo->dbid);

					if (mirror != NULL &&
						segStatus & FTS_STATUS_DEFINEDPRIMARY &&
						!(segStatus & FTS_STATUS_PRIMARY))
					{
							/* update the mirror too */
							mirrorChange = changeEntries;
							changes = (struct segmentStatusChange *)
								repalloc(changes, (changeEntries + 1)*sizeof(struct segmentStatusChange));
							changeEntries++;

							segStatus = ftsProbeInfo->fts_status[mirror->dbid];
							changes[mirrorChange].dbid = mirror->dbid;
							changes[mirrorChange].oldStatus = segStatus;
							changes[mirrorChange].newStatus = segStatus & ~FTS_STATUS_PRIMARY;
							changes[mirrorChange].persist = false;

							/* update primary */
							changes[primaryChange].newStatus |= FTS_STATUS_PRIMARY;
					}

					updated_bitmap = true;
				}
				/* was already marked up, no change */
			}
		}
		else
			at_least_one_failure = true;
	}

	/* We're going to use these timestamps below. */
	{
		pg_time_t		curtime;
		struct pg_tm	*time_tm;
		struct timeval	tv;

		gettimeofday(&tv, NULL);
		curtime = tv.tv_sec;
		time_tm = pg_gmtime(&curtime);
		pg_strftime(timestamp_str, sizeof(timestamp_str), "%+", time_tm); /* "%+" is the format used by the Unix date command */
	}

	if (changes != NULL)
	{
		CdbComponentDatabaseInfo *entryDB = &cdb_component_dbs->entry_db_info[0];

		if (entryDB->dbid != GpIdentity.dbid)
		{
			elog(LOG, "ftsProbe: advancing to second entryDB.");
			entryDB = entryDB + 1;
		}

		entryConn = openSelfConn(entryDB);
		Assert(entryConn != NULL);
	}

	/* Now we're going to apply the changelist we just generated. */
	ftsLock();

	/* first apply updates to shmem. */
	for (i=0; i < changeEntries; i++)
	{
		addChangeHistory(entryConn, &changes[i], timestamp_str);
		ftsProbeInfo->fts_status[changes[i].dbid] = changes[i].newStatus;
	}

	/*
	 * We're in read-only mode, so we need to update the global "readonly" status
	 */
	FtsUpdateROStatus(at_least_one_failure);

	/* If we're in "continue" mode, mark the heap copy persistently */
	if (*ftsAction != FTS_READONLY && updated_bitmap)
	{
		elog(LOG, "calling probeUpdate with changes %p entries %d", changes, changeEntries);

		probeUpdateConfig(entryConn, changes, changeEntries, timestamp_str);
	}
	else
		elog(DEBUG2, "did not not update config (%s)", updated_bitmap ? "bitmap updated" : "bitmap not updated");

	ftsUnlock();

	if (changes != NULL)
	{
		closeSelfConn(entryConn);

		pfree(changes);
	}
	
	return updated_bitmap;
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

	entryBuffer = createPQExpBuffer();

	initPQExpBuffer(entryBuffer);

	appendPQExpBuffer(entryBuffer, "options='-c gp_session_role=UTILITY' ");

	if (entryDB->hostname == NULL)
		appendPQExpBufferStr(entryBuffer, "host='' ");
	else if (isdigit(entryDB->hostname[0]))
		appendPQExpBuffer(entryBuffer, "hostaddr=%s ", entryDB->hostname);
	else
		appendPQExpBuffer(entryBuffer, "host=%s ", entryDB->hostname);

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

		free(entryBuffer->data);
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

		free(entryBuffer->data);
		PQfinish(entryConn);
		entryConn = NULL;

		elog(FATAL, "FTS: error connecting to entry db, could not start transaction error: %s", error_message);
	}

	free(entryBuffer->data);

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

static void
addChangeHistory(PGconn *entryConn, struct segmentStatusChange *change, char *time_str)
{
	int count;
	char cmd[200];

	PGresult   *rs;

	bool	valid;
	bool	primary;

	Assert(change != NULL);
	Assert(time_str != NULL);

	valid   = (change->newStatus & FTS_STATUS_ALIVE) ? true : false;
	primary = (change->newStatus & FTS_STATUS_PRIMARY) ? true : false;

	count = snprintf(cmd, sizeof(cmd),
					 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: fault mode %s marking as valid=%c isprimary=%c persist=%c')",
					 time_str, change->dbid,
					 (*ftsAction != FTS_READONLY) ? "\"RW\"" : "\"RO\"",
					 valid ? 't' : 'f',
					 primary ? 't' : 'f',
					 change->persist ? 't' : 'f');

	if (count >= sizeof(cmd))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("FTS: could not invalidate invalid node in config: format command string failure dbid %d", change->dbid)));
	}

	rs = PQexec(entryConn, cmd);
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

/*
 * shared memory has been updated already; we'll pull the updated
 * status from the shared-memory copy.
 */
static void
probeUpdateConfig(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str)
{
	int i;

	int count;
	char cmd[200];

	PGresult   *rs=NULL;

	bool	valid;
	bool	primary;

	Assert(changes != NULL);
	Assert(changeCount > 0);

	/* finally, we're ready to actually get some stuff done. */
	do
	{
		for (i=0; i < changeCount; i++)
		{
			if (!changes[i].persist)
				continue;

			valid   = (changes[i].newStatus & FTS_STATUS_ALIVE) ? true : false;
			primary = (changes[i].newStatus & FTS_STATUS_PRIMARY) ? true : false;

			count = snprintf(cmd, sizeof(cmd),
							 "update gp_configuration set valid='%c', isprimary='%c' where dbid=CAST(%d AS SMALLINT)",
							 valid ? 't' : 'f',	 
							 primary ? 't' : 'f',	 
							 changes[i].dbid);

			if (count >= sizeof(cmd))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("FTS: could not invalidate invalid node in config: format command string failure dbid %d", changes[i].dbid)));
				break;
			}

			rs = PQexec(entryConn, cmd);
			if (PQresultStatus(rs) != PGRES_COMMAND_OK)
				break;
		}

		if (i < changeCount)
			break;

		/* No error, we're ready to go */

		PQclear(rs);
		return;
	} while (0);

	/* Something horrible happened while marking our database ? */
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

		PQclear(rs);
		PQfinish(entryConn);
		entryConn = NULL;

		elog(FATAL, "FTS: connection to entry-DB could not commit: %s", error_message);
	}
}
