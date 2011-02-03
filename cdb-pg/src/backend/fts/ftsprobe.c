/*-------------------------------------------------------------------------
 *
 * ftsprobe.c
 *	  Implementation of segment probing interface
 *
 * Copyright (c) 2006-2011, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <pthread.h>
#include <limits.h>

#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"
#include "cdb/cdbgang.h"		/* gp_pthread_create */
#include "postmaster/fts.h"

#include "executor/spi.h"
#include "postmaster/primary_mirror_mode.h"

#include "cdb/ml_ipc.h" /* gettime_elapsed_ms */

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

/*
 * CONSTANTS
 */

#define MAX_PROBE_RESPONSE_LEN	20


/*
 * ENUMS
 */

/* segment probing state */
enum probe_state_t
{
	CONNECTING = 0,
	CONNECTED,
	REQUEST_SENT,
	RESPONSE_READY
};


/*
 * STRUCTURES
 */

struct threadWorkerInfo
{
	uint8 *scan_status;
};


/*
 * STATIC VARIABLES
 */

/* mutex used for pthread synchronization in parallel probing */
static pthread_mutex_t worker_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

/* struct holding segment configuration */
static CdbComponentDatabases *cdb_component_dbs = NULL;

/* one byte of status for each segment */
static uint8 *scan_status;

/*
 * FUNCTION PROTOTYPES
 */

static void *probeSegmentFromThread(void *cdb_component_dbs);
static char probeSegment(CdbComponentDatabaseInfo *dbInfo);


/*
 * probe segments to check if they are alive and if any failure has occurred
 */
void
FtsProbeSegments(CdbComponentDatabases *dbs, uint8 *probeRes)
{
	int i;

	struct threadWorkerInfo worker_info;
	int pthread_err;
	int workers = gp_fts_probe_threadcount;
	pthread_t *threads = NULL;

	cdb_component_dbs = dbs;
	scan_status = probeRes;
	if (cdb_component_dbs == NULL || scan_status == NULL)
	{
		elog(ERROR, "FTS: segment configuration has not been loaded to shared memory");
	}

	/* reset probe results */
	memset(scan_status, 0, cdb_component_dbs->total_segment_dbs);

	/* figure out which segments to include in the scan. */
	for (i=0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		CdbComponentDatabaseInfo *segInfo = &cdb_component_dbs->segment_db_info[i];

		if (FtsIsSegmentAlive(segInfo))
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
	if (!FtsIsActive())
		return;

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
}


/*
 * This is called from several different threads: ONLY USE THREADSAFE FUNCTIONS INSIDE.
 */
static char
probeSegment(CdbComponentDatabaseInfo *dbInfo)
{
	bool	done=false;
	char	ret = PROBE_DEAD;
	enum	probe_state_t probe_state = CONNECTING;

	int		n, fd;
	struct sockaddr_in  saddr;

	GpMonotonicTime startTime;
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

	gp_init_monotonic_time(&startTime);
  	gp_get_monotonic_time(&startTime);

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

							write_log("FTS: failed to send probe request to segment (dbid %d content %d), errno %d",
									  dbInfo->dbid, dbInfo->segindex, errno);

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
					write_log("FTS: failed to receive data, segment (dbid %d content %d) closed connection unexpectedly",
							  dbInfo->dbid, dbInfo->segindex);

					ret = PROBE_ALIVE;
					done = true;
					break;
				}
				else if (n < 0)
				{
					if (errno == EAGAIN || errno == EINTR)
					{
						if (gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
							write_log("FTS: interrupted while receiving data from segment (dbid %d content %d), errno %d",
									  dbInfo->dbid, dbInfo->segindex, errno);

						/* need to wait on more data ? (essentially we're rewinding our state) */
						probe_state = REQUEST_SENT;
						continue;
					}

					write_log("FTS: failed to receive data from segment (dbid %d content %d), errno %d",
							  dbInfo->dbid, dbInfo->segindex, errno);

					ret = PROBE_ALIVE;
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

					/* check for inconsistent segment state */
					char probe_role = getRole(pm_mode);
					char probe_mode = getMode(d_state);
					if (probe_role != dbInfo->role || probe_mode != dbInfo->mode)
					{
						write_log("Segment with dbid %d content %d has not reached new state yet, expected state: ('%c','%c'), reported state: ('%c','%c')",
						          dbInfo->dbid, dbInfo->segindex, dbInfo->role, dbInfo->mode, probe_role, probe_mode);
					}

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

		elapsed_ms = gettime_elapsed_ms(&startTime);

		if (gp_log_fts >= GPVARS_VERBOSITY_DEBUG)
			write_log("Probe elapsed time: " UINT64_FORMAT " milliseconds", elapsed_ms);

		if (done)
		{
			break;
		}

		/* If connection takes more than the gp_fts_probe_timeout, we fail. */
		if (elapsed_ms > gp_fts_probe_timeout * 1000)
		{
			write_log("FTS: probe elapsed time: " UINT64_FORMAT " ms, exceeded timeout", elapsed_ms);

			ret = PROBE_DEAD;
			break;
		}
	}

	/* we're done, slam everything shut. */
	shutdown(fd, SHUT_RDWR);
	close(fd);

	return (ret);
}


static void *
probeSegmentFromThread(void *arg)
{
	struct threadWorkerInfo *worker_info;
	int i;

	worker_info = (struct threadWorkerInfo *) arg;

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

				mirror = FtsGetPeerSegment(primary->segindex, primary->dbid);

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
		if (!FtsIsActive())
		{
			break;
		}

		/* probe primary */
		probe_result_primary = probeSegment(primary);
		Assert(!PROBE_CHECK_FLAG(probe_result_primary, PROBE_SEGMENT));

		if ((probe_result_primary & PROBE_ALIVE) == 0 && gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
		{
			write_log("Primary segment %d segment-dbid %d (status 0x%x) was found dead",
			          primary->segindex, primary->dbid, probe_result_primary);
		}

		if (mirror != NULL)
		{
			/* assume mirror is alive */
			probe_result_mirror = PROBE_ALIVE;

			/* probe mirror only if primary is dead or has a crash fault */
			if (!PROBE_CHECK_FLAG(probe_result_primary, PROBE_ALIVE) ||
				PROBE_CHECK_FLAG(probe_result_primary, PROBE_FAULT_CRASH))
			{
				/* probe mirror */
				probe_result_mirror = probeSegment(mirror);
				Assert(!PROBE_CHECK_FLAG(probe_result_mirror, PROBE_SEGMENT));

				if ((probe_result_mirror & PROBE_ALIVE) == 0 && gp_log_fts >= GPVARS_VERBOSITY_VERBOSE)
				{
					write_log("Mirror segment %d segment-dbid %d (status 0x%x) was found dead",
							  mirror->segindex, mirror->dbid, probe_result_mirror);
				}
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


/* EOF */
