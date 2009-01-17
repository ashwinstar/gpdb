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
	pqsigfunc sfunc;
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
#else
	elog(ERROR,"Not ported to win32 yet");
#endif

	tv.it_interval.tv_sec = gp_gpperfmon_send_interval;
	tv.it_interval.tv_usec = 0;
	tv.it_value = tv.it_interval;
	if (-1 == setitimer(ITIMER_VIRTUAL, &tv, 0)) {
		elog(WARNING, "gpmon: unable to start timer (%m)");
	}

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

void gpmon_record(apr_int32_t tmid, apr_int32_t ssid, apr_int32_t ccnt,
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

	fprintf(fp, "%d %s\n", len + 1, type);
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
	if (!Gp_is_primary)
		return;

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
