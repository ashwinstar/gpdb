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
 * FUNCTION PROTOTYPES
 */

#ifdef EXEC_BACKEND
static pid_t ftsprobe_forkexec(void);
#endif
NON_EXEC_STATIC void FtsProbeMain(int argc, char *argv[]);

static void FtsProbeLoop(void);

static void markSegmentInSyncInConfiguration(CdbComponentDatabaseInfo *dbInfo);
static char FtsTestOneSegment(CdbComponentDatabaseInfo *dbInfo);

static void retrieveUserAndDb(char **probeUser);
static bool probeSegments(CdbComponentDatabases *cdb_component_dbs);

static void *probeSegmentFromThread(void *arg);

extern bool FindMyDatabase(const char *name, Oid *db_id, Oid *db_tablespace);

static void fillTimestampStr(char *str, int maxLength );

static bool am_ftsprobe = false;

static volatile bool shutdown_requested = false;
static volatile bool rescan_requested = false;

static char *probeUser = NULL;
static char *probeDatabase = "postgres";

static char failover_strategy='n';

static CdbComponentDatabases *cdb_component_dbs = NULL;

static void readCdbComponentInfoAndUpdateStatus(void);
static bool probePublishUpdate(uint8 *scan_status);

struct segmentStatusChange {
	int	dbid;
	int16 segindex;
	uint8 oldStatus;
	uint8 newStatus;
};


typedef enum FilerepModeUpdateLoggingEnum
{
	FilerepModeUpdateLoggingEnum_MirrorToChangeTracking,
	FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking,
} FilerepModeUpdateLoggingEnum;

static PGconn *openSelfConn(CdbComponentDatabaseInfo *entryDB);
static void closeSelfConn(PGconn *conn);
static void addChangeHistory(PGconn *conn, struct segmentStatusChange *change, int changeCount, char *time_str);
static void probeUpdateConfig(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

static void getGpFailover_strategy(char *strategy);
static void SAN_Failover(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

static void Filerep_mode_update(int dbid, char *mode, char status, FilerepModeUpdateLoggingEnum logMsgToSend);
static void Filerep_Failover(struct segmentStatusChange *changes, int changeCount);

static void Filerep_preprocess_probe_results(uint8 *probe_results, CdbComponentDatabaseInfo *segInfo);

static void Null_Failover(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str);

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
	MemoryContext probeContext, oldContext;

	probeContext = AllocSetContextCreate(TopMemoryContext,
										 "FtsProbeMemCtxt",
										 ALLOCSET_DEFAULT_INITSIZE,	/* always have some memory */
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
	
	readCdbComponentInfoAndUpdateStatus();

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

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
		updated_bitmap = probeSegments(cdb_component_dbs);

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
			struct timeval tv;

			tv.tv_usec = 0;
			tv.tv_sec = gp_fts_probe_interval;

			select(0, NULL, NULL, NULL, &tv); /* don't care about return value. */
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

#define PROBE_SEGMENT	(0x80)
#define PROBE_PENDING	(0x40)
#define PROBE_DONE		(0x20)

#define PROBE_ALIVE		(0x01)
#define PROBE_FAULT		(0x02)
#define PROBE_TO_IN_SYNC (0x04)

enum probe_state_t { CONNECTING, CONNECTED, REQUEST_SENT, RESPONSE_READY };

/*
 * This is called from several different threads: ONLY USE THREADSAFE FUNCTIONS INSIDE.
 */
static char
FtsTestOneSegment(CdbComponentDatabaseInfo *dbInfo)
{
	bool	done=false;
	char	ret = 0;
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
		return (PROBE_DONE | PROBE_ALIVE);
	}

	if (!pg_set_noblock(fd))
	{
		/* something happened ? */
		write_log("Couldn't mark socket non-blocking.");
		close(fd);
		return (PROBE_DONE | PROBE_ALIVE);
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
		return (PROBE_DONE | PROBE_ALIVE); /* alive */
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

		return PROBE_DONE; /* NOT alive */
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
							ret = 0;
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
					ret = 0;
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
					ret = 0;
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
					if (offset < 16)
					{
						/* need to wait on more data ? (essentially we're rewinding our state) */
						probe_state = REQUEST_SENT;
						continue;
					}

					memcpy(&msg_int, buf, 4);
					if (ntohl(msg_int) != 20)
					{
						write_log("Probe protocol violation got length %d", ntohl(msg_int));
						ret = 0;
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
						write_log("Probe result %s %s %s %s", 
								  getMirrorModeLabel(pm_mode),
								  getSegmentStateLabel(s_state), 
								  getDataStateLabel(d_state),
								  getFaultTypeLabel(f_type));

					ret = PROBE_ALIVE;

					if (s_state == SegmentStateFault)
					{						
						write_log("Segment with dbid %d content %d reported a fault %s to the prober", 
								  dbInfo->dbid, 
								  dbInfo->segindex,
								  getFaultTypeLabel(f_type));

						/* This means that the segment needs to know something from the prober. */
						ret |= PROBE_FAULT;
					}
					
					if (d_state == DataStateInSync && dbInfo->mode == 'r')
					{
						ret |= PROBE_TO_IN_SYNC;
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
				ret = 0;
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
			ret = 0;
			break;
		}
	}

	/* we're done, slam everything shut. */
	shutdown(fd, SHUT_RDWR);
	close(fd);

	return (PROBE_DONE | ret);
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

pthread_t *threads=NULL;
static pthread_mutex_t	worker_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

struct threadWorkerInfo {
	bool		foundFault;
	bool		probeRestart;
	uint8		*scan_status;
	CdbComponentDatabases *dbs;
};

/*
 * one byte of status for each segment, dbids are int2, so we can represent a max
 * of 32k
 */
static uint8 scan_status[32768];

static bool
probeSegments(CdbComponentDatabases *cdb_component_dbs)
{
	int i;
	bool updated_bitmap = false;
	bool probeRestart = false;

	if (cdb_component_dbs == NULL)
		return false;

probe_restart:
	memset(scan_status, 0, sizeof(scan_status));

	/* single threaded */
	if (gp_fts_probe_threadcount == 1)
	{
		for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

			if (shutdown_requested)
				break;

			/* if this is worth scanning, then scan it. */
			if (segmentToProbe(segInfo, failover_strategy))
			{
				scan_status[segInfo->dbid] = FtsTestOneSegment(segInfo);

				if ((scan_status[segInfo->dbid] & PROBE_ALIVE) == 0)
				{
					if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
						elog(LOG, "Found a fault with a segment %d segment-dbid %d", segInfo->segindex, segInfo->dbid);
				}
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

			if (segmentToProbe(segInfo, failover_strategy))
			{
				scan_status[segInfo->dbid] |= PROBE_SEGMENT;
			}
			else
			{
				scan_status[segInfo->dbid] |= (PROBE_DONE | PROBE_ALIVE);
			}
		}

		worker_info.foundFault = false;
		worker_info.probeRestart = probeRestart;
		worker_info.scan_status = scan_status;
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

		if (worker_info.foundFault && !worker_info.probeRestart)
		{
			probeRestart = true;
			goto probe_restart;
		}
	}

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
	CdbComponentDatabaseInfo *segInfo;
	struct threadWorkerInfo *worker_info;
	int i;
	char probe_result;

	worker_info = (struct threadWorkerInfo *)arg;

	i = 0;
	
	for (;;)
	{
		segInfo = NULL;

		/*
		 * Get a work item: find first un-tested segment, mark it
		 * "tested" and unlock.
		 */
		pthread_mutex_lock(&worker_thread_mutex);
		for (; i < worker_info->dbs->total_segment_dbs; i++)
		{
			segInfo = &worker_info->dbs->segment_db_info[i];

			if ((worker_info->scan_status[segInfo->dbid] & PROBE_SEGMENT) == PROBE_SEGMENT)
			{
				worker_info->scan_status[segInfo->dbid] = PROBE_PENDING;
				break;
			}
			if (worker_info->foundFault && !worker_info->probeRestart)
			{
				/* we need to stop probing, and prepare for our rescan. */
				i = worker_info->dbs->total_segment_dbs;
				break;
			}
		}
		pthread_mutex_unlock(&worker_thread_mutex);

		/* no work left to do */
		if (i == worker_info->dbs->total_segment_dbs)
			break;

		/* no segInfo ? */
		if (segInfo == NULL)
			break;

		/* shutdown request ? bail-out. */
		if (shutdown_requested)
			break;

		/* Got a segment to test, test it. */

		probe_result = FtsTestOneSegment(segInfo);
		if ((probe_result & PROBE_ALIVE) == 0)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				write_log("Found a fault with a segment %d segment-dbid %d (status 0x%x)", segInfo->segindex, segInfo->dbid, probe_result);
		}
		pthread_mutex_lock(&worker_thread_mutex);
		if ((probe_result & PROBE_FAULT) != 0)
		{
			worker_info->foundFault = true;
		}
		worker_info->scan_status[segInfo->dbid] = probe_result;
		pthread_mutex_unlock(&worker_thread_mutex);
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

static void
fillTimestampStr(char *str, int maxLength )
{
	pg_time_t		curtime;
	struct pg_tm	*time_tm;
	struct timeval	tv;

	gettimeofday(&tv, NULL);
	curtime = tv.tv_sec;
	time_tm = pg_gmtime(&curtime);
	pg_strftime(str, maxLength, "%+", time_tm); /* "%+" is the format used by the Unix date command */
}

/*
 * Build a set of changes, based on our current state, and the probe results.
 */
static bool
probePublishUpdate(uint8 *probe_results)
{
	bool updated_bitmap=false;
	int i;

	struct segmentStatusChange *changes=NULL;
	int changeEntries=0;

	PGconn *entryConn = NULL;

	CdbComponentDatabaseInfo *primary;
	CdbComponentDatabaseInfo *mirror;

	char timestamp_str[100];

	if (failover_strategy == 'f')
	{
		/* if in filerep mode, convert FAULT messages (a failure in MIRRORING) to changes in ALIVE status */
		/* this preprocessing may also mark segment pairs in-sync in configuration */
		for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
		{
			CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

			/*
			 * Handle Filerep state changes (states other than "alive") --
			 * this has some side-effects on the probe-results.
			 *
			 * For instance:
			 * Handle PROBE_FAULT here. if we have a segment alive,
			 * but it is reporting a fault, then we need to update
			 * some state.
			 *
			 * "fault" is a Filerep concept.
			 *
			 * See FtsTestOneSegment(), PROBE_FAULT implies PROBE_ALIVE.
			 */
			Filerep_preprocess_probe_results(probe_results, segInfo);
		}
	}

	/* collect status changes */
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "probePublishUpdate: dbid %d", segInfo->dbid);

		/* We're an active segment ? */
		if (segmentToProbe(segInfo, failover_strategy))
		{
			bool updateMirror = false;
			int primaryChange;
			int mirrorChange;
			uint8 segStatus;

			/* We're no longer responding to probes ? */
			if ((probe_results[segInfo->dbid] & PROBE_ALIVE) == 0)
			{
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
					changes[primaryChange].segindex = segInfo->segindex;
					changes[primaryChange].oldStatus = segStatus;
					changes[primaryChange].newStatus = segStatus & ~FTS_STATUS_ALIVE;

					/*
					 * If we have a mirror (which is alive), we need
					 * to update it (even if it was already primary!)
					 *
					 * Things are slightly complicated by the differences between
					 * Filerep and SAN-failover strategies.
					 */
					if (mirror != NULL)
					{
						if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
							elog(LOG, "probePublishUpdate: dbid %d has mirror %d", segInfo->dbid, mirror->dbid);

						if (FTS_STATUS_ISALIVE(mirror->dbid, ftsProbeInfo->fts_status))
						{
							if (failover_strategy == 'f')
							{
								/* If the mirror is not currently synchronized, we can't use it */
								if (FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status))
								{
									if ((probe_results[mirror->dbid] & PROBE_ALIVE) == PROBE_ALIVE)
									{
										updateMirror = true;
									}
								}
								else
								{
									/* If the mirror was being synchronized, we need to mark it down. */
									updateMirror = true;
								}
							}
							else if (failover_strategy == 's')
							{
								updateMirror = true;
							}
						}
					}

					if (updateMirror)
					{
						/* update the mirror too */
						mirrorChange = changeEntries;
						changes = (struct segmentStatusChange *)
							repalloc(changes, (changeEntries + 1)*sizeof(struct segmentStatusChange));
						changeEntries++;

						segStatus = ftsProbeInfo->fts_status[mirror->dbid];
						changes[mirrorChange].dbid = mirror->dbid;
						changes[mirrorChange].segindex = mirror->segindex;
						changes[mirrorChange].oldStatus = segStatus;

						if (failover_strategy == 's' ||
							FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status))
						{
							changes[mirrorChange].newStatus = segStatus | FTS_STATUS_PRIMARY;

							/* update primary */
							/*
							 * MPP-8764/MPP-8765: We only clear the Primary-status if we successfully found an
							 * alive mirror. This means that the way we detect a second failure is that we have
							 * a "newStatus" which is an *invalid primary* (see addChangeHistory() and
							 * probeUpdateConfig()).
							 */
							/* 'p'/'u'/'s' */
							changes[primaryChange].newStatus &= ~FTS_STATUS_PRIMARY;
						}
						else if (FTS_STATUS_ISPRIMARY(mirror->dbid, ftsProbeInfo->fts_status))
						{
							/*
							 *'p'/'u'/'r': We have valid contents and
							 * were already the primary! No status
							 * change required.
							 */
							changes[mirrorChange].newStatus = segStatus;
						}
						else
						{
							/* we can't take over, we aren't up-to-date */
							changes[mirrorChange].newStatus = segStatus & (~FTS_STATUS_ALIVE);
						}
					}

					updated_bitmap = true;
				}
				/* was already marked down, no change. */
			}
		}
	}

	/* if we've gotten a pause request, we ignore our probe results. */
	if (ftsProbeInfo->fts_pauseProbes)
	{
		if (changes != NULL)
		{
			pfree(changes);
		}

		return false;
	}

	/* We're going to use these timestamps below. */
	fillTimestampStr(timestamp_str, sizeof(timestamp_str));

	if (changes != NULL)
	{
		CdbComponentDatabaseInfo *entryDB = &cdb_component_dbs->entry_db_info[0];

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			elog(LOG, "probePublishUpdate: Changes is not null");

		if (entryDB->dbid != GpIdentity.dbid)
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
				elog(LOG, "ftsProbe: advancing to second entryDB.");
			entryDB = entryDB + 1;
		}

		entryConn = openSelfConn(entryDB);
		Assert(entryConn != NULL);

		/* Now we're going to apply the changelist we just generated. */
		ftsLock();

		/* if we've gotten a pause request, we ignore our probe results. */
		if (ftsProbeInfo->fts_pauseProbes)
		{
			closeSelfConn(entryConn);
			pfree(changes);
			ftsUnlock();

			return false;
		}
		
		if (failover_strategy == 'n')
		{
			/* no fault strategy ? */
			Null_Failover(entryConn, changes, changeEntries, timestamp_str);

			/* we don't record history, or update configs */
			closeSelfConn(entryConn);
		}
		else
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				elog(LOG, "probePublishUpdate: updated config, now modifying segments");

			if (failover_strategy == 'f')
			{
				addChangeHistory(entryConn, changes, changeEntries, timestamp_str);
				probeUpdateConfig(entryConn, changes, changeEntries, timestamp_str);

				/* commit our config/history changes */
				closeSelfConn(entryConn);

				/* FILEREP response */
				Filerep_Failover(changes, changeEntries);
			}
			else if (failover_strategy == 's')
			{
				/* SAN response */
				SAN_Failover(entryConn, changes, changeEntries, timestamp_str);

				addChangeHistory(entryConn, changes, changeEntries, timestamp_str);
				probeUpdateConfig(entryConn, changes, changeEntries, timestamp_str);

				/* commit our config/history changes */
				closeSelfConn(entryConn);
			}

			if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				elog(LOG, "probePublishUpdate: finished segment modifications");
		}

		ftsUnlock();

		pfree(changes);
	}

	if (updated_bitmap)
	{
		if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
			elog(LOG, "probePublishUpdate: found changes.");
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
	if (PQExpBufferBroken(entryBuffer))
	{
		destroyPQExpBuffer(entryBuffer);
		elog(FATAL, "FTSPROBE: failed to create self-connection buffer.");
		/* never reached */
		return NULL;
	}

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

static void
addChangeHistory(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str)
{
	int i;
	int count;
	char cmd[200];

	PGresult   *rs;

	bool	valid;
	bool	primary;

	Assert(changes != NULL);
	Assert(time_str != NULL);

	for (i=0; i < changeCount; i++)
	{
		struct segmentStatusChange *change = &changes[i];

		valid   = (change->newStatus & FTS_STATUS_ALIVE) ? true : false;
		primary = (change->newStatus & FTS_STATUS_PRIMARY) ? true : false;

		/* see probeUpdateConfig() we're trying to describe the changes that it is going to make in human-readable terms */
		if (failover_strategy == 'f')
		{
			if (!valid && primary)
			{
				if ((changes[i].oldStatus & FTS_STATUS_CHANGELOGGING) != 0)
					continue; /* this was already marked */

				count = snprintf(cmd, sizeof(cmd),
								 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: content %d double failure, last known primary marked for changetracking')",
								 time_str, change->dbid, change->segindex);
			}
			else
			{
				count = snprintf(cmd, sizeof(cmd),
								 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: content %d fault marking status %s%s role %c')",
								 time_str, change->dbid, change->segindex,
								 valid ? "UP" : "DOWN",
								 (valid && primary) ? " mode: changetracking" : "",
								 primary ? 'p' : 'm');
			}
		}
		else /* SAN */
		{
			/*
			 * MPP-8764/MPP-8765: if we wound up with an invalid primary,
			 * we are "down" but we aren't going to record it.
			 */
			if (!valid && primary)
				continue;

			count = snprintf(cmd, sizeof(cmd),
							 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: content %d fault marking as %s role %c')",
							 time_str, change->dbid, change->segindex,
							 valid ? "UP" : "DOWN",
							 primary ? 'p' : 'm');
		}

		if (count >= sizeof(cmd))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("FTS: could not insert into configuration history: format command string failure dbid %d", change->dbid)));
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
			valid   = (changes[i].newStatus & FTS_STATUS_ALIVE) ? true : false;
			primary = (changes[i].newStatus & FTS_STATUS_PRIMARY) ? true : false;

			if (failover_strategy == 'f')
			{
				/*
				 * MPP-8764/MPP-8765: if we wound up with an invalid primary,
				 * we are "down" but we aren't going to record it.
				 * 
				 * See the changelist generation logic in probePublishUpdate() for
				 * details.
				 *
				 * This means that we also had an out-of-date or dead
				 * mirror, so someday if we recover, we want to log
				 * changes.
				 */
				if (!valid && primary)
				{
					if ((changes[i].oldStatus & FTS_STATUS_CHANGELOGGING) != 0)
						continue; /* already marked */

					count = snprintf(cmd, sizeof(cmd),
									 "update %s set mode = 'c' where dbid=CAST(%d AS SMALLINT)",
									 GpSegmentConfigRelationName,
									 changes[i].dbid);

				}
				else
				{
					count = snprintf(cmd, sizeof(cmd),
									 "update %s set role='%c', status='%c'%s where dbid=CAST(%d AS SMALLINT)",
									 GpSegmentConfigRelationName,
									 primary ? 'p' : 'm',
									 valid ? 'u' : 'd',
									 (valid && primary) ? ", mode = 'c'" : "",
									 changes[i].dbid);
				}
			}
			else
			{
				/*
				 * MPP-8764/MPP-8765: if we wound up with an invalid primary,
				 * we are "down" but we aren't going to record it.
				 * 
				 * See the changelist generation logic in probePublishUpdate() for
				 * details.
				 *
				 * For SAN we're done on second failure.
				 */
				if (!valid && primary)
					continue;

				count = snprintf(cmd, sizeof(cmd),
								 "update %s set role='%c', status='%c' where dbid=CAST(%d AS SMALLINT)",
								 GpSegmentConfigRelationName,
								 primary ? 'p' : 'm',
								 valid ? 'u' : 'd',
								 changes[i].dbid);
			}

			if (count >= sizeof(cmd))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("FTS: could not modify config: format command string failure dbid %d", changes[i].dbid)));
				break;
			}

			ftsProbeInfo->fts_status[changes[i].dbid] = changes[i].newStatus;

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
dumpChanges(struct segmentStatusChange *changes, int changeCount)
{
	int i;

	elog(LOG, "Dump our configuration change list, %d changes", changeCount);

	for (i=0; i < changeCount; i++)
	{
		bool new_alive, old_alive;
		bool new_pri, old_pri;

		new_alive = (changes[i].newStatus & FTS_STATUS_ALIVE ? true : false);
		old_alive = (changes[i].oldStatus & FTS_STATUS_ALIVE ? true : false);

		new_pri = (changes[i].newStatus & FTS_STATUS_PRIMARY ? true : false);
		old_pri = (changes[i].oldStatus & FTS_STATUS_PRIMARY ? true : false);

		elog(LOG, "   change %d of %d dbid %d content %d alive %d/%d primary %d/%d",
			 i+1, changeCount, changes[i].dbid, changes[i].segindex, new_alive, old_alive, new_pri, old_pri);
	}
}

static void
Null_Failover(PGconn *entryConn, struct segmentStatusChange *changes, int changeCount, char *time_str)
{
	if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		dumpChanges(changes, changeCount);
}

/*
 * Factor this out of probePublishUpdate(), we're pre-processing the
 * probe results to take into account some special state-changes that
 * Filerep uses: when the segments have completed re-sync, or when
 * they report an explicit fault.
 *
 * NOTE: we always call this twice for each mirror-pair, one for the
 * primary and one for the mirror, but we take their respective roles
 * into account (see below) otherwise it is impossible to reason about
 * the state changes.
 */
static void
Filerep_preprocess_probe_results(uint8 *probe_results, CdbComponentDatabaseInfo *segInfo)
{
#define PROBE_IS_DEAD(dbInfo) (((probe_results[(dbInfo)->dbid]) & PROBE_ALIVE) == 0)
#define PROBE_IS_FAULT(dbInfo) (((probe_results[(dbInfo)->dbid]) & PROBE_FAULT) == PROBE_FAULT)
#define PROBE_IS_TO_IN_SYNC(dbInfo) (((probe_results[(dbInfo)->dbid]) & PROBE_TO_IN_SYNC) == PROBE_TO_IN_SYNC)

	CdbComponentDatabaseInfo *primary = NULL, *mirror = NULL;

	if (SEGMENT_IS_ACTIVE_PRIMARY(segInfo))
	{
		primary = segInfo;
		mirror = getMirrorEntryByContent(cdb_component_dbs, primary->segindex, primary->dbid);
		Assert(mirror != NULL); /* mirrors should always be there in filerep mode */
	}
	else
	{
		/*
		 * all segment pairs will be preprocessed before we use any
		 * results, so it's okay to just preprocess when the primary
		 * is the segInfo.  Early returning here doesn't help
		 * correctness, but avoids bad log messages because the second
		 * trip through this function for a pair would print different
		 * output than the first
		 */
		return;
	}

	/*
	 * We've completed resync ? NOTE: we've only completed resync if *both* report completion.
	 */
	if (PROBE_IS_TO_IN_SYNC(primary) &&
	    PROBE_IS_TO_IN_SYNC(mirror))
	{
		/*
		 * if primary and mirror are both in-sync then update
		 * configuration: because will not necessarily reach in-sync
		 * internally at the same point, we make sure both are in-sync
		 * before updating configuraiton
		 */
		markSegmentInSyncInConfiguration(primary);
		probe_results[primary->dbid] &= ~PROBE_TO_IN_SYNC;

		markSegmentInSyncInConfiguration(mirror);
		probe_results[mirror->dbid] &= ~PROBE_TO_IN_SYNC;
	}

	/*
	 * There are a few possibilities here:
	 *    0) primary in fault, mirror already marked down, primary
	 * expected to be in changetracking -- re-issue changetracking transition.
	 *    1) primary in fault, mirror OK:
	 *             mirror is promoted.
	 *    2) primary in fault, mirror dead
	 *    3) primary in fault, mirror in fault
	 *             primary-changelogging, mirror considered dead.
	 *    4) primary in fault, primary and mirror busy resynching.
	 *             primary-changelogging, mirror considered dead.
	 *    5) primary OK, mirror in fault.
	 *             primary-changelogging, mirror considered dead.
	 *    6) primary dead, mirror dead.
	 *             do nothing more
	 *    7) primary dead, mirror alive (can be in fault) and in-sync
	 *             mirror-changelogging, primary dead
	 *    8) primary dead, mirror alive (can be in fault) but out-of-sync
	 *             consider all dead
	 *
	 * We're going to depend on the code further down to make the actual changes,
	 * here we just mark which segments to consider "down."
	 */
	if (PROBE_IS_FAULT(primary))
	{
		/* PRIMARY FAULT -- case 0 */
		if (segInfo->mode == 'c')
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported fault. Already in changetracking. Retrying transition.",
				 primary->dbid, probe_results[primary->dbid]);

			Filerep_mode_update(primary->dbid, "primary", 'c', FilerepModeUpdateLoggingEnum_PrimaryToChangeTracking);
		}

		/* PRIMARY FAULT -- cases 1-4 */
		if (PROBE_IS_FAULT(mirror))
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported fault, mirror %d/0x%x considered dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);

			/* consider mirror dead (3) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}
		else if (PROBE_IS_DEAD(mirror))
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported fault, mirror %d/0x%x is dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);

			/* consider mirror dead (2) */
		}
		else if (FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status)) /* mirror up, check in-sync */
		{
			/* The mirror was intact ? */
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported fault, mirror %d/0x%x looks good.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);

			/* mirror is OK (1) */
			probe_results[primary->dbid] &= ~PROBE_ALIVE;
		}
		else
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported fault, mirror %d/0x%x was resyncing, now considered dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
			/*
			 * mirror must have been re-syncing,
			 * so the primary is just reporting a
			 * problem from the mirror.
			 */
			/* consider mirror dead (3)(4) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}
	}
	else if (PROBE_IS_DEAD(primary))
	{
		/* PRIMARY DEAD -- cases 6-8 */
		if (PROBE_IS_DEAD(mirror))
		{
			/* mirror reported dead (6) */
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported dead, mirror %d/0x%x reported dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
		}
		else if (FTS_STATUS_IS_SYNCED(mirror->dbid, ftsProbeInfo->fts_status)) /* mirror not dead, check in-sync */
		{
			/* The mirror was intact and synchronized -- consider it alive (7) */

			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported dead, mirror %d/0x%x considered alive.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
		}
		else
		{
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported dead, mirror %d/0x%x was resyncing, now considered dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);

			/* mirror was alive but not in-sync, must consider it dead (8) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}
	}
	else
	{
 		/* Primary is OK. What about the mirror ? (case 5) */
		if (PROBE_IS_FAULT(mirror))
		{
			/* case 5: mirror should be considered dead. */
			elog(LOG, "probePublishUpdate: dbid %d/0x%x reported OK mirror, %d/0x%x considered dead.",
				 primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
			/* consider mirror dead (5) */
			probe_results[mirror->dbid] &= ~PROBE_ALIVE;
		}
		else if (PROBE_IS_DEAD(mirror))
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
				elog(LOG, "probePublishUpdate: dbid %d/0x%x reported OK, mirror %d/0x%x already dead.",
				 	primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
		}
		else
		{
			if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
				elog(LOG, "probePublishUpdate: dbid %d/0x%x reported OK, mirror %d/0x%x alive.",
				 	primary->dbid, probe_results[primary->dbid], mirror->dbid, probe_results[mirror->dbid]);
		}
	}

	/* clear PROBE_FAULT because it's not needed after this processing */
	probe_results[primary->dbid] &= ~PROBE_FAULT;
	probe_results[mirror->dbid] &= ~PROBE_FAULT;
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

			sprintf(cmd, "gp_primarymirror -h %s -p %d -i %s", seg_addr, seg_pm_port, tmp_filename);

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
 * Convenience macro for running some SQL on a previously established self-connection.
 */
#define executeEntrySQL(entryConn, query)								\
	{																	\
		PGresult   *rs=NULL;											\
																		\
		rs = PQexec(entryConn, query);									\
		if (PQresultStatus(rs) != PGRES_COMMAND_OK)						\
		{																\
			char	*error_message = strdup(PQerrorMessage(entryConn));	\
																		\
			if (!error_message)											\
				ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),			\
								errmsg("FTS: out of memory")));			\
																		\
			PQclear(rs);												\
			PQfinish(entryConn);										\
			entryConn = NULL;											\
																		\
			elog(FATAL, "FTS: connection to entry-DB could not commit: %s", error_message);	\
		}																\
	}

/**
 * Marks the given db as in-sync in the segment configuration.
 */
static void
markSegmentInSyncInConfiguration(CdbComponentDatabaseInfo *dbInfo)
{
	char cmd[2000];
	char timestamp_str[100];

	fillTimestampStr(timestamp_str, sizeof(timestamp_str));

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
		bool	isPrimary;

		/* update in-core copy */
		segStatus = ftsProbeInfo->fts_status[dbInfo->dbid];
		segStatus |= FTS_STATUS_SYNCHRONIZED;
		ftsProbeInfo->fts_status[dbInfo->dbid] = segStatus;

		/* update gp_segment_configuration to insync */
		snprintf(cmd, sizeof(cmd),
				 "update %s set mode='s' where dbid=CAST(%d AS SMALLINT)",
				 GpSegmentConfigRelationName,
				 dbInfo->dbid);
		executeEntrySQL(entryConn, cmd);

		/* update configuration history */
		snprintf(cmd, sizeof(cmd),
				 "insert into gp_configuration_history values ('%s', %d, 'FTSPROBE: changed segment to insync from resync.')",
				 timestamp_str, dbInfo->dbid);
		executeEntrySQL(entryConn, cmd);

		isPrimary = segStatus & FTS_STATUS_PRIMARY;
		if (!isPrimary)
		{
			ereport(LOG,
					(errmsg("Resynchronization of mirror segment on %s:%d has completed.",
							dbInfo->address, dbInfo->port ),
					 errSendAlert(true)));
		}
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
