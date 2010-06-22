#include "postgres.h"
#include "c.h"
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifdef WIN32
#include <io.h>
#endif
#include "postgres.h"
#include "libpq/pqsignal.h"
#include "gpmon/gpmon.h"

#include "utils/memutils.h"

#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"

/* Extern stuff */
extern const char * show_session_authorization(void);
extern char *get_database_name(Oid dbid);

static void gpmon_record(apr_int32_t tmid, apr_int32_t ssid, apr_int32_t ccnt,
						 const char* type, /* "qtext", "qplan" */
						 const char* content /* the query text or plan */
						 );
static void gpmon_record_update(apr_int32_t tmid, apr_int32_t ssid,
								apr_int32_t ccnt, apr_int32_t status);


struct  {
    int    gxsock;
	pid_t  pid;
	struct sockaddr_in gxaddr;
} gpmon = {0};

int64 gpmon_tick = 0;

void gpmon_sig_handler(int sig);

void gpmon_sig_handler(int sig) 
{
	gpmon_tick++;
}


void gpmon_init(void)
{
	struct itimerval tv;
#ifndef WIN32
	pqsigfunc sfunc;
#endif
	pid_t pid = getpid();
	int sock;

	if (pid == gpmon.pid)
		return;
#ifndef WIN32
	sfunc = pqsignal(SIGVTALRM, gpmon_sig_handler);
	if (sfunc == SIG_ERR) {
		elog(WARNING, "gpmon: unable to set signal handler for SIGVTALRM (%m)");
	}
	else if (sfunc == gpmon_sig_handler) {
		close(gpmon.gxsock); 
		gpmon.gxsock = -1;
	}
	else {
		Assert(sfunc == 0);
	}
#endif

	tv.it_interval.tv_sec = gp_gpperfmon_send_interval;
	tv.it_interval.tv_usec = 0;
	tv.it_value = tv.it_interval;
#ifndef WIN32
	if (-1 == setitimer(ITIMER_VIRTUAL, &tv, 0)) {
		elog(WARNING, "gpmon: unable to start timer (%m)");
	}
#endif 

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		elog(WARNING, "gpmon: cannot create socket (%m)");
	}
#ifndef WIN32
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        elog(WARNING, "fcntl(F_SETFL, O_NONBLOCK) failed");
    }
    if (fcntl(sock, F_SETFD, 1) == -1) {
        elog(WARNING, "fcntl(F_SETFD) failed");
    }
#endif 
	gpmon.gxsock = sock;
	memset(&gpmon.gxaddr, 0, sizeof(gpmon.gxaddr));
	gpmon.gxaddr.sin_family = AF_INET;
	gpmon.gxaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	gpmon.gxaddr.sin_port = htons(gpperfmon_port);
	gpmon.pid = pid;
}

static void gpmon_record(apr_int32_t tmid, apr_int32_t ssid, apr_int32_t ccnt,
				  const char* type,
				  const char* content)
{
	char fname[GPMON_DIR_MAX_PATH];
	FILE* fp;
	int len = strlen(content);

	snprintf(fname, GPMON_DIR_MAX_PATH, "%sq%d-%d-%d.txt", GPMON_DIR, tmid, ssid, ccnt);

	fp = fopen(fname, "a");
	if (!fp)
		return;

	fprintf(fp, "%d %s\n", len, type);
	fwrite(content, 1, len, fp);
	fprintf(fp, "\n%d", GPMON_QLOG_STATUS_SUBMIT);

	fclose(fp);
}

void gpmon_record_update(apr_int32_t tmid, apr_int32_t ssid, apr_int32_t ccnt,
						 apr_int32_t status)
{
	char fname[GPMON_DIR_MAX_PATH];
	FILE *fp;

	snprintf(fname, GPMON_DIR_MAX_PATH, "%sq%d-%d-%d.txt", GPMON_DIR, tmid, ssid, ccnt);

	fp = fopen(fname, "r+");

	if (!fp)
		return;

	if (0 == fseek(fp, -1, SEEK_END))
	{
		fprintf(fp, "%d", status);
	}
	fclose(fp);
}

void gpmon_gettmid(apr_int32_t* tmid)
{
    char buff[TMGIDSIZE] = {0};
    apr_int32_t xid;
    getDistributedTransactionIdentifier(buff);
    sscanf(buff, "%d-%d", tmid, &xid);
} 


void gpmon_send(gpmon_packet_t* p)
{
	if (p->magic != GPMON_MAGIC)  {
		elog(WARNING, "gpmon - bad magic %x", p->magic);
		return;
	}

	if (p->pkttype == GPMON_PKTTYPE_QEXEC) {
		p->u.qexec.p_mem = TopMemoryContext->allBytesAlloc - TopMemoryContext->allBytesFreed;
		p->u.qexec.p_memmax = TopMemoryContext->maxBytesHeld;
	}
	
	if (gpmon.gxsock > 0) {
		int n = sizeof(*p);
		if (n != sendto(gpmon.gxsock, (const char *)p, n, 0, 
						(struct sockaddr*) &gpmon.gxaddr, 
						sizeof(gpmon.gxaddr))) {
			elog(WARNING, "gpmon: cannot send (%m socket %d)", gpmon.gxsock);
		}
	}
}

#define GPMON_QLOG_PACKET_ASSERTS(gpmonPacket) \
		Assert(gp_enable_gpperfmon && Gp_role == GP_ROLE_DISPATCH); \
		Assert(gpmonPacket); \
		Assert(gpmonPacket->magic == GPMON_MAGIC); \
		Assert(gpmonPacket->version == GPMON_PACKET_VERSION); \
		Assert(gpmonPacket->pkttype == GPMON_PKTTYPE_QLOG)

/**
 * To be used when gpmon packet has not been inited already.
 */
void gpmon_qlog_packet_init(gpmon_packet_t *gpmonPacket)
{
	const char *username = NULL;
	char *dbname = NULL;

	Assert(gp_enable_gpperfmon && Gp_role == GP_ROLE_DISPATCH);
	Assert(gpmonPacket);
	Assert(gpmonPacket->magic != GPMON_MAGIC);
	
	gpmonPacket->magic = GPMON_MAGIC;
	gpmonPacket->version = GPMON_PACKET_VERSION;
	gpmonPacket->pkttype = GPMON_PKTTYPE_QLOG;

	gpmon_gettmid(&gpmonPacket->u.qlog.key.tmid);
	gpmonPacket->u.qlog.key.ssid = gp_session_id;

	username = show_session_authorization(); /* does not have to be freed */
	/* User Id.  We use session authorization (so to make sense with session id) */
	snprintf(gpmonPacket->u.qlog.user, sizeof(gpmonPacket->u.qlog.user), "%s",
			username ? username : "");

	/* DB Id */
	dbname = get_database_name(MyDatabaseId); /* needs to be freed */
	snprintf(gpmonPacket->u.qlog.db, sizeof(gpmonPacket->u.qlog.db), "%s", dbname ? dbname : ""); 
	if(dbname)
	{
		pfree(dbname);
		dbname = NULL;
	}

	/* Fix up command count */
	gpmonPacket->u.qlog.key.ccnt = gp_command_count;
}

/**
 * Call this method when query is submitted.
 */
void gpmon_qlog_query_submit(gpmon_packet_t *gpmonPacket)
{
	struct timeval tv;

	GPMON_QLOG_PACKET_ASSERTS(gpmonPacket);

	gettimeofday(&tv, 0);
	
	gpmonPacket->u.qlog.status = GPMON_QLOG_STATUS_SUBMIT;
	gpmonPacket->u.qlog.tsubmit = tv.tv_sec;
	
	gpmon_record_update(gpmonPacket->u.qlog.key.tmid,
			gpmonPacket->u.qlog.key.ssid,
			gpmonPacket->u.qlog.key.ccnt,
			gpmonPacket->u.qlog.status);
	
	gpmon_send(gpmonPacket);
}

/**
 * Call this method to let gpmon know the query text.
 */
void gpmon_qlog_query_text(const gpmon_packet_t *gpmonPacket, const char *queryText)
{
	GPMON_QLOG_PACKET_ASSERTS(gpmonPacket);

	gpmon_record(gpmonPacket->u.qlog.key.tmid,
			gpmonPacket->u.qlog.key.ssid,
			gpmonPacket->u.qlog.key.ccnt,
			"qtext", queryText);
}

/**
 * Call this method when query starts executing.
 */
void gpmon_qlog_query_start(gpmon_packet_t *gpmonPacket)
{
	struct timeval tv;

	GPMON_QLOG_PACKET_ASSERTS(gpmonPacket);

	gettimeofday(&tv, 0);
	
	gpmonPacket->u.qlog.status = GPMON_QLOG_STATUS_START;
	gpmonPacket->u.qlog.tstart = tv.tv_sec;
	
	gpmon_record_update(gpmonPacket->u.qlog.key.tmid,
			gpmonPacket->u.qlog.key.ssid,
			gpmonPacket->u.qlog.key.ccnt,
			gpmonPacket->u.qlog.status);
	
	gpmon_send(gpmonPacket);
}

/**
 * Call this method when query finishes executing.
 */
void gpmon_qlog_query_end(gpmon_packet_t *gpmonPacket)
{
	struct timeval tv;

	GPMON_QLOG_PACKET_ASSERTS(gpmonPacket);
	Assert(gpmonPacket->u.qlog.status == GPMON_QLOG_STATUS_START);
	gettimeofday(&tv, 0);
	
	gpmonPacket->u.qlog.status = GPMON_QLOG_STATUS_DONE;
	gpmonPacket->u.qlog.tfin = tv.tv_sec;
	
	gpmon_record_update(gpmonPacket->u.qlog.key.tmid,
			gpmonPacket->u.qlog.key.ssid,
			gpmonPacket->u.qlog.key.ccnt,
			gpmonPacket->u.qlog.status);
	
	gpmon_send(gpmonPacket);
}

/**
 * Call this method when query errored out.
 */
void gpmon_qlog_query_error(gpmon_packet_t *gpmonPacket)
{
	struct timeval tv;

	GPMON_QLOG_PACKET_ASSERTS(gpmonPacket);
	Assert(gpmonPacket->u.qlog.status == GPMON_QLOG_STATUS_START ||
		   gpmonPacket->u.qlog.status == GPMON_QLOG_STATUS_SUBMIT);

	gettimeofday(&tv, 0);
	
	gpmonPacket->u.qlog.status = GPMON_QLOG_STATUS_ERROR;
	gpmonPacket->u.qlog.tfin = tv.tv_sec;
	
	gpmon_record_update(gpmonPacket->u.qlog.key.tmid,
			gpmonPacket->u.qlog.key.ssid,
			gpmonPacket->u.qlog.key.ccnt,
			gpmonPacket->u.qlog.status);
	
	gpmon_send(gpmonPacket);
}


