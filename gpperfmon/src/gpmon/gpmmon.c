#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <signal.h>
#include "apr_atomic.h"
#include "apr_env.h"
#include "apr_time.h"
#include "apr_hash.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_pools.h"
#include "apr_tables.h"
#include "apr_thread_proc.h"
#include "apr_thread_mutex.h"
#include "apr_md5.h"
#include "gpmonlib.h"
#include "gpmon/gpmon.h"
#include "gpmon_agg.h"
#include "gpmondb.h"
#include "apr_getopt.h"
#include <event.h>
#include "libpq-fe.h"
#include <time.h>


#define SET_MAXFD(fd)  if (fd < opt.max_fd) ; else opt.max_fd = fd

/* gpmmon options */
struct
{
	int argc;
	const char* const *argv;
	const char* pname;
	char* gpdb_port;
	char* conf_file;
	char* log_dir;
	apr_uint64_t max_log_size;
	/* TODO: add log file rotation values */
	int max_fd; /* this is the max fd value we ever seen */
	int v;
	int q;
	int m;
	int d;
	int harvest_interval;
	apr_uint64_t tail_buffer_max;
} opt = { 0 };

/* gpsmon host */
typedef struct host_t host_t;
struct host_t
{
	apr_thread_mutex_t *mutex;
	int sock; /* socket connected to gpsmon on this host */
	int eflag; /* flag: socket has error */
	struct event* event; /* points to _event if set */
	struct event _event; /* static */
	char* hostname; /* static */
	char* realhostname; /* static */
	char* ipstr; /* static */
	char* data_dir;
};

/* gpmmon host */
static struct
{
	int port; /* port of gpsmon */
	apr_pool_t* pool; /* pool */
	char* gphome; /* GPHOME env variable */
	apr_int64_t signature; /* a large random number */

	host_t* hosttab; /* multi-home filtered machines */
	int hosttabsz; /* # machines */

	char* master_data_directory;
	apr_thread_mutex_t *agg_mutex; /* lock when accessing the agg data */
	agg_t* agg;

	apr_thread_mutex_t *tailfile_mutex; /* lock when accessing the physical tail files */

	int exit; /* TRUE if we need to exit */
	int reload;
	apr_uint64_t tail_buffer_bytes;
	apr_uint64_t _tail_buffer_bytes;
} ax = { 0 };

void interuptable_sleep(unsigned int seconds);


/* Default option values */
int verbose = 0; /* == opt.v */
int very_verbose = 0; /* == opt.V */
int quantum = 15; /* == opt.q */
int min_query_time = 60; /* == opt.m */
int min_detailed_query_time = 60; /* == opt.d */


/* thread handles */
static apr_thread_t* conm_th = NULL;
static apr_thread_t* event_th = NULL;
static apr_thread_t* harvest_th = NULL;

/* signal masks */
sigset_t unblocksig;
sigset_t blocksig;


/* Function defs */
static int read_conf_file(char *conffile);
static void gethostlist(host_t** tab, int* tabsz);
static void getconfig(void);
static apr_status_t sendpkt(int sock, const gpmon_packet_t* pkt);
static apr_status_t recvpkt(int sock, gpmon_packet_t* pkt);


/** Gets quantum */
int gpmmon_quantum(void)
{
	return opt.q;
}

/* prints usage and exit */
static void usage(const char* msg)
{
	fprintf(stderr, "\nusage: %s -D <configuration file> [options]\n\n",
			opt.pname);
	fprintf(stderr, "options:\n");
	fprintf(stderr, "\t-?\t\t: print this help screen\n");
	fprintf(stderr, "\t-V\t\t: print version information\n\n");
	if (msg)
		fprintf(stderr, "%s\n\n", msg);

	exit(msg ? 1 : 0);
}

void incremement_tail_bytes(apr_uint64_t bytes)
{
	ax.tail_buffer_bytes += bytes;
}

/* Cleanup function called on exit. */
static void cleanup()
{
	/* wait for threads, close ports, apr cleanup, etc. */
	apr_status_t tstatus;
	int i;

	ax.exit = 1;

	if (event_th)
		apr_thread_join(&tstatus, event_th);
	if (conm_th)
		apr_thread_join(&tstatus, conm_th);
	if (harvest_th)
		apr_thread_join(&tstatus, harvest_th);

	for (i = 0; i < ax.hosttabsz; i++)
	{
		host_t* h = &ax.hosttab[i];
		if (h)
		{
			apr_thread_mutex_lock(h->mutex);
			if (h->sock)
				close(h->sock);
			h->sock = 0;
			apr_thread_mutex_unlock(h->mutex);
			h = NULL;
		}
	}

	if (ax.pool)
		apr_pool_destroy(ax.pool);
}


/**
 * Signal handlers
 */
static void SIGHUP_handler(int sig)
{
	/* Flag to reload configuration values from conf file */
	ax.reload = 1;
}

static void SIGTERM_handler(int sig)
{
	ax.exit = 1;
}

static void SIGQUIT_handler(int sig)
{
	/* Quick exit here */
	sigprocmask(SIG_SETMASK, &blocksig, NULL);
	exit(0);
}

static void SIGUSR2_handler(int sig)
{
	ax.exit = 1;
}


/** ------------------------------------------------------------
 After we sent a 'D'ump command, gpsmon will send us packets thru
 the TCP connection. This function gets called whenever a packet
 arrives.
 */
static void recv_from_gx(SOCKET sock, short event, void* arg)
{
	host_t* h = arg;
	int e;
	gpmon_packet_t pktbuf;
	gpmon_packet_t* pkt = 0;
	TR2(("recv_from_gx sock %d host %s (%s:%d)\n", sock, h->hostname, h->ipstr, ax.port));

	if (!(event & EV_READ))
		return;

	apr_thread_mutex_lock(h->mutex);

	if (h->event)
	{
		if (!h->eflag)
		{
			char* p = (char*) &pktbuf;
			char* q = p + sizeof(pktbuf);
			while (p < q)
			{
				int n = recv(sock, p, q - p, 0);
				if (n == -1)
				{
					switch (errno)
					{
					case EINTR:
					case EAGAIN:
						continue;
					}
					gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno),
							"cannot get packet from host %s (%s:%d)",
							h->hostname, h->ipstr, ax.port);
					break;
				}
				p += n;
				if (p == (char*) &pktbuf)
				{
					TR1(("at %s: connection dropped by host %s (%s:%d) [set eflag]\n", FLINE, h->hostname, h->ipstr, ax.port));
					h->eflag = 1;
					break;
				}
			}
		}
		if (h->eflag)
		{
			event_del(h->event);
			h->event = 0;
		}
		else
		{
			pkt = &pktbuf;
			TR2(("received packet %d from %s (%s:%d)\n", pkt->pkttype, h->hostname, h->ipstr, ax.port));
		}
	}
	apr_thread_mutex_unlock(h->mutex);
	if (pkt)
	{
		apr_thread_mutex_lock(ax.agg_mutex);
		e = agg_put(ax.agg, pkt);
		apr_thread_mutex_unlock(ax.agg_mutex);
		if (e)
		{
        	interuptable_sleep(30); // sleep to prevent loop of forking process and failing
			gpmon_fatalx(FLINE, e, "agg_put failed");
		}
	}
}

/** ------------------------------------------------------------
 This is where we setup events for TCP connections to gpsmon and
 service those TCP connections, i.e. receiving qexec/qlog packets
 following a dump command. We send the dump command in gpmmon_main().

 Event thread:
 Forever:
 serve events for 2 sec
 check if we need to set up new event

 On ready:
 if error:
 close(event->ev_fd);
 if readok:
 read a packet
 */
static void* event_main(apr_thread_t* thread_, void* arg_)
{
	struct timeval tv;
	host_t* tab = ax.hosttab;
	const int tabsz = ax.hosttabsz;

	if (!event_init())
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_init failed");
	}
	while (!ax.exit)
	{
		int i;
		int count_valid = 0;

		/* setup new events */
		TR2(("event_main: scan hosttab\n"));

		for (i = 0; i < tabsz; i++)
		{
			host_t* h = &tab[i];
			SOCKET close_sock = 0;
			apr_thread_mutex_lock(h->mutex);
			if (h->sock)
			{
				if (h->eflag)
				{
					if (h->event)
					{
						event_del(h->event);
						h->event = 0;
					}
					close_sock = h->sock;
					h->sock = 0;
				}
				else
				{
					count_valid++;
					if (!h->event)
					{
						/* set up the event */
						h->event = &h->_event;
						event_set(h->event, h->sock, EV_READ | EV_PERSIST,
								recv_from_gx, h);
						if (event_add(h->event, 0))
						{
        					interuptable_sleep(30); // sleep to prevent loop of forking process and failing
							gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_add failed");
						}
					}
				}
			}
			apr_thread_mutex_unlock(h->mutex);
			if (close_sock)
			{
				TR2(("closing socket %d\n", close_sock));
				closesocket(close_sock);
			}
		}

		if (count_valid == 0)
		{
			TR2(("no valid connection, sleep 1\n"));
			apr_sleep(apr_time_from_sec(1));
			continue;
		}

		/* serve events for 3 second */
		tv.tv_sec = 3;
		tv.tv_usec = 0;
		TR2(("event_loopexit\n"));
		if (-1 == event_loopexit(&tv))
		{
        	interuptable_sleep(30); // sleep to prevent loop of forking process and failing
			gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_loopexit failed");
		}
		TR2(("event_dispatch\n"));
		if (-1 == event_dispatch() && ETIME != errno)
		{
        	interuptable_sleep(30); // sleep to prevent loop of forking process and failing
			gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_dispatch failed");
		}
		if (ax.reload == 1)
		{
			ax.reload = 0;
			/* Check if perfmon is enabled.  If not, exit */
			if (!gpdb_gpperfmon_enabled())
			{
				TR0(("Monitoring has been disabled, exiting..."));
				ax.exit = 1;
				continue;
			}
			else
				read_conf_file(opt.conf_file);
		}
	}
	return 0;
}

static apr_status_t conm_connect(SOCKET* retsock, const char* ipstr, int port)
{
	struct sockaddr_in sa;
	SOCKET sock = -1;
	gpmon_packet_t pkt;
	int e = 0;

	if (-1 == (sock = socket(AF_INET, SOCK_STREAM, 0)))
	{
		e = APR_FROM_OS_ERROR(errno);
		gpmon_warningx(FLINE, e, "socket()");
		goto bail;
	}

	SET_MAXFD(sock);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr(ipstr);
	sa.sin_port = htons(port);

	if (-1 == connect(sock, (void*) &sa, sizeof(sa)))
	{
		e = APR_FROM_OS_ERROR(errno);
		gpmon_warningx(FLINE, e, "connect()");
		goto bail;
	}

	pkt.magic = GPMON_MAGIC;
	pkt.version = GPMON_PACKET_VERSION;
	pkt.pkttype = GPMON_PKTTYPE_HELLO;
	pkt.u.hello.signature = ax.signature;
	if (0 != (e = sendpkt(sock, &pkt)))
	goto bail;

	if (0 != (e = recvpkt(sock, &pkt)))
	goto bail;

	if (pkt.pkttype != GPMON_PKTTYPE_HELLO)
	{
		gpmon_warning(FLINE, "invalid packet type");
		e = APR_EINVAL;
		goto bail;
	}

	*retsock = sock;
	return 0;

	bail:
	if (sock >= 0) closesocket(sock);
	return e;
}

/** ------------------------------------------------------------
 Connection management thread:
 Forever:
 sleep 5
 for any broken connection:
 close socket
 for any broken connection:
 start gpsmon using ssh
 for any broken connection:
 try connect
 */
static void* conm_main(apr_thread_t* thread_, void* arg_)
{
	host_t* tab = ax.hosttab;
	const int tabsz = ax.hosttabsz;
	int i, j, e;
	int* broken;
	int count_broken = 0;
	unsigned int loop;

	broken = malloc(sizeof(*broken) * tabsz);
	CHECKMEM(broken);
	memset(broken, 0, sizeof(*broken) * tabsz);

	for (loop = 0; !ax.exit; loop++)
	{
		apr_sleep(apr_time_from_sec(1));

		if (1 == (loop % 2))
		{
			// find broken connections
			count_broken = 0;
			for (i = 0; i < tabsz; i++)
			{
				host_t* h = &tab[i];
				apr_thread_mutex_lock(h->mutex);
				if (h->sock == 0)
					broken[count_broken++] = i;
				apr_thread_mutex_unlock(h->mutex);
			}
		}

		if (1 == (loop % 16))
		{

			// for any broken connection, start gpsmon
			for (i = 0; i < count_broken;)
			{
				FILE* fp[BATCH];
				const int line_size = 1024;
				char line[line_size];
				memset(fp, 0, sizeof(fp));
				for (j = 0; j < 8 && i < count_broken; j++, i++)
				{
					host_t* h = &tab[broken[i]];
					snprintf(line, line_size, "ssh -o 'BatchMode yes' -o 'StrictHostKeyChecking no'"
							" %s %s/bin/gpsmon -m %" FMT64 " -l %s %s %d ",
							h->hostname, ax.gphome, opt.max_log_size, h->data_dir,
							opt.v ? "-v" : "", ax.port);
					TR0(("Connection to %s lost.  Restarting gpsmon.\n", h->hostname));
					TR1(("%s\n", line));
					fp[j] = popen(line, "w");
					if (fp[j])
					{
						SET_MAXFD(fileno(fp[j]));
						fprintf(fp[j], "%" APR_INT64_T_FMT "\n\n", ax.signature);
					}
				}
				for (j = 0; j < 8; j++)
				{
					if (fp[j])
					{
						fflush(fp[j]);
						pclose(fp[j]);
					}
				}
			}
		}

		// for any broken connection, try connect
		if (9 == (loop % 16))
		{
			for (i = 0; i < count_broken; i++)
			{
				host_t* h = &tab[broken[i]];
				SOCKET sock = 0;

				TR1(("connecting to %s (%s:%d)\n", h->hostname, h->ipstr, ax.port));
				if (0 != (e = conm_connect(&sock, h->ipstr, ax.port)))
				{
					gpmon_warningx(FLINE, e, "cannot connect to %s (%s:%d)",
							h->hostname, h->ipstr, ax.port);
					continue;
				}
				/* connected - set it to valid */
				TR1(("connected to %s (%s:%d)\n", h->hostname, h->ipstr, ax.port));
				apr_thread_mutex_lock(h->mutex);
				h->sock = sock;
				h->event = 0;
				h->eflag = 0;
				apr_thread_mutex_unlock(h->mutex);
			}
		}
	}
	return 0;
}

/* seperate thread for harvest */
static void* harvest_main(apr_thread_t* thread_, void* arg_)
{
	unsigned int loop;
	apr_status_t status;
	unsigned int consecutive_failures = 0;

	for (loop = 1; !ax.exit; loop++)
	{
		apr_sleep(apr_time_from_sec(1));

		if (0 == (loop % opt.harvest_interval))
		{
			/* 	
				PROCESS:
				1) WITH TAIL MUTEX: rename tail files to stage files
				2) WITH TAIL MUTEX: create new tail files
				3) Append data from stage files into _tail files
				4) load data from _tail files into system history
				5) delete _tail files after successful data load

				NOTES:
				1) mutex is held only over rename and creation of new tail files
				   The reason is this is a fast operation and both the main thread doing dumps and the harvest
				   thread uses the tail files

				2) tail files are renamed/moved because this operation is safe to be done while the clients are
				   reading the files.  

				3) The stage files are written over every harvest cycle, so the idea is no client
				   will still be reading the tail files for an entire harvest cycle.  (this is not perfect logic but ok)
			*/

			/* LOCK TAIL MUTEX ****************************************/
			apr_thread_mutex_lock(ax.tailfile_mutex);

			ax._tail_buffer_bytes += ax.tail_buffer_bytes;
			ax.tail_buffer_bytes = 0;

			status = gpdb_rename_tail_files(ax.pool);

			status = gpdb_truncate_tail_files(ax.pool);

			apr_thread_mutex_unlock(ax.tailfile_mutex);
			/* UNLOCK TAIL MUTEX ****************************************/

			status = gpdb_copy_stage_to_harvest_files(ax.pool);

			if (status == APR_SUCCESS)
			{
				status = gpdb_harvest();
			}

			if (status != APR_SUCCESS)
			{
				gpmon_warningx(FLINE, 0, "harvest failure: accumulated tail file size is %llu bytes", ax._tail_buffer_bytes);
				consecutive_failures++;
			}

			if (status == APR_SUCCESS || consecutive_failures > 100 || (ax._tail_buffer_bytes > opt.tail_buffer_max))
			{
				/* 
					delete the data in the _tail file because it has been inserted successfully into history
					we also delete the data without loading if it loading failed more than 100 times as a defensive measure for corrupted data in the file
					better to lose some perf data and self fix than calling support to have them manually find the corrupted perf data and clear it themselves
					we also delete the data if the size of the data is greater than our max data size
				*/
				status = gpdb_empty_harvest_files(ax.pool);

				if (status != APR_SUCCESS)
				{
					gpmon_warningx(FLINE, 0, "error trying to clear harvest files");
				}
				consecutive_failures = 0;
				ax._tail_buffer_bytes = 0;
			}
		}
	}

	return APR_SUCCESS;
}

time_t compute_next_dump_to_file()
{
	time_t current_time = time(NULL);
	return (current_time - (current_time % opt.q) + opt.q);
}

static void gpmmon_main(void)
{
	int e;
	apr_threadattr_t* ta;
	time_t this_cycle_ts = 0;

	/* log check is not exact. do it every X loops */
	int ticks_since_last_log_check = 0;
	const unsigned int log_check_interval = 60;

	const int safety_ticks = 2 * opt.q;
	unsigned int dump_request_time_allowance = opt.q / 2;

	/* DUMP TO FILE */
	time_t next_dump_to_file_ts;
	int dump_to_file_safety_ticks = safety_ticks;

	/* SEND MESSAGE  */
	time_t next_send_msg_ts;
	int send_msg_safety_ticks = safety_ticks;

	/* init timestamps */
	next_dump_to_file_ts = compute_next_dump_to_file();
	next_send_msg_ts = next_dump_to_file_ts - dump_request_time_allowance;

	/* TODO: MPP-3974 might have actually been caused by the spin lock problem... investigate */
	setenv("EVENT_NOKQUEUE", "1", 1);
	setenv("EVENT_NOEVPORT", "1", 1);
	setenv("EVENT_SHOW_METHOD", "1", 1);
	/* MPP-3974.  Hawks systems don't like devpoll */
	setenv("EVENT_NODEVPOLL", "1", 1);

	if (0 != (e = apr_pool_create(&ax.pool, 0)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_pool_create failed");
	}

	if (0 != (e = apr_env_get(&ax.gphome, "GPHOME", ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "GPHOME environment variable not set");
	}

	/* Create mutexes */
	if (0 != (e = apr_thread_mutex_create(&ax.agg_mutex, APR_THREAD_MUTEX_UNNESTED, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "Resource Error: Failed to create agg_mutex");
	}

	if (0 != (e = apr_thread_mutex_create(&ax.tailfile_mutex, APR_THREAD_MUTEX_UNNESTED, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "Resource Error: Failed to create tailfile_mutex");
	}

	if (0 != (e = apr_threadattr_create(&ta, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_threadattr_create failed");
	}

	if (0 != (e = apr_threadattr_detach_set(ta, 1)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_threadattr_detach_set failed");
	}

	/* generate signature */
	if (0 != (e = apr_generate_random_bytes((void*) &ax.signature,
			sizeof(ax.signature))))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_generate_random_bytes failed");
	}
	if (ax.signature < 0)
		ax.signature = ~ax.signature;

	/* get hostlist */
	gethostlist(&ax.hosttab, &ax.hosttabsz);

	/* create the agg */
	if (0 != (e = agg_create(&ax.agg, 1, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "agg_create failed");
	}

	/* spawn conm thread */
	if (0 != (e = apr_thread_create(&conm_th, ta, conm_main, 0, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_thread_create failed");
	}

	/* spawn event thread */
	if (0 != (e = apr_thread_create(&event_th, ta, event_main, 0, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_thread_create failed");
	}

	/* spawn harvest thread */
	if (0 != (e = apr_thread_create(&harvest_th, ta, harvest_main, 0, ax.pool)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_thread_create failed");
	}

	/* main loop */
	while (!ax.exit)
	{
		apr_sleep(apr_time_from_sec(1));

		this_cycle_ts = time(NULL);
		send_msg_safety_ticks--;
		dump_to_file_safety_ticks--;
		ticks_since_last_log_check++;

		/* SEND MESSAGE */
		if ((this_cycle_ts >= next_send_msg_ts) || (send_msg_safety_ticks < 1))
		{
			int i;
			for (i = 0; i < ax.hosttabsz; i++)
			{
				host_t* h = &ax.hosttab[i];
				apr_thread_mutex_lock(h->mutex);
				/* only send to entries with a socket, handling events, and no error */
				TR1(("send dump %d eflag %d\n", h->sock, h->eflag));
				if (h->sock && h->event && !h->eflag)
				{
					if (1 != send(h->sock, "D", 1, 0))
					{
						h->eflag = 1;
						TR1(("at %s: cannot send 'D'ump command [set eflag]\n", FLINE));
					}
				}
				apr_thread_mutex_unlock(h->mutex);
			}

			send_msg_safety_ticks = safety_ticks;
			next_send_msg_ts = this_cycle_ts + opt.q;
		}

		/* DUMP TO FILE */
		if ((this_cycle_ts >= next_dump_to_file_ts) || (dump_to_file_safety_ticks < 1))
		{
			agg_t* newagg = 0;
			agg_t* oldagg = 0;

			/* mutex lock the aggregate data while we dump and dupe it */
			apr_thread_mutex_lock(ax.agg_mutex);

			/* mutex tail files during dump call */
			apr_thread_mutex_lock(ax.tailfile_mutex);

			/* dump the current aggregates */
			if (0 != (e = agg_dump(ax.agg)))
			{
				gpmon_warningx(FLINE, e, "unable to finish aggregation");
			}

			apr_thread_mutex_unlock(ax.tailfile_mutex);

			/* make a new one, copy only recently updated entries */
			if (0 != (e = agg_dup(&newagg, ax.agg, ax.pool)))
			{
        		interuptable_sleep(30); // sleep to prevent loop of forking process and failing
				gpmon_fatalx(FLINE, e, "agg_dup failed");
			}
			oldagg = ax.agg;
			ax.agg = newagg;

			apr_thread_mutex_unlock(ax.agg_mutex);
			/* destroy the old agg */
			agg_destroy(oldagg);

			next_dump_to_file_ts = compute_next_dump_to_file();
			next_send_msg_ts = next_dump_to_file_ts - dump_request_time_allowance;
			dump_to_file_safety_ticks = safety_ticks;
		}

		if (ticks_since_last_log_check > log_check_interval)
		{
			apr_finfo_t finfo;
			char log_file[MAXPATHLEN + 1] =
			{ 0 };
			snprintf(log_file, MAXPATHLEN + 1, "%s/%s", opt.log_dir,
					"gpmmon.log");
			if (0 == apr_stat(&finfo, log_file, APR_FINFO_SIZE, ax.pool))
			{
				if (opt.max_log_size != 0 && finfo.size > opt.max_log_size)
				{
					freopen(log_file, "w+", stdout);
					freopen(log_file, "w+", stderr);
				}
			}
			ticks_since_last_log_check = 0;
		}
	}
}

static void print_version(void)
{
	fprintf(stdout, GPMMON_VERSION_STRING);
	exit(0);
}

static int read_conf_file(char *conffile)
{
	char buffer[1024] = { 0 };
	char *p = NULL;
	FILE *fp = fopen(conffile, "r");
	int section = 0, section_found = 0;

	opt.q = quantum;
	opt.m = min_query_time;
	opt.d = min_detailed_query_time;
	opt.harvest_interval = 120;
	opt.max_log_size = 0;
	opt.log_dir = strdup(DEFAULT_GPMMON_LOGDIR);

	if (!fp)
	{
		fprintf(stderr, "Performance Monitor - Error: Failed to open configuration file.  Using defaults.");
		return 0;
	}

	while (NULL != fgets(buffer, 1024, fp))
	{
		/* remove new line */
		p = gpmon_trim(buffer);

		if (p[0] == '[') /* Start of section */
		{
			if (apr_strnatcasecmp(p, "[gpmmon]") == 0)
				section = section_found = 1;
			else
				section = 0;
		}
		else /* config param */
		{
			char *pName = NULL, *pVal = NULL, *pTemp = NULL;
			/* is it a comment? */
			pTemp = p;
			while (pTemp && *pTemp)
			{
				if (*pTemp == '#')
				{
					*pTemp = '\0';
					break;
				}
				pTemp++;
			}

			pName = strtok(buffer, "=");
			pVal = strtok(NULL, "=");

			if (section == 0 || buffer[0] == 0 || pName == NULL || pVal == NULL)
				continue;

			pName = gpmon_trim(pName);
			pVal = gpmon_trim(pVal);

			if (apr_strnatcasecmp(pName, "quantum") == 0)
			{
				opt.q = atoi(pVal);
			}
			else if (apr_strnatcasecmp(pName, "harvest_interval") == 0)
			{
				opt.harvest_interval = atoi(pVal);
			}
			else if (apr_strnatcasecmp(pName, "min_query_time") == 0)
			{
				opt.m = atoi(pVal);
			}
			else if (apr_strnatcasecmp(pName, "min_detailed_query_time") == 0)
			{
				opt.d = atoi(pVal);
			}
			else if (apr_strnatcasecmp(pName, "verbose") == 0)
			{
				opt.v = atoi(pVal);
			}
			else if (apr_strnatcasecmp(pName, "log_location") == 0)
			{
				/* can't use APR here as the pool is just temporary */
				if (opt.log_dir)
					free(opt.log_dir);

				opt.log_dir = strdup(pVal);
			}
			else if (apr_strnatcasecmp(pName, "tail_buffer_max") == 0)
			{
				opt.tail_buffer_max = apr_atoi64(pVal);
			}
			else if (apr_strnatcasecmp(pName, "max_log_size") == 0)
			{
				opt.max_log_size = apr_atoi64(pVal);
			}
			/* TODO: Log rotation settings
			 else if (apr_strnatcasecmp(pName, "log_rotation_age") == 0)
			 {

			 }
			 else if (apr_strnatcasecmp(pName, "log_rotation_size") == 0)
			 {

			 }
			 else if (apr_strnatcasecmp(pName, "log_truncate_on_rotation") == 0)
			 {
			 if (apr_strnatcasecmp(pVal, "on") == 0)
			 {

			 }
			 */
			else
			{
				fprintf(stderr, "Unknown option %s\n", pName == NULL ? "(NULL)"
						: pName);
			}
		}
	}

	/* check for valid entries */
	if (!section_found)
		fprintf(stderr, "Performance Monitor - Failed to find [gpmmon] section in the "
				"configuration file.  Using default values");

	if (opt.q != 10 && opt.q != 15 && opt.q != 20 && opt.q != 30 && opt.q != 60)
	{
		fprintf(stderr, "Performance Monitor - quantum value must be be either 10, 15, 20, 30 or 60.  Using "
				"default value of 15");
		opt.q = 15;
	}

	if (opt.m < 0)
		opt.m = 0;

	if (opt.d < opt.m && opt.m >= 10)
	{
		opt.d = opt.m;
		fprintf(stderr, "Performance Monitor - min_detail_query_time cannot be less than min_query_time.  "
				"Setting min_detail_query_time equal to min_query_time");
	}

	if (opt.d < 10)
	{
		fprintf(stderr, "Performance Monitor - invalid value for min_detailed_query_time.  "
				"Using default value 60");
		opt.d = 60;
	}

	if (opt.log_dir == NULL)
	{
		char log_dir[MAXPATHLEN + 1] = { 0 };
		snprintf(log_dir, MAXPATHLEN, "%s/%s", ax.master_data_directory,
				"gpperfmon/logs/");
		opt.log_dir = strdup(log_dir);
	}

	if (opt.harvest_interval < 30)
	{
		fprintf(stderr, "Performance Monitor - harvest_interval must be greater than 30.  Using "
				"default value 120");
		opt.harvest_interval = 120;
	}

	if (opt.tail_buffer_max == 0)
	{
		opt.tail_buffer_max = (1LL << 31); /* 2GB */
	}

	verbose = opt.v;
	min_query_time = opt.m;
	min_detailed_query_time = opt.d;
	quantum = opt.q;

	fclose(fp);
	return 0;
}

static void parse_command_line(int argc, const char* const argv[])
{
	apr_getopt_t* os;
	int ch;
	const char* arg;
	const char* bin_start = NULL;
	int e;
	static apr_getopt_option_t option[] =
		{
		{ NULL, '?', 0, "Print help screen" },
		{ NULL, 'D', 1, "Configuration file" },
		{ NULL, 'p', 1, "GPDB port"},
		{ "version", 'V', 0, "Print version number" },
		{ NULL, 0, 0, NULL } };

	apr_pool_t* pool;

	if (0 != (e = apr_pool_create(&pool, 0)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_pool_create failed");
	}

	bin_start = argv[0] + strlen(argv[0]) - 1;
	while (bin_start != argv[0] && *bin_start != '/')
		bin_start--;
	if (bin_start[0] == '/')
		bin_start++;

	opt.pname = bin_start;

	if (0 != (e = apr_getopt_init(&os, pool, argc, argv)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_getopt_init failed");
	}

	while (0 == (e = apr_getopt_long(os, option, &ch, &arg)))
	{
		switch (ch)
		{
		case '?':
			usage(0);
			break;
		case 'D':
			opt.conf_file = strdup(arg);
			break;
		case 'p':
			opt.gpdb_port = strdup(arg);
			break;
		case 'V':
			print_version();
			break;
		}
	}

	if (e != APR_EOF)
		usage("Error: illegal arguments");

	if (opt.conf_file == NULL)
		usage("Error: Missing configuration file");

	apr_pool_destroy(pool);
}

void interuptable_sleep(unsigned int seconds)
{
    int i;
    for (i = 0; i < seconds && !ax.exit; i++)
	    apr_sleep(apr_time_from_sec(1));

    if (ax.exit)
        exit(0);
}

int main(int argc, const char* const argv[])
{
	int db_check_count = 0;

	/* save argc, argv and startup dir so we can restart */
	opt.argc = argc;
	opt.argv = argv;

	if (apr_initialize())
	{
		fprintf(stderr, "Performance Monitor - Internal error, failed to initialize APR.\n");
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		exit(1);
	}

	if ( gpdb_debug_string_lookup_table() != APR_SUCCESS)
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatal(FLINE, "internal consistency check failed at gpperfmon startup");
	}

	parse_command_line(argc, argv);

	/* Set env if we got a port.  This will be picked up by libpq */
	if (opt.gpdb_port)
		setenv("PGPORT", opt.gpdb_port, 1);

	sigemptyset(&unblocksig);
	sigfillset(&blocksig);

	/* Set up signal handlers */
	if ((signal(SIGQUIT, SIGQUIT_handler) == SIG_ERR) ||
		(signal(SIGTERM, SIGTERM_handler) == SIG_ERR) ||
		(signal(SIGHUP, SIGHUP_handler) == SIG_ERR) ||
		(signal(SIGUSR2, SIGUSR2_handler) == SIG_ERR))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatal(FLINE, "Failed to set signal handlers\n");
	}

	/* unblock signals now that we have set up the handlers
	 * and can process them */

	sigprocmask(SIG_SETMASK, &unblocksig, NULL);

	/* Check for gpperfmon database.  If it doesn't exists,
	 * hang around until it does or we get a stop request */
	for (;;)
	{
		int gpperfmon_valid = 0;

		if (ax.exit)
			exit(0);

		gpperfmon_valid = gpdb_validate_gpperfmon();
		if (!gpperfmon_valid)
		{
			/* Don't want to fill up the log with these messages,
			 * so only log it once every 5 minutes */
			if (db_check_count % 5 == 0)
				fprintf(stderr, "Performance Monitor - There was a problem "
								"accessing the gpperfmon database.");

			db_check_count += 1;

            interuptable_sleep(60); // sleep to prevent loop of forking process and failing
		}
		else
			break;
	}

	getconfig();
	read_conf_file(opt.conf_file);

    /* redirect output to log_file */
	/* stdout goes to log: debug and warnings */
	/* stderr goes to pg_log: fatal errors */
	{
        if (gpmon_recursive_mkdir(opt.log_dir))
        {
            fprintf(stderr, "\nPerformance Monitor -- cannot create directory %s", opt.log_dir);
            interuptable_sleep(30); // sleep to prevent loop of forking process and failing
            gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "cannot create directory %s", opt.log_dir);
        }

		char log_file[MAXPATHLEN + 1];
		snprintf(log_file, MAXPATHLEN, "%s/%s", opt.log_dir, "gpmmon.log");
		if (!freopen(log_file, "w", stdout)){
            fprintf(stderr, "\nPerformance Monitor -- failed to open perfmon log file %s\n", log_file);
            interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		    gpmon_fatal(FLINE, "\nfailed (1) to open perfmon log file %s\n", log_file);
        }
	}


	/* check port */
	if (!(0 < ax.port && ax.port < (1 << 16)))
	{
		usage("Error: invalid port number");
	}

	/* check that we are indeed running in a postgres data directory */
	{
		FILE* fp = fopen("pg_hba.conf", "r");
		if (!fp)
		{
			usage("Error: master data directory is not valid; can't find pg_hba.conf");
		}
		SET_MAXFD(fileno(fp));
		fclose(fp);
	}

	/* start up gpperfmon directory */
	{
		char work_dir[MAXPATHLEN+1] = {0};
		strncpy(work_dir, GPMON_DIR, MAXPATHLEN);
		work_dir[MAXPATHLEN] = 0;

        if (gpmon_recursive_mkdir(work_dir))
        {
            fprintf(stderr, "\ncannot create directory %s/%s", ax.master_data_directory, GPMON_DIR);
            interuptable_sleep(30); // sleep to prevent loop of forking process and failing
            gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "cannot create directory %s/%s", ax.master_data_directory, GPMON_DIR);
        }
	}

	gpmmon_main();

	cleanup();

	return 0;
}

#define GPMMON_GETHOSTLIST_LINE_BUFSIZE 1024
static void gethostlist(host_t** rettab, int* rettabsz)
{
	apr_pool_t* pool;
	apr_hash_t* htab;
	int e, i;
	
	char line[GPMMON_GETHOSTLIST_LINE_BUFSIZE];
	apr_array_header_t* hostarray;
	int host_count = 0;
	char **hosts = NULL;
	char **data_dirs = NULL;

	if (0 != (e = apr_pool_create(&pool, NULL)))
	{
        interuptable_sleep(30); // sleep to prevent loop of forking process and failing
		gpmon_fatalx(FLINE, e, "apr_pool_create failed");
	}

	hostarray = apr_array_make(pool, 4, sizeof(host_t));
	CHECKMEM(hostarray);

	htab = apr_hash_make(pool);
	CHECKMEM(htab);

	/* Connect to database, get segment hosts from gp_segment_configuration */
	gpdb_get_hostlist(&host_count, &hosts, &data_dirs, ax.pool);

	for (i = 0; i < host_count; i++)
	{
		host_t *host = apr_array_push(hostarray);
		CHECKMEM(host);
		memset(host, 0, sizeof(*host));
		host->hostname = hosts[i];
		host->data_dir = data_dirs[i];
	}

	if (apr_is_empty_array(hostarray))
	{
        interuptable_sleep(10); // sleep to prevent loop of forking process and failing
		gpmon_fatal(FLINE, "No valid hosts in gp_segment_configuration.");
	}

	/* popen ssh hostname, put host_t entries into htab to
	 eliminate duplicate */
	for (;;)
	{
		host_t host[BATCH];
		FILE* fp[BATCH];
		int i;
		host_t* rec;

		memset(fp, 0, sizeof(fp));
		memset(host, 0, sizeof(host));
		for (i = 0; i < BATCH; i++)
		{
			rec = apr_array_pop(hostarray);
			if (!rec)
				break;
			memcpy(&host[i], rec, sizeof(*rec));
			snprintf(line, GPMMON_GETHOSTLIST_LINE_BUFSIZE, "ssh -o 'BatchMode yes' -o "
				"'StrictHostKeyChecking no' %s hostname", host[i].hostname);
			fp[i] = popen(line, "r");
			if (!fp[i])
			{
				SET_MAXFD(fileno(fp[i]));
				gpmon_warning(FLINE, "cannot ssh to %s ... skipped\n",
						host[i].hostname);
				continue;
			}
		}

		for (i = 0; i < BATCH && host[i].hostname; i++)
		{
			if (! fp[i]) continue;
			line[sizeof(line)-1] = 0;
			if (! fgets(line, sizeof(line), fp[i]))
			{
				gpmon_warning(FLINE, "cannot read hostname of %s",
						host[i].hostname);
				continue;
			}
			if (line[sizeof(line)-1])
			{
				line[sizeof(line)-1] = 0;
				gpmon_warning(FLINE,
						"hostname of %s is too long\n\t%s ...",
						host[i].hostname, line);
				continue;
			}

			host[i].realhostname = gpmon_trim(line);
			rec = apr_hash_get(htab, host[i].realhostname,
					APR_HASH_KEY_STRING);
			if (rec)
			{
				/* FIXME:throw a coin to determine which hostname to use */
				/* for now, just use the last entry that got there */
				rec->hostname = host[i].hostname;
				rec->data_dir = host[i].data_dir;
				apr_thread_mutex_create(&rec->mutex, APR_THREAD_MUTEX_UNNESTED, ax.pool);
			}
			else
			{
				/* insert a new entry */
				rec = apr_palloc(pool, sizeof(*rec));
				CHECKMEM(rec);
				memcpy(rec, &host[i], sizeof(*rec));
				rec->realhostname = apr_pstrdup(pool, rec->realhostname);
				CHECKMEM(rec->realhostname);
				rec->data_dir = apr_pstrdup(pool, rec->data_dir);
				CHECKMEM(rec->data_dir);
				apr_hash_set(htab, rec->realhostname,
						APR_HASH_KEY_STRING, rec);
				apr_thread_mutex_create(&rec->mutex, APR_THREAD_MUTEX_UNNESTED, ax.pool);
			}
		}
		/* clean up file handles */
		for (i = 0; i < BATCH && fp[i]; i++)
		{
			if (fp[i])
			{
				fflush(fp[i]);
				pclose(fp[i]);
			}
		}

		if (i < BATCH)
		break;
	}

	{
		int max = apr_hash_count(htab);
		apr_hash_index_t* hi;
		int i;
		host_t* ret = malloc(sizeof(host_t) * (max+1));
		CHECKMEM(ret);

		/* NOTE: ret and its contents are malloc-ed */

		for (i = 0, hi = apr_hash_first(pool, htab);
				hi; hi = apr_hash_next(hi), i++)
		{
			void *val;
			struct hostent *hp;

			apr_hash_this(hi, 0, 0, &val);
			memcpy(&ret[i], val, sizeof(ret[i]));
			ret[i].hostname = strdup(ret[i].hostname);
			CHECKMEM(ret[i].hostname);
			ret[i].realhostname = strdup(ret[i].realhostname);
			CHECKMEM(ret[i].realhostname);
			hp = gethostbyname(ret[i].hostname);
			if (! hp)
			{
        		interuptable_sleep(30); // sleep to prevent loop of forking process and failing
				gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno),
						"cannot resolve host %s",
						ret[i].hostname);
			}
			ret[i].ipstr =
			strdup(inet_ntoa(*(struct in_addr*) hp->h_addr_list[0]));
			CHECKMEM(ret[i].ipstr);
		}
		memset(&ret[max], 0, sizeof(ret[max]));

		*rettab = ret;
		*rettabsz = max;
	}

	apr_pool_destroy(pool);
}

/* send a packet thru sock */
static apr_status_t sendpkt(int sock, const gpmon_packet_t* pkt)
{
	const char* p = (const char*) pkt;
	const char* q = p + sizeof(*pkt);
	while (p < q)
	{
		int n = send(sock, p, q - p, 0);
		if (n == -1)
		{
			switch (errno)
			{
			case EINTR:
			case EAGAIN:
				continue;
			}
			return APR_FROM_OS_ERROR(errno);
		}
		p += n;
	}
	return 0;
}

/* recv a packet thru sock */
static apr_status_t recvpkt(int sock, gpmon_packet_t* pkt)
{
	char* p = (char*) pkt;
	char* q = p + sizeof(*pkt);
	while (p < q)
	{
		int n = recv(sock, p, q - p, 0);
		if (n == -1)
		{
			switch (errno)
			{
			case EINTR:
			case EAGAIN:
				continue;
			}
			return APR_FROM_OS_ERROR(errno);
		}
		p += n;
		if (n == 0)
			return APR_FROM_OS_ERROR(EINTR);
	}

	TR2(("read packet from sock %d\n", sock));
	return gpmon_ntohpkt(pkt);
}


static void getconfig(void)
{
	char *hostname = NULL;
	char *master_data_directory = NULL;
	int rc = 0;

	static apr_pool_t *pool = NULL;

	int port = gpdb_get_gpmon_port();
	ax.port = port;

	if (pool == NULL)
	{
		if (APR_SUCCESS != (rc = apr_pool_create(&pool, NULL)))
		{
        	interuptable_sleep(30); // sleep to prevent loop of forking process and failing
			gpmon_fatalx(FLINE, rc, "Failed to create APR pool\n");
		}
	}

	/* fetch datadir */
	gpdb_get_master_data_dir(&hostname, &master_data_directory, pool);

	if (ax.master_data_directory == NULL)
	{
		ax.master_data_directory = strdup(master_data_directory);
		CHECKMEM(ax.master_data_directory);
	}

	/* clear pool for next call */
	apr_pool_clear(pool);
}
