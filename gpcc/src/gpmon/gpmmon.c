#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/param.h>
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


struct {
    int argc;
    const char* const *argv;
    const char* pname;
    int max_fd;			/* this is the max fd value we ever seen */
    int v;
    int V;
    int q;
    int m;
    int d;
} opt = {0};

int verbose = 0;		  /* == opt.v */
int very_verbose = 0;		  /* == opt.V */
int quantum = 15;		  /* == opt.q */
int min_query_time = 60;	  /* == opt.m */
int min_detailed_query_time = 60; /* == opt.d */ 

#define SET_MAXFD(fd)  if (fd < opt.max_fd) ; else opt.max_fd = fd

int gpmmon_quantum(void)
{
    return opt.q;
}


static void usage(const char* msg)
{
    fprintf(stderr, "\nusage: %s [options]\n\n", opt.pname);
    fprintf(stderr, "\toptions:\n");
    fprintf(stderr, "\t-?         : print this help screen\n");
    fprintf(stderr, "\t-v         : verbose\n");
    fprintf(stderr, "\t-q quantum : # seconds between updates\n");
    fprintf(stderr, "\t-m minruntime : # seconds minimum query runtime to log\n");
    fprintf(stderr, "\t-d mindetail : # seconds minimum query runtime to log detailed\n"
            "\t               query information\n");
    fprintf(stderr, "\t--version  : print version information\n\n");
    if (msg) 
	fprintf(stderr, "%s\n\n", msg);

    exit(msg ? 1 : 0);
}


typedef struct host_t host_t;
struct host_t {
    gpmon_spin_t spin;
    int   sock;			/* socket connected to gpsmon on this host */
    int   eflag;		/* flag: socket has error */
    struct event* event;	/* points to _event if set */
    struct event _event;	/* static */
    char* hostname;		/* static */
    char* realhostname;		/* static */
    char* ipstr;		/* static */
};

static struct {
    int         port;		/* port of gpsmon */
    int         pgpid;		/* postgres pid */
    apr_pool_t* pool;		/* pool */
    char*       gphome;		/* GPHOME env variable */
    apr_int64_t signature;	/* a large random number */

    host_t*     hosttab;	/* multi-home filtered machines */
    int         hosttabsz;	/* # machines */

    char*       master_data_directory;
    char        startup_directory[MAXPATHLEN];

    gpmon_spin_t agg_spin;
    agg_t*       agg;

    int         restart;	/* TRUE if we need to restart */
    unsigned int gpmon_tick;
} ax = {0};



static void gethostlist(host_t** tab, int* tabsz);
static int checkhostlist(void);
static int getpgpid(void);
static int pgcheck(int pid);
static int getconfig(void);
static apr_status_t sendpkt(int sock, const gpmon_packet_t* pkt);
static apr_status_t recvpkt(int sock, gpmon_packet_t* pkt);


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
    gpmon_spin_t* spin = 0;

    TR2(("recv_from_gx sock %d host %s (%s:%d)\n", sock, 
	 h->hostname, h->ipstr, ax.port));

    if (! (event & EV_READ))
	return;

    spin = gpmon_spin_lock(&h->spin);
    if (h->event) {
	if (!h->eflag) {
	    char* p = (char*) &pktbuf;
	    char* q = p + sizeof(pktbuf);
	    while (p < q) {
		int n = recv(sock, p, q - p, 0);
		if (n == -1) {
		    switch (errno) {
		    case EINTR: case EAGAIN: continue;
		    }
		    gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno),
				   "cannot get packet from host %s (%s:%d)",
				   h->hostname, h->ipstr, ax.port);
		    break;
		}
		p += n;
		if (p == (char*) &pktbuf) {
		    TR1(("at %s: connection dropped by host %s (%s:%d) [set eflag]\n", 
			FLINE, h->hostname, h->ipstr, ax.port));
		    h->eflag = 1;
		    break;
		}
	    }
	}
	if (h->eflag) {
	    event_del(h->event);
	    h->event = 0;
	}
	else {
	    pkt = &pktbuf;
	    TR2(("received packet %d from %s (%s:%d)\n",
		 pkt->pkttype, h->hostname, h->ipstr, ax.port));
	}
    }
    spin = gpmon_spin_unlock(spin);

    if (pkt) {
	spin = gpmon_spin_lock(&ax.agg_spin);
	e = agg_put(ax.agg, pkt);
	spin = gpmon_spin_unlock(spin);
	if (e)
	    gpmon_fatalx(FLINE, e, "agg_put failed");
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

    if (! event_init()) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_init failed");
    }
    while (! ax.restart) {
	int i;
	int count_valid = 0;

	/* setup new events */
	TR2(("event_main: scan hosttab\n"));

	for (i = 0; i < tabsz; i++) {
	    host_t* h = &tab[i];
	    SOCKET close_sock = 0;
	    gpmon_spin_t* spin = gpmon_spin_lock(&h->spin);
	    if (h->sock) {
		if (h->eflag) {
		    if (h->event) {
			event_del(h->event);
			h->event = 0;
		    }
		    close_sock = h->sock;
		    h->sock = 0;
		}
		else {
		    count_valid++;
		    if (!h->event) {
			/* set up the event */
			h->event = &h->_event;
			event_set(h->event, h->sock, EV_READ | EV_PERSIST, 
				  recv_from_gx, h);
			if (event_add(h->event, 0)) {
			    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
					 "event_add failed");
			}
		    }
		}
	    }
	    spin = gpmon_spin_unlock(spin);
	    if (close_sock) {
		TR2(("closing socket %d\n", close_sock));
		closesocket(close_sock);
	    }
	}

	if (count_valid == 0) {
	    TR2(("no valid connection, sleep 1\n"));
	    apr_sleep(apr_time_from_sec(1));
	    continue;
	}

	/* serve events for 3 second */
	tv.tv_sec = 3;
	tv.tv_usec = 0;
	TR2(("event_loopexit\n"));
	if (-1 == event_loopexit(&tv))  {
	    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno),
			 "event_loopexit failed");
	}
	TR2(("event_dispatch\n"));
	if (-1 == event_dispatch() && ETIME != errno) {
	    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno),
			 "event_dispatch failed");
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

    if (-1 == (sock = socket(AF_INET, SOCK_STREAM, 0))) {
	e = APR_FROM_OS_ERROR(errno);
	gpmon_warningx(FLINE, e, "socket()");
	goto bail;
    }

    SET_MAXFD(sock);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(ipstr);
    sa.sin_port = htons(port);
	    
    if (-1 == connect(sock, (void*) &sa, sizeof(sa))) {
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

    if (pkt.pkttype != GPMON_PKTTYPE_HELLO) {
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
    int   i, j, e;
    int* broken;
    int  count_broken = 0;
    gpmon_spin_t* spin;
    unsigned int loop;

    broken = malloc(sizeof(*broken) * tabsz);
    CHECKMEM(broken);
    memset(broken, 0, sizeof(*broken) * tabsz);

    for (loop = 0; !ax.restart; loop++) {
	apr_sleep(apr_time_from_sec(1));

	if (1 == (loop % 2)) {
	    // find broken connections
	    count_broken = 0;
	    for (i = 0; i < tabsz; i++) {
		host_t* h = &tab[i];
		spin = gpmon_spin_lock(&h->spin);
		if (h->sock == 0)
		    broken[count_broken++] = i;
		spin = gpmon_spin_unlock(spin);
	    }
	}

	if (1 == (loop % 16)) {

	    // for any broken connection, start gpsmon
	    for (i = 0; i < count_broken; ) {
		FILE* fp[BATCH];
		char line[1024];
		memset(fp, 0, sizeof(fp));
		for (j = 0; j < 8 && i < count_broken; j++, i++) {
		    host_t* h = &tab[broken[i]];
		    sprintf(line, "ssh -o 'BatchMode yes' -o 'StrictHostKeyChecking no'"
			    " %s %s/bin/gpsmon %s %d ",
			    h->hostname, ax.gphome, 
			    (opt.V ? "-V" : (opt.v ? "-v" : "")),
			    ax.port);
		    TR1(("%s\n", line));
		    fp[j] = popen(line, "w");
		    if (fp[j]) {
			SET_MAXFD(fileno(fp[j]));
			fprintf(fp[j], "%" APR_INT64_T_FMT "\n\n", ax.signature);
		    }
		}
		for (j = 0; j < 8; j++) {
		    if (fp[j]) {
			fflush(fp[j]);
			pclose(fp[j]);
		    }
		}
	    }
	}
	
	// for any broken connection, try connect
	if (9 == (loop % 16)) {
	    for (i = 0; i < count_broken; i++) {
		host_t* h = &tab[broken[i]];
		SOCKET sock = 0;

		TR1(("connecting to %s (%s:%d)\n", h->hostname, h->ipstr, ax.port));
		if (0 != (e = conm_connect(&sock, h->ipstr, ax.port))) {
		    gpmon_warningx(FLINE, e, "cannot connect to %s (%s:%d)", 
				   h->hostname, h->ipstr, ax.port);
		    continue;
		}
		/* connected - set it to valid */
		TR1(("connected to %s (%s:%d)\n", h->hostname, h->ipstr, ax.port));
		spin = gpmon_spin_lock(&h->spin);
		h->sock = sock;
		h->event = 0;
		h->eflag = 0;
		spin = gpmon_spin_unlock(spin);
	    }
	}
    }
    return 0;
}



static void gpmmon_main(void)
{
    int e;
    apr_threadattr_t* ta;
    apr_thread_t* tp;

    setenv("EVENT_NOKQUEUE", "1", 1);
    setenv("EVENT_NOEVPORT", "1", 1);
    setenv("EVENT_SHOW_METHOD", "1",1);

    if (0 != (e = apr_pool_create(&ax.pool, 0))) {
	gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }

    if (0 != (e = apr_env_get(&ax.gphome, "GPHOME", ax.pool))) {
	gpmon_fatalx(FLINE, e, "GPHOME environment variable not set");
    }

    if (0 != (e = apr_threadattr_create(&ta, ax.pool))) {
	gpmon_fatalx(FLINE, e, "apr_threadattr_create failed");
    }

    if (0 != (e = apr_threadattr_detach_set(ta, 1))) {
	gpmon_fatalx(FLINE, e, "apr_threadattr_detach_set failed");
    }


    /* generate signature */
    if (0 != (e = apr_generate_random_bytes((void*) &ax.signature, sizeof(ax.signature)))) {
	gpmon_fatalx(FLINE, e, "apr_generate_random_bytes failed");
    }
    if (ax.signature < 0) 
	ax.signature = ~ax.signature;

    /* get pgpid */
    ax.pgpid = getpgpid();
    if (! ax.pgpid) {
	gpmon_fatal(FLINE, "cannot read postmaster.pid");
    }

    /* get hostlist */
    gethostlist(&ax.hosttab, &ax.hosttabsz);

    /* create the agg */
    if (0 != (e = agg_create(&ax.agg, 1, ax.pool))) {
	gpmon_fatalx(FLINE, e, "agg_create failed");
    }

    /* spawn conm thread */
    if (0 != (e = apr_thread_create(&tp, ta, conm_main, 0, ax.pool))) {
	gpmon_fatalx(FLINE, e, "apr_thread_create failed");
    }

    /* spawn event thread */
    if (0 != (e = apr_thread_create(&tp, ta, event_main, 0, ax.pool))) {
	gpmon_fatalx(FLINE, e, "apr_thread_create failed");
    }

    /* main loop */
    while (!ax.restart) {
	apr_sleep(apr_time_from_sec(1));
	ax.gpmon_tick++;

	/* check health of DB */
	if (1 == (ax.gpmon_tick % 4)) {
	    if (0 != (e = pgcheck(ax.pgpid))) {
		exit(3);
	    }
	}
	/* get all hosts to dump their metrics */
	if (1 == (ax.gpmon_tick % opt.q)) {
	    int i;
	    for (i = 0; i < ax.hosttabsz; i++) {
		host_t* h = &ax.hosttab[i];
		gpmon_spin_t* spin = gpmon_spin_lock(&h->spin);
		/* only send to entries with a socket, handling events, and no error */
		TR1(("send dump %d eflag %d\n", h->sock, h->eflag));
		if (h->sock && h->event && !h->eflag) {
		    if (1 != send(h->sock, "D", 1, 0)) {
			h->eflag = 1;
			TR1(("at %s: cannot send 'D'ump command [set eflag]\n", FLINE));
		    }
		}
		spin = gpmon_spin_unlock(spin);
	    }
	}
	/* aggregate after collection */
	if (5 == (ax.gpmon_tick % opt.q)) {
	    agg_t* newagg = 0;
	    agg_t* oldagg = 0;

	    /* dump the current aggregates */
	    if (0 != (e = agg_dump(ax.agg))) {
		gpmon_warningx(FLINE, e, "unable to finish aggregation");
	    }

	    /* make a new one, copy only recently updated entries */
	    gpmon_spin_lock(&ax.agg_spin);
	    if (0 != (e = agg_dup(&newagg, ax.agg, ax.pool)))
		gpmon_fatalx(FLINE, e, "agg_dup failed");
	    oldagg = ax.agg;
	    ax.agg = newagg;
	    gpmon_spin_unlock(&ax.agg_spin);
	    
	    /* destroy the old agg */
	    agg_destroy(oldagg);
	}

	/* every N quantums, run the harvest queries */
	if (1 == (ax.gpmon_tick % (opt.q * 3))) {
	    gpdb_harvest();
	}

	if (31 == (ax.gpmon_tick % 32)) {
	    TR2(("checking for configuration changes...\n"));
	    int confchanges = getconfig();
	    int hostchanges = checkhostlist();
	    if (confchanges || hostchanges) 
		ax.restart = 1;
	}
    }
}


static void print_version(void)
{
    printf("%s version '%s'\n", opt.pname, GP_VERSIONX);
    exit(0);
}


static void parse_command_line(int argc, const char* const argv[])
{
    apr_getopt_t* os;
    int ch;
    const char* arg;
    int e;
    static apr_getopt_option_t option[] = {
	{      NULL, '?', 0, "print help screen" },
	{      NULL, 'V', 0, "very verbose" },
	{      NULL, 'v', 0, "slightly verbose" },
	{      NULL, 'q', 1, "# seconds between updates" },
	{      NULL, 'm', 1, "minimum query time to log" },
        {      NULL, 'd', 1, "minimum query time to log details" },
	{ "version", 256, 0, "print version number" }
    };
    apr_pool_t* pool;

    if (0 != (e = apr_pool_create(&pool, 0))) {
	gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }

    opt.pname = argv[0];

    if (0 != (e = apr_getopt_init(&os, pool, argc, argv))) {
	gpmon_fatalx(FLINE, e, "apr_getopt_init failed");
    }

    opt.q = quantum;
    opt.m = min_query_time;
    opt.d = min_detailed_query_time;

    while (0 == (e = apr_getopt_long(os, option, &ch, &arg))) {
        switch (ch) {
        case '?': usage(0); break;
        case 'v': opt.v = 1; break;
        case 'V': opt.v = opt.V = 1; break;
        case 'q': opt.q = atoi(arg); break;
        case 'm': opt.m = atoi(arg); break;
        case 'd': opt.d = atoi(arg); break;
        case 256: print_version(); break;
        }
    }
    
    if (e != APR_EOF)
        usage("Error: illegal arguments");
    
    if (! (10 <= opt.q && opt.q <= 120)) 
        usage("Error: -q quantum value must be between 10 and 120");

    /* check for valid entries */
    if (opt.m < 0)
        opt.m = 0;

    if (opt.d < 15)
        usage("Error: -d mindetail value must be greater than 15");

    /* See if opt.m changed, but opt.d did not */
    if (opt.m != min_query_time &&
        opt.d == min_detailed_query_time )
    {
        opt.d = opt.m;
    }

    if (opt.d < opt.m)
    {
        opt.d = opt.m;
        printf("Minimum query detail cannot be less than minimum query time");
        printf("Setting minimum query detail equal to minimum query time");
    }

    apr_pool_destroy(pool);

    verbose = opt.v;
    very_verbose = opt.V;
    min_query_time = opt.m;
    min_detailed_query_time = opt.d;
    quantum = opt.q;
}


int main(int argc, const char* const argv[]) 
{
    /* save argc, argv and startup dir so we can restart */
    opt.argc = argc;
    opt.argv = argv;
    getcwd(ax.startup_directory, MAXPATHLEN);

    if (apr_initialize()) {
	fprintf(stderr, "internal error: cannot apr_initialize\n");
	exit(1);
    }

    parse_command_line(argc, argv);
    getconfig();
    /* check port */
    if (! (0 < ax.port && ax.port < (1 << 16))) {
	usage("Error: invalid port number");
    }

    /* check quantum */
    if (! (10 <= opt.q && opt.q <= 120)) {
	usage("Error: -q quantum value should be in range [10..120].\n");
    }

    /* check master data directory */
    if (-1 == chdir(ax.master_data_directory)) {
	usage("Error: invalid master_data_directory");
    }

    /* check that we are indeed running in a postgres data directory */
    {
	FILE* fp = fopen("pg_hba.conf", "r");
	if (!fp) {
	    usage("Error: master data directory is not valid; can't find pg_hba.conf");
	}
	SET_MAXFD(fileno(fp));
	fclose(fp);
    }

    /* start up gpperfmon directory */
    {
        char work_dir[PATH_MAX] = {0};
        char *pdir = work_dir;

        strncpy(work_dir, GPMON_DIR, PATH_MAX - 1);
        work_dir[PATH_MAX - 1] = 0;

        while (*pdir)
        {
            if (*pdir == '/')
            {
                *pdir = 0;
                if (-1 == mkdir(work_dir, 0700) && EEXIST != errno)
                {
                    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
                                 "cannot create directory %s/%s", ax.master_data_directory, GPMON_DIR);                    
                }
                *pdir = '/';
            }
            pdir++;
        }
    }

    gpmmon_main();

    /* if we get here, we should re-exec self */
    {
	int i, j;

	/* chdir could fail, but maybe gpmmon will be in PATH */
	chdir(ax.startup_directory);

	for (j = 0; j < 20; j++) {
	    for (i = 3; i <= opt.max_fd; i++)
		close(i);
	    execv(argv[0], (char**) argv);
	    apr_sleep(apr_time_from_sec(1));
	}
    }

    /* if we get here, execv has failed */
    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno),
		 "cannot exec");
    return -1;
}

/* check that postgres is still running */
static int pgcheck(int pid)
{
    char line[1024];
    FILE* fp;
    int rc;
    char* ret;

    sprintf(line, "ps -p %d", pid);
    fp = popen(line, "r");
    if (!fp) 
	return 0;

    SET_MAXFD(fileno(fp));
    ret = fgets(line, sizeof(line), fp);
    line[sizeof(line)-1] = 0;
    rc = pclose(fp);
    if (!rc)
	return 0;

    return 0 != strstr(line, "postgres");
}


/* read the post master pid */
static int getpgpid(void)
{
    int pid = 0;
    FILE* fp = fopen("postmaster.pid", "r");
    if (fp) {
	SET_MAXFD(fileno(fp));
	fscanf(fp, "%d", &pid);
	fclose(fp);
    }
    return pid;
}

static int checkhostlist(void)
{
    static unsigned char lastmd5[APR_MD5_DIGESTSIZE];
    unsigned char currentmd5[APR_MD5_DIGESTSIZE];
    static int first = 1;

    /* Connect to database, get segment hosts from gp_configuration */
    gpdb_get_config_md5(currentmd5);

    if (first)
    {
        first = 0;
        memcpy(lastmd5, currentmd5, APR_MD5_DIGESTSIZE);
    }

    return (memcmp(currentmd5, lastmd5, APR_MD5_DIGESTSIZE) == 0 ? 0 : 1);
}

static void gethostlist(host_t** rettab, int* rettabsz)
{
    apr_pool_t* pool;
    apr_hash_t* htab;
    int e, i;
    char line[1024];
    apr_array_header_t* hostarray;
    int host_count = 0;
    char **hosts = NULL;

    if (0 != (e = apr_pool_create(&pool, NULL))) {
        gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }

    hostarray = apr_array_make(pool, 4, sizeof(host_t));
    CHECKMEM(hostarray);
    
    htab = apr_hash_make(pool);
    CHECKMEM(htab);
    
    /* Connect to database, get segment hosts from gp_configuration */
    gpdb_get_hostlist(&host_count, &hosts, ax.pool);

    for (i = 0; i < host_count; i++)
    {
        host_t *host = apr_array_push(hostarray);
        CHECKMEM(host);
        memset(host, 0, sizeof(*host));
        host->hostname = hosts[i];
    }


    if (apr_is_empty_array(hostarray)) {
        gpmon_fatal(FLINE, "No valid hosts in gp_configuration.");
    }

    /* popen ssh hostname, put host_t entries into htab to 
       eliminate duplicate */
    for (;;) {
        host_t host[BATCH];
        FILE* fp[BATCH];
        int i;
        host_t* rec;
       	
	memset(fp, 0, sizeof(fp)); 
        memset(host, 0, sizeof(host));
        for (i = 0; i < BATCH; i++) {
            rec = apr_array_pop(hostarray);
            if (! rec) break;
            memcpy(&host[i], rec, sizeof(*rec));
            sprintf(line, "ssh -o 'BatchMode yes' -o "
                    "'StrictHostKeyChecking no' %s hostname",
                    host[i].hostname);
            fp[i] = popen(line, "r");
            if (! fp[i]) {
		SET_MAXFD(fileno(fp[i]));
                gpmon_warning(FLINE, "cannot ssh to %s ... skipped\n", 
                              host[i].hostname);
                continue;
            }
        }

        
        for (i = 0; i < BATCH && host[i].hostname; i++) {
            if (! fp[i]) continue;
            line[sizeof(line)-1] = 0;
            if (! fgets(line, sizeof(line), fp[i])) {
                gpmon_warning(FLINE, "cannot read hostname of %s", 
                              host[i].hostname);
                continue;
            }
            if (line[sizeof(line)-1]) {
                line[sizeof(line)-1] = 0;
                gpmon_warning(FLINE, 
                              "hostname of %s is too long\n\t%s ...",
                              host[i].hostname, line);
                continue;
            }
            
            host[i].realhostname = gpmon_trim(line);
            rec = apr_hash_get(htab, host[i].realhostname, 
                               APR_HASH_KEY_STRING);
            if (rec) {
                /* FIXME:throw a coin to determine which hostname to use */
                /* for now, just use the last entry that got there */
                rec->hostname = host[i].hostname;
            }
            else {
                /* insert a new entry */
                rec = apr_palloc(pool, sizeof(*rec));
                CHECKMEM(rec);
                memcpy(rec, &host[i], sizeof(*rec));
                rec->realhostname = apr_pstrdup(pool, rec->realhostname);
                CHECKMEM(rec->realhostname);
                apr_hash_set(htab, rec->realhostname, 
                             APR_HASH_KEY_STRING, rec);
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
             hi; hi = apr_hash_next(hi), i++) {
            void *val;
            struct hostent *hp;
            
            apr_hash_this(hi, 0, 0, &val);
            memcpy(&ret[i], val, sizeof(ret[i]));
            ret[i].hostname = strdup(ret[i].hostname);
            CHECKMEM(ret[i].hostname);
            ret[i].realhostname = strdup(ret[i].realhostname);
            CHECKMEM(ret[i].realhostname);
            hp = gethostbyname(ret[i].hostname);
            if (! hp) {
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
    while (p < q) {
	int n = send(sock, p, q - p, 0);
	if (n == -1) {
	    switch (errno) {
	    case EINTR: case EAGAIN: continue;
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
    while (p < q) {
	int n = recv(sock, p, q - p, 0);
	if (n == -1) {
	    switch (errno) {
	    case EINTR: case EAGAIN: 
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

static int getconfig(void)
{
    char *hostname = NULL;
    char *master_data_directory = NULL;
    int changed = 0, rc = 0;
    /* Don't want to create/destroy this pool every 30 seconds */
    static apr_pool_t *pool = NULL;

    int port = gpdb_get_gpmon_port();
    if (ax.port != port) 
    {
        ax.port = port;
        changed = 1;
    }

    /* fetch datadir */
    if (pool == NULL)
    {
        if (APR_SUCCESS != (rc = apr_pool_create(&pool, NULL)))
            gpmon_fatalx(FLINE, rc, "Failed to create APR pool\n");
    }

    gpdb_get_master_data_dir(&hostname, &master_data_directory, pool);
    
    if (ax.master_data_directory == NULL)
    {
        ax.master_data_directory = strdup(master_data_directory);
        CHECKMEM(ax.master_data_directory);
    }
    else
    {
        if (apr_strnatcmp(ax.master_data_directory, 
                          master_data_directory) != 0)
        {
            /* for now a change to master data directory
               means a restart of gpmmon so we just need
               to notify the things have chagned.  If this
               changes then we'll need to store off the change */
            changed = 1;
        }
    }

    /* clear pool for next call */
    apr_pool_clear(pool);

    return changed;
}
