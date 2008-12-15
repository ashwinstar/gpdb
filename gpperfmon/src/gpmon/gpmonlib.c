#undef GP_VERSION
#include "postgres_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include "gpmonlib.h"
#include "apr_time.h"
#include "apr_queue.h"
#include "apr_atomic.h"
#include "apr_lib.h"

#if APR_IS_BIGENDIAN
#define local_htonll(n)  (n)
#define local_ntohll(n)  (n)
#else
#define local_htonll(n)  ((((apr_uint64_t) htonl(n)) << 32LL) | htonl((n) >> 32LL))
#define local_ntohll(n)  ((((apr_uint64_t) ntohl(n)) << 32LL) | (apr_uint32_t) ntohl(((apr_uint64_t)n) >> 32LL))
#endif

#define GPMON_NTOHS(x) x = ntohs(x)
#define GPMON_NTOHL(x) x = ntohl(x)
#define GPMON_NTOHLL(x) x = local_ntohll(x)

apr_status_t gpmon_ntohpkt(gpmon_packet_t* pkt)
{
	static apr_int64_t last_err_sec = 0;

    if (pkt->magic != GPMON_MAGIC) {
	int i;

	GPMON_NTOHL(pkt->magic);
	GPMON_NTOHS(pkt->version);
	GPMON_NTOHS(pkt->pkttype);
	switch (pkt->pkttype) {
	case GPMON_PKTTYPE_HELLO: 
	    {
		gpmon_hello_t* p = &pkt->u.hello;
		GPMON_NTOHLL(p->signature);
		break;
	    }
	case GPMON_PKTTYPE_METRICS: 
	    {
		gpmon_metrics_t* p = &pkt->u.metrics;
		GPMON_NTOHLL(p->mem.total);
		GPMON_NTOHLL(p->mem.used);
		GPMON_NTOHLL(p->mem.actual_used);
		GPMON_NTOHLL(p->mem.actual_free);
		GPMON_NTOHLL(p->swap.total);
		GPMON_NTOHLL(p->swap.used);
		GPMON_NTOHLL(p->swap.page_in);
		GPMON_NTOHLL(p->swap.page_out);
		GPMON_NTOHLL(p->disk.rcount);
		GPMON_NTOHLL(p->disk.wcount);
		GPMON_NTOHLL(p->disk.rbytes);
		GPMON_NTOHLL(p->disk.wbytes);
		GPMON_NTOHLL(p->net.rcount);
		GPMON_NTOHLL(p->net.wcount);
		GPMON_NTOHLL(p->net.rbytes);
		GPMON_NTOHLL(p->net.wbytes);
		break;
	    }
	case GPMON_PKTTYPE_QLOG:
	    {
		gpmon_qlog_t* p = &pkt->u.qlog;
         	GPMON_NTOHL(p->key.tmid);
         	GPMON_NTOHL(p->key.ssid);
		GPMON_NTOHL(p->key.ccnt);
		GPMON_NTOHL(p->cost);
		GPMON_NTOHL(p->tsubmit);
		GPMON_NTOHL(p->tstart);
		GPMON_NTOHL(p->tfin);
		GPMON_NTOHL(p->status);
		GPMON_NTOHLL(p->rows_out);
		GPMON_NTOHL(p->_segmax);
		for (i = 0; i < sizeof(p->_segcpu) / sizeof(p->_segcpu[0]); i++)
		    GPMON_NTOHL(p->_segcpu[i]);
		break;
	    }
	case GPMON_PKTTYPE_QEXEC:
	    {
		gpmon_qexec_t* p = &pkt->u.qexec;
         	GPMON_NTOHL(p->key.tmid);
         	GPMON_NTOHL(p->key.ssid);
		GPMON_NTOHS(p->key.ccnt);
		GPMON_NTOHS(p->key.segid);
		GPMON_NTOHL(p->key.pid);
		GPMON_NTOHS(p->key.nid);
		GPMON_NTOHS(p->key.zero);
		GPMON_NTOHL(p->pnid);
		GPMON_NTOHL(p->tstart);
		GPMON_NTOHL(p->tduration);
		GPMON_NTOHLL(p->p_mem);
		GPMON_NTOHLL(p->p_memmax);
                GPMON_NTOHLL(p->_cpu_elapsed);
		GPMON_NTOHL(p->_p_metrics.fd_cnt);
		GPMON_NTOHLL(p->_p_metrics.mem.size);
		GPMON_NTOHLL(p->_p_metrics.mem.resident);
		GPMON_NTOHLL(p->_p_metrics.mem.share);
          	GPMON_NTOHLL(p->rowsout);
          	GPMON_NTOHLL(p->rowsout_est);
		for (i = 0; i < sizeof(p->m) / sizeof(p->m[0]); i++) {
		    GPMON_NTOHLL(p->m[i].value);
		    GPMON_NTOHLL(p->m[i].valuemax);
		}
		break;
	    }
	}
    }

    if (pkt->magic != GPMON_MAGIC) 
	{
		apr_int64_t now = apr_time_sec(apr_time_now());
		if (now - last_err_sec >= GPMON_PACKET_ERR_LOG_TIME)
		{
			last_err_sec = now;
			gpmon_warning(FLINE, "bad packet (magic number mismatch)");
		}
		return APR_EINVAL;
    }

    if (pkt->version != GPMON_PACKET_VERSION) 
	{
		apr_int64_t now = apr_time_sec(apr_time_now());
		if (now - last_err_sec >= GPMON_PACKET_ERR_LOG_TIME)
		{
			last_err_sec = now;
			gpmon_warning(FLINE, "bad packet (version %d, expected %d)",
		    			  pkt->version, GPMON_PACKET_VERSION);
		}
		return APR_EINVAL;
    }

    if (! (GPMON_PKTTYPE_NONE < pkt->pkttype && pkt->pkttype < GPMON_PKTTYPE_MAX)) 
	{
		apr_int64_t now = apr_time_sec(apr_time_now());
		if (now - last_err_sec >= GPMON_PACKET_ERR_LOG_TIME)
		{
			last_err_sec = now;
			gpmon_warning(FLINE, "bad packet (unexpected packet type %d)", 
		    			  pkt->pkttype);
		}
		return APR_EINVAL;
    }

    return 0;
}



static char* datetime(void)
{
    static char buf[100];
    apr_time_exp_t t;
    apr_time_exp_lt(&t, apr_time_now());

    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
            1900 + t.tm_year, 1 + t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}



int gpmon_print(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "%s|:-LOG: ", datetime());
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    return 0;
}


int gpmon_fatal(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s|:-FATAL: [INTERNAL ERROR %s] ", datetime(), fline);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n          ... exiting\n");
    exit(1);
    return 0;
}


int gpmon_fatalx(const char* fline, int e, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s|:-FATAL: [INTERNAL ERROR %s] ", datetime(), fline);
    vfprintf(stderr, fmt, ap);
    if (e) {
	char msg[512];
	fprintf(stderr, "\n\terror %d (%s)", e, apr_strerror(e, msg, sizeof(msg)));
    }
    fprintf(stderr, "\n\t... exiting\n");
    exit(1);
    return 0;
}



int gpmon_warning(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s|:-WARNING: [%s] ", datetime(), fline);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

int gpmon_warningx(const char* fline, int e, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s|:-WARNING: [%s] ", datetime(), fline);
    vfprintf(stderr, fmt, ap);
    if (e) {
	char msg[512];
	fprintf(stderr, "\n\terror %d (%s)", e, apr_strerror(e, msg, sizeof(msg)));
    }
    fprintf(stderr, "\n");
    return 0;
}





gpmon_spin_t* gpmon_spin_lock(gpmon_spin_t* spin)
{
    do ; while (0 != (apr_atomic_xchg32(spin, 1)));
    return spin;
}


gpmon_spin_t* gpmon_spin_unlock(gpmon_spin_t* spin)
{
    *spin = 0;
    return 0;
}


const char* gpmon_qlog_status_string(int gpmon_qlog_status)
{
    switch (gpmon_qlog_status) {
    case GPMON_QLOG_STATUS_SILENT: return "silent";
    case GPMON_QLOG_STATUS_SUBMIT: return "submit";
    case GPMON_QLOG_STATUS_START: return "start";
    case GPMON_QLOG_STATUS_DONE: return "done";
    case GPMON_QLOG_STATUS_ERROR: return "abort";
    }
    return "unknown";
}


/* remove whitespaces from front & back of s */
char* gpmon_trim(char* s)
{
    char* p = s;
    char* q = s + strlen(s);
    for ( ; p < q && apr_isspace(*p); p++);
    for ( ; p < q && apr_isspace(q[-1]); q--);
    *q = 0;
    return p;
}


/* datetime, e.g. 2004-02-14  23:50:02 */
char* gpmon_datetime(apr_time_t t, char str[24])
{
    apr_time_exp_t tm;
    int e;
    str[0] = 0;
    if (t < 0) 
	t = 0;
    if (0 != (e = apr_time_exp_lt(&tm, t))) {
	gpmon_warningx(FLINE, e, "apr_time_exp_lt failed");
	return str;
    }
    sprintf(str, "%04d-%02d-%02d %02d:%02d:%02d", 
	    1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday, 
	    tm.tm_hour, tm.tm_min, tm.tm_sec);
    return str;
}

/* get status from query text file */
apr_int32_t get_query_status(apr_int32_t tmid, apr_int32_t ssid,
							 apr_int32_t ccnt)
{
	char fname[GPMON_DIR_MAX_PATH];
	FILE *fp;
	apr_int32_t status = GPMON_QLOG_STATUS_INVALID;

	snprintf(fname, GPMON_DIR_MAX_PATH, "%sq%d-%d-%d.txt", GPMON_DIR, tmid, ssid, ccnt);

	fp = fopen(fname, "r");
	if (!fp)
		return GPMON_QLOG_STATUS_INVALID;

	if (0 != fseek(fp, -1, SEEK_END))
	{
		fclose(fp);
		return GPMON_QLOG_STATUS_INVALID;
	}

	fscanf(fp, "%d", &status);
	fclose(fp);
	return status;
}
