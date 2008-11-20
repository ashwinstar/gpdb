#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <math.h>
#include "apr_getopt.h"
#include "apr_env.h"
#include "apr_time.h"
#include "apr_hash.h"
#include "apr_strings.h"
#include "apr_pools.h"
#include "gpmonlib.h"
#include "gpmon/gpmon.h"
#include "apr_thread_proc.h"
#include "event.h"
#include "sigar.h"


#define FMT64 APR_INT64_T_FMT

static struct {
    const char* pname;
    int v;
    int V;
    int D;
    const char* arg_port;
} opt = {0};


int verbose = 0;		/* == opt.v */
int very_verbose = 0;		/* == opt.V */

typedef struct pidrec_t pidrec_t;
struct pidrec_t {
    apr_int64_t updated_tick;		/* when this pidrec was updated */
    apr_int32_t pid;
    char* pname;
    char* cwd;
    gpmon_proc_metrics_t p_metrics;
    apr_int64_t cpu_elapsed;
};


typedef struct gx_t gx_t;
struct gx_t {
    int port;
    apr_int64_t signature;
    apr_int64_t tick;
    apr_time_t  now;

    sigar_t* sigar;
    const char** fslist;
    const char** devlist;
    const char** netlist;

    SOCKET listen_sock;
    struct event listen_event;

    SOCKET tcp_sock;
    struct event tcp_event;

    SOCKET udp_sock;
    struct event udp_event;

    apr_pool_t* pool;
    int         qd_pid;
    const char* hostname; 	/* my hostname */

    /* hash tables */
    apr_hash_t* qexectab;	/* stores qexec packets */
    apr_hash_t* qlogtab;	/* stores qlog packets */
    apr_hash_t* pidtab;		/* key=pid, value=pidrec_t */
};
static struct gx_t gx = {0};

static void gx_accept(SOCKET sock, short event, void* arg);
static void gx_recvfrom(SOCKET sock, short event, void* arg);
static gpmon_packet_t* gx_pktdup(apr_pool_t* pool, const gpmon_packet_t* pkt)
{
    gpmon_packet_t* t = apr_palloc(pool, sizeof(*t));
    CHECKMEM(t);
    memcpy(t, pkt, sizeof(*t));
    return t;
}


static void gx_exit(const char* reason)
{
    TR0(("exit %s\n", reason ? reason : "1"));
    exit(reason ? 1 : 0);
}



static void send_fully(SOCKET sock, const void* p_, int len)
{
    const char* p = p_;
    const char* q = p + len;
    while (p < q) {
	int n = send(sock, p, q - p, 0);
	if (n == -1) {
	    switch (errno) {
	    case EINTR: case EAGAIN: continue;
	    }
	    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "send failed");
	}
	p += n;
    }
}

static void get_pid_metrics(apr_int32_t pid)
{
    sigar_proc_cpu_t cpu;
    sigar_proc_mem_t mem;
    sigar_proc_fd_t fd;
    pidrec_t  newrec;
    pidrec_t* rec;
    apr_pool_t* pool = apr_hash_pool_get(gx.pidtab);

    rec = apr_hash_get(gx.pidtab, &pid, sizeof(pid));
    if (rec && rec->updated_tick == gx.tick)
	return;			/* updated in current cycle */

    TR2(("--------------------- stating %d\n", pid));
    memset(&cpu, 0, sizeof(cpu));
    memset(&mem, 0, sizeof(mem));
    memset(&fd, 0, sizeof(fd));
    sigar_proc_mem_get(gx.sigar, pid, &mem);
    sigar_proc_cpu_get(gx.sigar, pid, &cpu);
    sigar_proc_fd_get(gx.sigar, pid, &fd);

    if (rec) {
	newrec.pname = rec->pname;
	newrec.cwd = rec->cwd;
    }
    else {
	sigar_proc_exe_t exe;
	newrec.pname = newrec.cwd = 0;
	if (0 == sigar_proc_exe_get(gx.sigar, pid, &exe)) {
	    newrec.pname = apr_pstrdup(pool, exe.name);
	    newrec.cwd = apr_pstrdup(pool, exe.root);
	    /*
	    TR1(("pname = %s\nroot = %s\ncwd = %s\n", 
		exe.name, exe.root, exe.cwd));
	    */
	}
	if (! newrec.pname)
	    newrec.pname = "unknown";
	if (! newrec.cwd)
	    newrec.cwd = "unknown";
    }
    newrec.pid = pid;
    newrec.updated_tick = gx.tick;
    newrec.p_metrics.fd_cnt = (apr_int32_t) fd.total;
    newrec.p_metrics.cpu_pct = (float) (cpu.percent * 100);
    newrec.p_metrics.mem.size = mem.size;
    newrec.p_metrics.mem.resident = mem.resident;
    newrec.p_metrics.mem.share = mem.share;
    newrec.cpu_elapsed = cpu.total;

    if (rec)
	*rec = newrec;
    else {
	rec = apr_palloc(pool, sizeof(*rec));
	CHECKMEM(rec);
	*rec = newrec;
	apr_hash_set(gx.pidtab, &rec->pid, sizeof(rec->pid), rec);
    }
}


static void send_machine_metrics(SOCKET sock)
{
    sigar_mem_t mem;
    sigar_swap_t swap;
    sigar_cpu_t cpu;
    sigar_loadavg_t loadavg;
    sigar_file_system_usage_t fsusage;
    sigar_disk_usage_t tdisk;
    sigar_net_interface_stat_t tnet;
    static int first = 1;
    static sigar_disk_usage_t pdisk = {0};
    static sigar_net_interface_stat_t pnet = {0};
    static sigar_cpu_t pcpu = {0};
    static sigar_swap_t pswap = {0};
    gpmon_packet_t pkt;
    const char** fsdir;
    const char** netname;
    sigar_net_interface_stat_t netstat;
    int cpu_total_diff;

    float cpu_norm = 0.0f;

    memset(&mem, 0, sizeof(mem));
    sigar_mem_get(gx.sigar, &mem);
    TR2(("mem ram: %" FMT64 " total: %" FMT64 
	 " used: %" FMT64 " free: %" FMT64 "\n",
	 mem.ram, mem.total, mem.used, mem.free));

    memset(&swap, 0, sizeof(swap));
    sigar_swap_get(gx.sigar, &swap);
    TR2(("swap total: %" FMT64 " used: %" FMT64 "page_in: %" FMT64 " page_out: %" FMT64 "\n",
	 swap.total, swap.used, swap.page_in, swap.page_out));

    memset(&cpu, 0, sizeof(cpu));
    sigar_cpu_get(gx.sigar, &cpu);
    TR2(("cpu user: %" FMT64 " sys: %" FMT64 " idle: %" FMT64 " wait: %" FMT64 
	 " total: %" FMT64 "\n",
	 cpu.user, cpu.sys, cpu.idle, cpu.wait, cpu.total));

    memset(&loadavg, 0, sizeof(loadavg));
    sigar_loadavg_get(gx.sigar, &loadavg);
    TR2(("load_avg: %e %e %e\n", loadavg.loadavg[0], loadavg.loadavg[1], loadavg.loadavg[2]));

    memset(&tdisk, 0, sizeof(tdisk));
    for (fsdir = gx.fslist; *fsdir; fsdir++) {
	int e = sigar_file_system_usage_get(gx.sigar, *fsdir, &fsusage);
	TR2(("get fs: %s, e: %d\n", *fsdir, e));
	if (0 == e) {
	    tdisk.reads += fsusage.disk.reads;
	    tdisk.writes += fsusage.disk.writes;
	    tdisk.write_bytes += fsusage.disk.write_bytes;
	    tdisk.read_bytes += fsusage.disk.read_bytes;
	}
    }
    TR2(("disk reads: %" FMT64 " writes: %" FMT64
	 " rbytes: %" FMT64 " wbytes: %" FMT64 "\n",
	 tdisk.reads, tdisk.writes, tdisk.read_bytes, tdisk.write_bytes));

    memset(&tnet, 0, sizeof(tnet));
    for (netname = gx.netlist; *netname; netname++) {
	int e = sigar_net_interface_stat_get(gx.sigar, *netname, &netstat);
	if (0 == e) {
	    tnet.rx_packets += netstat.rx_packets;
	    tnet.rx_bytes += netstat.rx_bytes;
	    tnet.tx_packets += netstat.tx_packets;
	    tnet.tx_bytes += netstat.tx_bytes;
	}
    }
    TR2(("rx: %" FMT64 " rx_bytes: %" FMT64 "\n", 
	 tnet.rx_packets, tnet.rx_bytes));
    TR2(("tx: %" FMT64 " tx_bytes: %" FMT64 "\n",
	 tnet.tx_packets, tnet.tx_bytes));

    if (first) {
	pswap = swap, pcpu = cpu, pdisk = tdisk, pnet = tnet;
    }
    first = 0;

    pkt.magic = GPMON_MAGIC;
    pkt.version = GPMON_PACKET_VERSION;
    pkt.pkttype = GPMON_PKTTYPE_METRICS;
    pkt.u.metrics.mem.total = mem.total;
    pkt.u.metrics.mem.used = mem.used;
    pkt.u.metrics.mem.actual_used = mem.actual_used;
    pkt.u.metrics.mem.actual_free = mem.actual_free;
    pkt.u.metrics.swap.total = swap.total;
    pkt.u.metrics.swap.used = swap.used;
    pkt.u.metrics.swap.page_in = swap.page_in - pswap.page_in;
    pkt.u.metrics.swap.page_out = swap.page_out - pswap.page_out;
    cpu_total_diff = cpu.total - pcpu.total;
    if (cpu_total_diff) {
        cpu_norm = round((((float) (cpu.user - pcpu.user) * 100 / cpu_total_diff) +
                          ((float) (cpu.sys - pcpu.sys) * 100 / cpu_total_diff) + 
                          ((float) (cpu.idle - pcpu.idle) * 100 / cpu_total_diff)) / 100.0f);

        pkt.u.metrics.cpu.user_pct = ((float) (cpu.user - pcpu.user) * 100 / cpu_total_diff) / cpu_norm;
        pkt.u.metrics.cpu.sys_pct = ((float) (cpu.sys - pcpu.sys) * 100 / cpu_total_diff) / cpu_norm;
        pkt.u.metrics.cpu.idle_pct = ((float) (cpu.idle - pcpu.idle) * 100 / cpu_total_diff) / cpu_norm;
    }
    else {
	pkt.u.metrics.cpu.user_pct = 0;
	pkt.u.metrics.cpu.sys_pct = 0;
	pkt.u.metrics.cpu.idle_pct = 0;
    }
    pkt.u.metrics.load_avg.value[0] = (float) loadavg.loadavg[0];
    pkt.u.metrics.load_avg.value[1] = (float) loadavg.loadavg[1];
    pkt.u.metrics.load_avg.value[2] = (float) loadavg.loadavg[2];
    pkt.u.metrics.disk.rcount = tdisk.reads - pdisk.reads;
    pkt.u.metrics.disk.wcount = tdisk.writes - pdisk.writes;
    pkt.u.metrics.disk.rbytes = tdisk.read_bytes - pdisk.read_bytes;
    pkt.u.metrics.disk.wbytes = tdisk.write_bytes - pdisk.write_bytes;
    pkt.u.metrics.net.rcount = tnet.rx_packets - pnet.rx_packets;
    pkt.u.metrics.net.wcount = tnet.tx_packets - pnet.tx_packets;
    pkt.u.metrics.net.rbytes = tnet.rx_bytes - pnet.rx_bytes;
    pkt.u.metrics.net.wbytes = tnet.tx_bytes - pnet.tx_bytes;
	
    strncpy(pkt.u.metrics.hname, gx.hostname, sizeof(pkt.u.metrics.hname) - 1);
    pkt.u.metrics.hname[sizeof(pkt.u.metrics.hname) - 1] = 0;
    send_fully(sock, &pkt, sizeof(pkt));

    /* save for next time around */
    pswap = swap, pcpu = cpu, pdisk = tdisk, pnet = tnet;
}


static void gx_gettcpcmd(SOCKET sock, short event, void* arg)
{
    char dump;
    int n, e;
    apr_hash_t* qetab;
    apr_hash_t* qdtab;
    apr_hash_t* pidtab;

    n = recv(sock, &dump, 1, 0);
    if (n == 0)
	gx_exit("peer closed");

    if (n == -1)
	gx_exit("socket error");

    if (dump != 'D')
	gx_exit("bad data");

    TR1(("start dump %c\n", dump));

    qetab = gx.qexectab;
    qdtab = gx.qlogtab;
    pidtab = gx.pidtab;

    /* make a new qexec/qlog/pidtab hashtab for next cycle */
    {
	apr_pool_t* newpool;
	if (0 != (e = apr_pool_create(&newpool, gx.pool))) {
	    gpmon_fatalx(FLINE, e, "apr_pool_create failed");
	}
	/* qexec hash table */
	gx.qexectab = apr_hash_make(newpool);
	CHECKMEM(gx.qexectab);

	/* qlog hash table */
	gx.qlogtab = apr_hash_make(newpool);
	CHECKMEM(gx.qlogtab);

	/* pidtab hash table */
	gx.pidtab = apr_hash_make(newpool);
	CHECKMEM(gx.pidtab);
    }

    /* push out a metric of the machine */
    send_machine_metrics(sock);

    /* push out records in qe/qd */
    {
	void* vptr;
	apr_hash_index_t* hi;
	gpmon_packet_t* ppkt;
	pidrec_t* pidrec;
	int count = 0;

	for (hi = apr_hash_first(0, qdtab); hi; hi = apr_hash_next(hi)) {
	    apr_hash_this(hi, 0, 0, &vptr);
	    ppkt = vptr;
	    if (ppkt->pkttype != GPMON_PKTTYPE_QLOG)
		continue;
	    TR2(("sending magic %x, pkttype %d\n", ppkt->magic, ppkt->pkttype));
	    send_fully(sock, ppkt, sizeof(*ppkt));
	    count++;
	}

	for (hi = apr_hash_first(0, qetab); hi; hi = apr_hash_next(hi)) {
	    apr_hash_this(hi, 0, 0, &vptr);
	    ppkt = vptr;
	    if (ppkt->pkttype != GPMON_PKTTYPE_QEXEC)
		continue;
	    /* fill in _hname */
	    strncpy(ppkt->u.qexec._hname, gx.hostname, 
		    sizeof(ppkt->u.qexec._hname) - 1);
	    ppkt->u.qexec._hname[sizeof(ppkt->u.qexec._hname) - 1] = 0;
	    
	    /* fill in _p_metrics */
	    pidrec = apr_hash_get(pidtab, &ppkt->u.qexec.key.pid, 
				  sizeof(ppkt->u.qexec.key.pid));
	    if (pidrec) {
		ppkt->u.qexec._p_metrics = pidrec->p_metrics;
                ppkt->u.qexec._cpu_elapsed = pidrec->cpu_elapsed;
	    }
	    else {
		memset(&ppkt->u.qexec._p_metrics, 0, 
		       sizeof(ppkt->u.qexec._p_metrics));
	    }
	    TR2(("sending magic %x, pkttype %d\n", ppkt->magic, ppkt->pkttype));
	    send_fully(sock, ppkt, sizeof(*ppkt));
	    count++;
	}

	TR1(("end dump ... sent %d entries\n", count));
    }

    /* get rid of the old pool */
    {
	apr_pool_t* oldpool = apr_hash_pool_get(qetab);
	apr_pool_destroy(oldpool);
    }
    
    return;
}


static void gx_accept(SOCKET sock, short event, void* arg)
{
    SOCKET nsock;
    gpmon_packet_t pkt;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    char* p; 
    char* q;

    if (0 == (event & EV_READ))
	return;

    if (-1 == (nsock = accept(sock, (void*) &a, &alen))) {
	gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), "accept failed");
	return;
    }

    TR1(("accepted\n"));

    /* we do this one at a time */
    if (gx.tcp_sock) {
	gpmon_warning(FLINE, "cannot accept new connection before old one dies");
	close(nsock);
	return;
    }

    p = (char*) &pkt;
    q = p + sizeof(pkt);
    while (p < q) {
	int n = recv(nsock, p, q - p, 0);
	if (n == -1) {
	    gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), "recv failed");
	    close(nsock);
	    return;
	}
	p += n;
    }

    if (0 != gpmon_ntohpkt(&pkt)) {
	gpmon_warning(FLINE, "bad packet");
	close(nsock);
	return;
    }

    if (pkt.pkttype != GPMON_PKTTYPE_HELLO) {
	gpmon_warning(FLINE, "bad packet");
	close(nsock);
	return;
    }
    
    if (pkt.u.hello.signature != gx.signature) {
	gx_exit("bad signature... maybe a new gpmmon has started");
    }

    /* echo the hello */
    TR2(("accepted pkt.magic = %x\n", (int) pkt.magic));
    p = (char*) &pkt;
    q = p + sizeof(pkt);
    while (p < q) {
	int n = send(nsock, p, q - p, 0);
	if (n == -1) {
	    gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), 
			   "send failed");
	    close(nsock);
	    return;
	}
	p += n;
    }

    event_set(&gx.tcp_event, nsock, EV_READ | EV_PERSIST, gx_gettcpcmd, 0);
    if (event_add(&gx.tcp_event, 0)) {
	gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), "event_add failed");
	close(nsock);
	return;
    }
    gx.tcp_sock = nsock;
    TR1(("connection established --------------------- \n"));
}



/* got a packet from peer. put it in the queue */
static void gx_recvqlog(gpmon_packet_t* pkt)
{
    gpmon_qlog_t* p;
    gpmon_packet_t* rec;

    if (pkt->pkttype != GPMON_PKTTYPE_QLOG)
	gpmon_fatal(FLINE, "assert failed; expected pkttype qlog");
    
    p = &pkt->u.qlog;
    TR2(("Received qlog packet for query %d-%d-%d.  Status now %d\n",
         p->key.tmid, p->key.ssid, p->key.ccnt, p->status));
    rec = apr_hash_get(gx.qlogtab, &p->key, sizeof(p->key));
    if (rec) {
	memcpy(rec, pkt, sizeof(*pkt));
    } 
    else {
	rec = gx_pktdup(apr_hash_pool_get(gx.qlogtab), pkt);
	apr_hash_set(gx.qlogtab, &rec->u.qlog.key, sizeof(rec->u.qlog.key), rec);
    }
}


static void gx_recvqexec(gpmon_packet_t* pkt)
{
    gpmon_qexec_t* p;
    gpmon_packet_t* rec;

    if (pkt->pkttype != GPMON_PKTTYPE_QEXEC)
	gpmon_fatal(FLINE, "assert failed; expected pkttype qexec");

    p = &pkt->u.qexec;

    /* fill in the tstart / tduration */
    if (p->tstart == 0) {
	p->tstart = apr_time_sec(gx.now);
	p->tduration = 0;
    }
    else {
	p->tduration = apr_time_sec(gx.now) - p->tstart;
    }

    rec = apr_hash_get(gx.qexectab, &p->key, sizeof(p->key));
    if (rec) {
	/* overwrite an old record */
	memcpy(rec, pkt, sizeof(*pkt));
    }
    else {
	/* insert a new record */
	rec = gx_pktdup(apr_hash_pool_get(gx.qexectab), pkt);
	apr_hash_set(gx.qexectab, &rec->u.qexec.key, sizeof(rec->u.qexec.key), rec);
    }
}



/* callback from libevent when a udp socket is ready to be read.
   This function determines the packet type, then calls 
   gx_recvqlog() or gx_recvqexec().
*/
static void gx_recvfrom(SOCKET sock, short event, void* arg)
{
    gpmon_packet_t pkt;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int n;

    if (! (event & EV_READ))
	return;

    n = recvfrom(sock, &pkt, sizeof(pkt), 0, (void*) &addr, &addrlen);
    if (n == -1) {
	gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), "recvfrom failed");
	return;
    }

    if (n != sizeof(pkt)) {
	gpmon_warning(FLINE, "bad packet (length %d)", n);
	return;
    }

    /* do some packet marshalling */
    if (0 != gpmon_ntohpkt(&pkt)) {
	gpmon_warning(FLINE, "bad packet");
	return;
    }

    /* process the packet */
    switch (pkt.pkttype) {
    case GPMON_PKTTYPE_QLOG:
	gx_recvqlog(&pkt);
	break;
    case GPMON_PKTTYPE_QEXEC:
	gx_recvqexec(&pkt);
	break;
    default:
	gpmon_warning(FLINE, "unexpected packet type %d", pkt.pkttype);
	return;
    }
}






static void setup_tcp(void)
{
    SOCKET sock;
    struct sockaddr_in a;
    int on = 1;
    struct linger linger;

    /* create socket */
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
		     "unable to create tcp socket");
    }

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    /* bind socket to port */
    memset(&a, 0, sizeof(a));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(gx.port);
    a.sin_family = AF_INET;
    if (bind(sock, (void*) &a, sizeof(a))) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
		     "unable to bind udp socket");
    }

    if (-1 == listen(sock, 5)) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "listen failed");
    }

    /* save it */
    gx.listen_sock = sock;
    TR1(("TCP port %d opened\n", gx.port));

    /* set up listen event, and associate with our event_base */
    event_set(&gx.listen_event, sock, EV_READ | EV_PERSIST, gx_accept, 0);

    /* start watching this event */
    if (event_add(&gx.listen_event, 0)) {
	gpmon_fatal(FLINE, "event_add failed");
    }

}


static void setup_udp()
{
    SOCKET sock;
    struct sockaddr_in a;

    /* create the socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
		     "cannot create udp socket");
    }

    /* bind socket to port */
    memset(&a, 0, sizeof(a));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(gx.port);
    a.sin_family = AF_INET;
    if (bind(sock, (void*) &a, sizeof(a))) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
		     "unable to bind udp socket");
    }

    /* save it */
    gx.udp_sock = sock;

    /* set up udp event */
    event_set(&gx.udp_event, gx.udp_sock, EV_READ | EV_PERSIST, gx_recvfrom, 0);

    /* start watching this event */
    if (event_add(&gx.udp_event, 0)) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_add failed");
    }
}




static void setup_gx(int port, apr_int64_t signature)
{
    int e;
    char hname[255 + 1];
    apr_pool_t* subpool;

    /* set up pool */
    if (0 != (e = apr_pool_create(&gx.pool, 0))) {
	gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }
    
    /* set up hostname */
    if (gethostname(hname, sizeof(hname)-1)) 
	sprintf(hname, "unknown");
    hname[sizeof(hname) - 1] = 0;
    gx.hostname = apr_pstrdup(gx.pool, hname);
    CHECKMEM(gx.hostname);
    TR1(("hostname = %s\n", gx.hostname));

    /* set up port, event */
    gx.port = port;
    gx.signature = signature;
    if (!event_init()) {
	gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), "event_init failed");
    }

    if (0 != (e = apr_pool_create(&subpool, gx.pool))) {
	gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }

    /* qexec hash table */
    gx.qexectab = apr_hash_make(subpool);
    CHECKMEM(gx.qexectab);

    /* qlog hash table */
    gx.qlogtab = apr_hash_make(subpool);
    CHECKMEM(gx.qlogtab);

    /* pidtab */
    gx.pidtab = apr_hash_make(subpool);
    CHECKMEM(gx.pidtab);
}

static void setup_sigar(void)
{
    sigar_file_system_list_t sigar_fslist;
    sigar_net_interface_list_t sigar_netlist;
    int i, e, cnt;
    int do_destroy = 0;

    /* initialize sigar */
    if (0 != (e = sigar_open(&gx.sigar))) {
	gpmon_fatalx(FLINE, e, "sigar_open failed");
    }

    TR2(("sigar initialized\n"));
    do_destroy = 1;
    if (0 != sigar_net_interface_list_get(gx.sigar, &sigar_netlist)) {
	memset(&sigar_netlist, 0, sizeof(sigar_netlist));
	do_destroy = 0;
    }
    gx.netlist = apr_pcalloc(gx.pool, sizeof(const char*) * (1+sigar_netlist.number));
    CHECKMEM(gx.netlist);
    for (i = 0; i < sigar_netlist.number; i++) {
	gx.netlist[i] = apr_pstrdup(gx.pool, sigar_netlist.data[i]);
	CHECKMEM(gx.netlist[i]);
	TR2(("sigar net %d: %s\n", i, gx.netlist[i]));
    }
    if (do_destroy)
	sigar_net_interface_list_destroy(gx.sigar, &sigar_netlist);

    do_destroy = 1;
    if (0 != sigar_file_system_list_get(gx.sigar, &sigar_fslist)) {
	memset(&sigar_fslist, 0, sizeof(sigar_fslist));
	do_destroy = 0;
    }
    cnt = 0;
    TR2(("sigar fsnumber: %d\n", sigar_fslist.number));
    for (i = 0; i < sigar_fslist.number; i++) {
	if (sigar_fslist.data[i].type == SIGAR_FSTYPE_LOCAL_DISK) {
	    TR2(("sigar cnt: %d\n", cnt+1));
	    cnt++;
	}
    }
    gx.fslist = apr_pcalloc(gx.pool, sizeof(const char*) * (cnt+1));
    CHECKMEM(gx.fslist);
    gx.devlist = apr_pcalloc(gx.pool, sizeof(const char*) * (cnt+1));
    CHECKMEM(gx.devlist);
    cnt = 0;
    for (i = 0; i < sigar_fslist.number; i++) {
	if (sigar_fslist.data[i].type == SIGAR_FSTYPE_LOCAL_DISK) {
	    gx.fslist[cnt] = apr_pstrdup(gx.pool, sigar_fslist.data[i].dir_name);
	    CHECKMEM(gx.fslist[cnt]);
	    TR2(("fs: %s\n", gx.fslist[cnt]));
	    gx.devlist[cnt] = apr_pstrdup(gx.pool, sigar_fslist.data[i].dev_name);
	    CHECKMEM(gx.devlist[cnt]);
	    cnt++;
	}
    }
    if (do_destroy) 
	sigar_file_system_list_destroy(gx.sigar, &sigar_fslist);
}


void gx_main(int port, apr_int64_t signature)
{
    char* user = 0;

    /* set up our log files */
    if (0 != apr_env_get(&user, "USER", gx.pool)) 
	if (0 != apr_env_get(&user, "LOGIN", gx.pool))
	    user = 0;

    if (0 == chdir("/tmp")) {
	if (user) {
	    mkdir(user, 01700);
	    chdir(user);
	}
    }
    freopen("gpsmon.out", "a", stderr);
    freopen("gpsmon.out", "a", stdout);
    setlinebuf(stderr);
    setlinebuf(stdout);

    setup_gx(port, signature);
    setup_sigar();
    setup_udp();
    setup_tcp();

    gx.tick = 0;
    for (;;) {
	struct timeval tv;
	apr_hash_index_t* hi;

	/* serve events every 2 second */
	gx.tick++;
	gx.now = apr_time_now();
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (-1 == event_loopexit(&tv)) {
	    gpmon_warningx(FLINE, APR_FROM_OS_ERROR(errno), 
			   "event_loopexit failed");
	}
	if (-1 == event_dispatch()) {
	    gpmon_fatalx(FLINE, APR_FROM_OS_ERROR(errno), 
			 "event_dispatch failed");
	}

	/* get pid metrics */
	for (hi = apr_hash_first(0, gx.qexectab); hi; hi = apr_hash_next(hi)) {
	    void* vptr;
	    gpmon_packet_t* rec;
	    apr_hash_this(hi, 0, 0, &vptr);
	    rec = vptr;
	    if (rec->pkttype != GPMON_PKTTYPE_QEXEC)
		continue;
	    get_pid_metrics(rec->u.qexec.key.pid);
	}
    }
}



static void usage(const char* msg)
{
    fprintf(stderr, "\nusage: %s [options] port\n\n", opt.pname);
    fprintf(stderr, "\toptions:\n");
    fprintf(stderr, "\t-?        : print this help screen\n");
    fprintf(stderr, "\t-v        : verbose\n");
    fprintf(stderr, "\t-V        : very verbose\n");
    fprintf(stderr, "\t-D        : run in debug mode; don't run as daemon\n");
    fprintf(stderr, "\t--version : print version number\n\n");
    if (msg)
	fprintf(stderr, "%s\n\n", msg);
    
    exit(msg ? 1 : 0);
}

static void print_version(void)
{
    printf("%s version '%s'\n", opt.pname, GPPERFMON_VERSIONX);
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
	{      NULL, 'v', 0, "verbose" },
	{      NULL, 'V', 0, "very verbose" },
	{      NULL, 'D', 0, "debug mode" },
	{ "version", 256, 0, "print version number" },
	{ NULL, 0, 0, NULL}
    };
    apr_pool_t* pool;

    if (0 != (e = apr_pool_create(&pool, 0))) {
	gpmon_fatalx(FLINE, e, "apr_pool_create failed");
    }

    opt.pname = argv[0];
    opt.v = opt.V = opt.D = 0;

    if (0 != (e = apr_getopt_init(&os, pool, argc, argv))) {
	gpmon_fatalx(FLINE, e, "apr_getopt_init failed");
    }

    while (0 == (e = apr_getopt_long(os, option, &ch, &arg))) {
	switch (ch) {
	case '?': usage(0); break;
	case 'v': opt.v = 1; break;
	case 'V': opt.v = opt.V = 1; break;
	case 'D': opt.D = 1; break;
	case 256: print_version(); break;
	}
    }

    if (e != APR_EOF)
	usage("Error: illegal arguments");

    if (os->ind >= argc) 
	usage("Error: missing port argument");
    opt.arg_port = argv[os->ind++];

    apr_pool_destroy(pool);

    verbose = opt.v;
    very_verbose = opt.V;
}


int main(int argc, const char* const argv[])
{
    int port, e;
    apr_int64_t signature;

    if (0 != (e = apr_initialize())) {
	gpmon_fatalx(FLINE, e, "apr_initialize failed");
    }

    parse_command_line(argc, argv);

    port = atoi(opt.arg_port);
    if (! (0 < port && port < (1<<16)))
	usage("Error: bad port number");

    if (1 != fscanf(stdin, "%" FMT64, &signature)) {
	gpmon_fatal(FLINE, "cannot read signature");
    }

    TR2(("\tsignature = %" FMT64 "\n", signature));
    
    if (!opt.D) {
	if (0 != (e = apr_proc_detach(1))) 
	    gpmon_fatalx(FLINE, e, "apr_proc_detach failed");
    }

    gx_main(port, signature);
    return 0;
}

