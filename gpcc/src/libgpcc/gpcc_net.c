#include <fcntl.h>
#include <netdb.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#define SHUT_RD SD_RECEIVE
#endif

#include <signal.h>
#include <gpcc_net.h>
#include <gpcc_http.h>
#include <gpcc_spin.h>
#include <apr_network_io.h>
#include <apr_thread_proc.h>
#include <apr_poll.h>
#include <apr_strings.h>
#include <gpcc_strings.h>
#include <stdlib.h>
#include <apr_sha1.h>

#include <event.h>

#ifndef WIN32
#define SOCKET int
#define closesocket(x) close(x)
#endif

static unsigned short unique_gen = 1;


typedef struct buf_t buf_t;
struct buf_t {
    char buf[1024*64];
    int bot, top;
};


typedef struct item_t item_t;
struct item_t {
    gpcc_net_conn_t conn;
    SOCKET    sock;
    struct event sock_event;
    void*           arg;			/* user supplied data ptr */
    apr_pool_t* pool;
    int is_connecting:1;
    int is_open:1;
    int is_closing:1;
    int self_origin:1;
    buf_t in, out;
};



static struct {
    apr_uint32_t net_spin[1];
    apr_pool_t* pool;
    apr_thread_t* thread;
    SOCKET listen_sock;
    struct event listen_event;
    struct event timer_event;
    struct { /* callbacks */
	gpcc_net_tick_t* tick;
	gpcc_net_opened_t* opened;
	gpcc_net_closed_t* closed;
	gpcc_net_recv_t* recv;
    } cb;
    struct {
	item_t* item[GPCC_NET_CONN_MAX];
    } tab;
} net = {{0}};

static void do_read(int fd, short event, void *arg);

static item_t* getitem(gpcc_net_conn_t conn) 
{
    const int idx = conn & 0xffff;
    if (0 <= idx && idx < GPCC_NET_CONN_MAX) {
	item_t* item = net.tab.item[idx];
	if (item->conn == conn)
	    return item;
    }
    return 0;
}


static gpcc_net_conn_t mkitem(SOCKET sock, void* arg, 
			      int is_connecting, 
			      int is_open,
			      int self_origin,
			      apr_pool_t* pool)
{
    apr_uint32_t* spin;
    int i, e;
    item_t* item;
    gpcc_net_conn_t conn;
    struct linger linger;
    int on = 1;
    int addr;

    /* set to non-blocking, and close-on-exec */
#ifdef WIN32
    {
	unsigned long nonblocking = 1;
	ioctlsocket(sock, FIONBIO, &nonblocking);
    }
#else
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
	gpcc_warning(FLINE, "fcntl(F_SETFL, O_NONBLOCK) failed");
	closesocket(sock);
	return;
    }
    if (fcntl(sock, F_SETFD, 1) == -1) {
	gpcc_warning(FLINE, "fcntl(F_SETFD) failed");
	closesocket(sock);
	return;
    }
#endif

    /* set keepalive, reuseaddr, and linger */
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    spin = gpcc_spin_secure(net.net_spin);
    for (i = 0; i < GPCC_NET_CONN_MAX && net.tab.item[i]; i++) {;}
    if (i >= GPCC_NET_CONN_MAX) {
	spin = gpcc_spin_release(spin);
	return 0;
    }
    item = net.tab.item[i] = apr_palloc(pool, sizeof(*item));
    item->conn = (((int)unique_gen++) << 16) | i;
    item->sock = sock;
    item->arg = arg;
    item->pool = pool;
    item->is_connecting = is_connecting;
    item->is_open = is_open;
    item->is_closing = 0;
    item->self_origin = self_origin;
    item->in.bot = item->in.top = 0;
    item->out.bot = item->out.top = 0;
    event_set(&item->sock_event, item->sock, EV_READ | EV_PERSIST, do_read, item);
    event_add(&item->sock_event, 0);
    conn = item->conn;
    spin = gpcc_spin_release(net.net_spin);
    return conn;
}

static void do_closed(item_t *item) 
{
    int i;
    apr_uint32_t *spin = 0;

    net.cb.closed(item->conn, item->arg);
    spin = gpcc_spin_secure(spin);
    event_del(&item->sock_event);
    for (i = 0 ; i < GPCC_NET_CONN_MAX && item != net.tab.item[i] ; i++){;}
    if (i < GPCC_NET_CONN_MAX) {
	net.tab.item[i] = 0;
    }
    apr_pool_destroy(item->pool);
    gpcc_spin_release(spin);
}

static void do_read(int fd, short event, void *arg) 
{
    item_t *item = (item_t *) arg;
    buf_t *bb = 0;
    int e = 0;
    int bbfree = 0;
    apr_uint32_t *spin = 0;

    spin = gpcc_spin_secure(net.net_spin);

    bb = &item->in;
    bbfree = sizeof(bb->buf) - bb->top; /* freespace in buf */
    
    if (bbfree < 32 && bb->bot != 0) {
	/* compact buffer */
	memmove(bb->buf, bb->buf + bb->bot, bb->top - bb->bot);
	bb->top = bb->top - bb->bot;
	bb->bot = 0;
	bbfree = sizeof(bb->buf) - bb->top;
    }

    /* read from network */
    if (bbfree > 0) {
	apr_size_t n = bbfree;
	n = recv(item->sock, bb->buf + bb->top , n, 0);
	if (n < 0) {
#ifdef WIN32
	    int e = WSAGetLastError();
	    int ok = (e == WSAEINTR || e == WSAEWOULDBLOCK);
#else
	    int e = errno;
	    int ok = (e == EINTR || e == EAGAIN);
#endif
	    if (! ok) {
		/* end request */
		item->is_closing = 1;
	    }
	}
	else if (n == 0) {
	    /* didn't read anything */
	    ;
	}
	else {
	    bb->top += n;
	    bbfree -= n;
	}
    }

    if (item->is_connecting) {
	item->is_connecting = 0;
	item->is_open = 1;
	gpcc_spin_release(spin);
	item->arg = net.cb.opened(item->conn, item->arg, 
				  item->self_origin, item->pool);
	gpcc_spin_secure(spin);
	if (item->is_closing) {
	    goto CLOSED;
	}
    }

    if (bb->bot < bb->top) {
	gpcc_spin_release(spin);
	net.cb.recv(item->conn, item->arg);
	gpcc_spin_secure(spin);
	if (item->is_closing) {
	    goto CLOSED;
	}
    }

    gpcc_spin_release(spin);
    return;


    CLOSED:

    gpcc_spin_release(spin);

    do_closed(item);
}


static apr_status_t do_write(item_t* item)
{
    buf_t* bb = &item->out;
    apr_size_t n = bb->top - bb->bot;
    int e = 0;
    
    if (n > 0) {
	e = send(item->sock, bb->buf + bb->bot, n, 0);
	bb->bot += n;

	if (e == -1) {
	    return APR_EINVAL;
	}
	else if (e != n) {
	    gpcc_warning(FLINE, "do_write: send error (%d != %d)", n, e);
	}
    }
    return 0;
}



static void do_accept(int fd, short event, void *arg)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    SOCKET sock;
    gpcc_net_conn_t conn;
    apr_pool_t* pool;
    int addr;
    int on = 1;
    struct linger linger;
    apr_uint32_t *spin;
    item_t * item;

    /* do the accept */
    if ((sock = accept(fd, (void*) &a, &len)) < 0) {
	gpcc_warning(FLINE, "accept failed");
	return;
    }

    conn = mkitem(sock, 0 /* arg */,
		  0 /*is_connecting */,
		  1 /*is_open*/,
		  0 /*self_origin*/,
		  net.pool);
    if (! conn) {
	gpcc_warning(FLINE, "do_accept: unable to accept new connection");
	return;
    }

    spin = gpcc_spin_secure(net.net_spin);
    item = getitem(conn);
    if (item) {
	gpcc_spin_release(spin);
	item->arg = net.cb.opened(item->conn, item->arg, 
				  item->self_origin, item->pool);
	gpcc_spin_secure(spin);
    }
    
    gpcc_spin_release(spin);
}

static void do_timeout(int fd, short event, void *arg)
{
    struct timeval tm;
    apr_time_t now = apr_time_now();

    net.cb.tick(now);
    /* 1 sec timeout */
    tm.tv_sec = 0;
    tm.tv_usec = 1000 * 1000;
    evtimer_add(&net.timer_event, &tm);

}

    
static void* APR_THREAD_FUNC net_run(apr_thread_t* thread, void* arg)
{
    while (net.thread == thread) {
	if (event_dispatch()) {
	    break;
	}
    }
    return 0;
}

APR_DECLARE(apr_status_t) gpcc_net_once(void) 
{
    return event_loop(EVLOOP_ONCE);

}

APR_DECLARE(apr_status_t) gpcc_net_start(int port, 
					 gpcc_net_opened_t* opened,
					 gpcc_net_closed_t* closed,
					 gpcc_net_recv_t* recv,
					 gpcc_net_tick_t* tick,
					 int threaded)
{
    int e = 0;
    apr_threadattr_t* ta;
    apr_uint32_t* spin = 0;
    struct sockaddr_in a;
    SOCKET f;
    int on = 1;
    struct linger linger;
    struct timeval tm;

    if (port <= 0) return APR_EINVAL;

    spin = gpcc_spin_secure(net.net_spin);
    if (net.pool) {
	spin = gpcc_spin_release(spin);
	return 0;
    }
    GPCC_DO(0 == apr_pool_create(&net.pool, 0));

    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    a.sin_family = AF_INET;

    f = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (f < 0) {
	goto bail;
    }

#ifndef WIN32
    if (fcntl(f, F_SETFD, 1) == -1)
	goto bail;

#endif

    setsockopt(f, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(f, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    if (bind(f, (void*) &a, sizeof(a)))
	goto bail;

    if (listen(f, 5))
	goto bail;

    net.listen_sock = f;
    net.cb.opened = opened;
    net.cb.closed = closed;
    net.cb.recv = recv;
    net.cb.tick = tick;

    if (event_priority_init(10))
	goto bail;

    event_set(&net.listen_event, net.listen_sock, EV_READ | EV_PERSIST, do_accept, 0);

    if (event_priority_set(&net.listen_event, 0))
	goto bail;

    if (event_add(&net.listen_event, 0))
	goto bail;

    /* add timer */
    evtimer_set(&net.timer_event, do_timeout, 0);

    tm.tv_sec = 0;
    tm.tv_usec = 1000 * 1000;
    evtimer_add(&net.timer_event, &tm);

    spin = gpcc_spin_release(spin);

    if (threaded) {
	GPCC_DO(0 == apr_threadattr_create(&ta, net.pool));
	GPCC_DO(0 == apr_threadattr_detach_set(ta, 0));
	GPCC_DO(0 == apr_thread_create(&net.thread, ta, net_run, &net, net.pool));
    } 

    return 0;

    bail:
    if (spin) spin = gpcc_spin_release(spin);
    gpcc_net_stop();
    return e;
}


APR_DECLARE(void) gpcc_net_stop(void)
{
    apr_thread_t* thread = net.thread;
    apr_status_t status;
    apr_uint32_t *spin = 0;
    int i;

    if (thread) {
	net.thread = 0;

	spin = gpcc_spin_secure(net.net_spin);
	/* delete all events */
	event_del(&net.listen_event);
	for (i = 0 ; i < GPCC_NET_CONN_MAX ; i++){
	    if (net.tab.item[i]) {
		event_del(&net.tab.item[i]->sock_event);
	    }
	}
	gpcc_spin_release(spin);
	apr_thread_join(&status, thread);
    }
    if (net.pool) 
	apr_pool_destroy(net.pool);
    memset(&net, 0, sizeof(net));
}

APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* pconn, const char* addr, 
					void* arg)

{
    apr_pool_t* pool = 0;
    apr_uint32_t* spin = 0;
    int e;
    char* addr2;
    char* scope;
    apr_port_t port;
    apr_sockaddr_t* sa;
    SOCKET sock;
    struct hostent *server;
    struct sockaddr_in serv_addr;

    spin = gpcc_spin_secure(net.net_spin);
    if (0 != (e = apr_pool_create(&pool, net.pool)))
	goto bail;
    spin = gpcc_spin_release(spin);

    /* resolve addr */
    if (0 != (e = apr_parse_addr_port(&addr2, &scope, &port, addr, pool)))
	goto bail;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
	goto bail;

    server = gethostbyname(addr2);
    if (server == 0) 
	goto bail;
    
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy((char *) server->h_addr, (char *) &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
	goto bail;

    *pconn = mkitem(sock, arg, 
		    1 /*is_connecting*/, 
		    0 /*is_open*/, 
		    1 /*self_origin*/, 
		    pool);
    if (! (*pconn))
	goto bail;

    return 0;

    bail:
    if (spin) spin = gpcc_spin_release(spin);
    if (pool) apr_pool_destroy(pool);
    return e;
}

APR_DECLARE(apr_status_t) gpcc_net_close(gpcc_net_conn_t conn)
{
    item_t* item;
    apr_uint32_t* spin;
    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }
    item->is_closing = 1;
    spin = gpcc_spin_release(spin);
    do_closed(item);
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_read(gpcc_net_conn_t conn, 
					char* buf, int len)
{
    apr_uint32_t* spin;
    item_t* item;
    buf_t* bb;
    int n;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }

    bb = &item->in;
    n = bb->top - bb->bot;
    if (n < len) {
	spin = gpcc_spin_release(spin);
	return APR_EAGAIN;
    }
	
    memcpy(buf, bb->buf + bb->bot, len);
    bb->bot += n;
    spin = gpcc_spin_release(spin);
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_readln(gpcc_net_conn_t conn, 
					  char* buf, int len)
{
    apr_uint32_t* spin;
    item_t* item;
    buf_t* bb;
    int n;
    char* p;
    char* q;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }
    bb = &item->in;
    n = bb->top - bb->bot;
    p = bb->buf + bb->bot;
    q = memchr(p, '\n', n);
    if (!q) {
	spin = gpcc_spin_release(spin);
	return APR_EAGAIN;
    }
    n = q + 1 - p;
    if (n > len - 1) 		/* save 1 char for \0 */
	n = len - 1;
    memcpy(buf, p, n);
    buf[n] = 0;

    bb->bot += n;
    spin = gpcc_spin_release(spin);
    return 0;
}

APR_DECLARE(apr_status_t) gpcc_net_read_packet(gpcc_net_conn_t conn, 
					       gpcc_net_packet_t** packet)
{
    gpcc_net_packet_t pkt;
    char* p;
    item_t* item;
    buf_t* bb;
    int n;
    apr_uint32_t* spin;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }
    bb = &item->in;
    n = bb->top - bb->bot;
    p = bb->buf + bb->bot;
    if (n >= sizeof(pkt.mh)) {
	memcpy(&pkt, p, sizeof(pkt.mh));
	pkt.mh.type = ntohs(pkt.mh.type);
	pkt.mh.len = ntohs(pkt.mh.len);
	pkt.mh.sndidx = ntohl(pkt.mh.sndidx);
	pkt.mh.rcvidx = ntohl(pkt.mh.rcvidx);
    }
    if (n < sizeof(pkt.mh) + pkt.mh.len)  {
	spin = gpcc_spin_release(spin);
	return APR_EAGAIN;
    }
    *packet = malloc(sizeof(**packet) + pkt.mh.len - 1);
    (*packet)->mh = pkt.mh;
    bb->bot += sizeof(pkt.mh);
    p += sizeof(pkt.mh);
    memcpy((*packet)->mb, p, pkt.mh.len);
    bb->bot += pkt.mh.len;
    spin = gpcc_spin_release(spin);
    return 0;
}

APR_DECLARE(void) gpcc_net_free_packet(gpcc_net_packet_t* packet)
{
    free(packet);
}


APR_DECLARE(apr_status_t) gpcc_net_send_packet(gpcc_net_conn_t conn,
					       int type, 
					       int sndidx, int rcvidx,
					       const char* buf, int len)
{
    gpcc_net_packet_t pkt;
    const char* tbuf[2];
    int tlen[2];
    pkt.mh.type = htons(type);
    pkt.mh.len = htons(len);
    pkt.mh.sndidx = htonl(sndidx);
    pkt.mh.rcvidx = htonl(rcvidx);

    tbuf[0] = (char*) &pkt.mh, tlen[0] = sizeof(pkt.mh);
    tbuf[1] = buf, tlen[1] = len;

    return gpcc_net_sendv(conn, 2, tbuf, tlen);
}




APR_DECLARE(apr_status_t) gpcc_net_sendv(gpcc_net_conn_t conn, 
					 int nbuf,
					 const char* buf[], int bufsz[])
{
    int tot = 0;
    int i;
    apr_uint32_t* spin;
    int e = 0;
    item_t* item;
    buf_t* bb;
    int bbfree;

    for (i = 0; i < nbuf; i++) {
	if (bufsz[i] < 0) 
	    return APR_EINVAL;
	tot += bufsz[i];
    }
    if (tot > sizeof(bb->buf)) {
	gpcc_log_error("gpcc_net_sendv: packet too big (%d, max %d)", tot, sizeof(bb->buf));
	return APR_EINVAL;
    }

    retry:
    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	e = APR_EINVAL;
	goto bail;
    }
    bb = &item->out;
    bbfree = sizeof(bb->buf) - bb->top; /* freespace in buf */

    if (bbfree < tot) {
	/* compact buffer */
	memmove(bb->buf, bb->buf + bb->bot, bb->top - bb->bot);
	bb->top = bb->top - bb->bot;
	bb->bot = 0;
	bbfree = sizeof(bb->buf) - bb->top;
	if (bbfree < tot) {
	    if (do_write(item)) { /* flush */
		item->is_closing = 1;
		e = APR_EINVAL;
		goto CLOSED;
	    }
	    spin = gpcc_spin_release(spin);
	    goto retry;
	}
    }

    for (i = 0; i < nbuf; i++) {
	memcpy(bb->buf + bb->top, buf[i], bufsz[i]);
	bb->top += bufsz[i];
    }
    spin = gpcc_spin_release(spin);
    return 0;

    bail:
    if (spin) spin = gpcc_spin_release(spin);
    return e;

    CLOSED:

    if (spin) spin = gpcc_spin_release(spin);
    do_closed(item);
    return e;
}


APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					const char* buf, int buflen)
{
    return gpcc_net_sendv(conn, 1, &buf, &buflen);
}



#ifdef UNDEF
APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					const gpcc_net_msghdr_t* msghdr, 
					const char* msgbody)
{
    gpcc_net_msghdr_t nethdr;

    nethdr.mtype = htons(msghdr->mtype);
    nethdr.mlen = htons(msghdr->mlen);
    nethdr.sndidx = htonl(msghdr->sndidx);
    nethdr.rcvidx = htonl(msghdr->rcvidx);

    return send_raw(conn, (char*) &nethdr, sizeof(nethdr), msgbody, msghdr->mlen);
}
#endif 


APR_DECLARE(apr_status_t) gpcc_net_setarg(gpcc_net_conn_t conn, void* arg)
{
    item_t* item;
    apr_uint32_t* spin;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }
    item->arg = arg;
    spin = gpcc_spin_release(spin);
    return 0;
}


APR_DECLARE(void*) gpcc_net_getarg(gpcc_net_conn_t conn)
{
    item_t* item;
    void* arg = 0;
    apr_uint32_t* spin;

    spin = gpcc_spin_secure(net.net_spin);
    if (0 != (item = getitem(conn))) 
	arg = item->arg;
    spin = gpcc_spin_release(spin);
    return arg;
}




APR_DECLARE(int) gpcc_net_get_conns(gpcc_net_conn_t conns[GPCC_NET_CONN_MAX])
{
    apr_uint32_t* spin;
    item_t** p = &net.tab.item[0];
    item_t** q = &net.tab.item[GPCC_NET_CONN_MAX];
    int count = 0;

    spin = gpcc_spin_secure(net.net_spin);
    for ( ; p < q; p++) {
	if (*p && (*p)->is_open)
	    conns[count++] = (*p)->conn;
    }
    spin = gpcc_spin_release(spin);
    return count;
}


APR_DECLARE(apr_status_t) gpcc_net_send_request(gpcc_net_conn_t conn,
						const gpcc_net_request_t* req)
{
    char buf[1024*8];
    char* p = buf;
    char* q = buf + sizeof(buf);
    int i;

    for (i = 0; i < req->argc; i++) 
	p += apr_snprintf(p, q - p, "%s ", req->argv[i]);
    if (i) 
	p += apr_snprintf(p, q - p, "\r\n");
    for (i = 0; i < req->hc; i++) 
	p += apr_snprintf(p, q - p, "%s: %s\r\n", req->hname[i], req->hvalue[i]);
    p += apr_snprintf(p, q - p, "\r\n");

    if (p == q)
	return APR_EINVAL;

    return gpcc_net_send(conn, buf, p - buf);
}

APR_DECLARE(apr_status_t) gpcc_net_read_request(gpcc_net_conn_t conn, 
						gpcc_net_request_t** req, 
						apr_pool_t* pool)
{
    apr_uint32_t* spin;
    item_t* item;
    buf_t* bb;
    int n;

    *req = 0;
    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }

    bb = &item->in;
    n = bb->top - bb->bot;
    *req = gpcc_net_parse_request(bb->buf + bb->bot, &n, pool);
    bb->bot += n;

    spin = gpcc_spin_release(spin);
    return *req ? 0 : APR_EAGAIN;
}

    
APR_DECLARE(apr_status_t) gpcc_net_read_request_data(gpcc_net_conn_t conn,
						     gpcc_net_request_t *req)
{
    apr_uint32_t *spin;
    item_t *item;
    buf_t *bb;
    int n, nn;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	spin = gpcc_spin_release(spin);
	return APR_EINVAL;
    }

    bb = &item->in;
    n = bb->top - bb->bot;
    if (n == 0) {
	spin = gpcc_spin_release(spin);
	return APR_EAGAIN;
    }
 
    nn = req->datalen - req->offset;
    
    n = (n > nn) ? nn : n ;
    memcpy(req->data + req->offset, bb->buf + bb->bot, n);
    req->offset += n;
    bb->bot += n;
    spin = gpcc_spin_release(spin);

    return (req->datalen == req->offset) ? 0 : APR_EAGAIN;
}
    
    
APR_DECLARE(gpcc_net_request_t*) gpcc_net_parse_request(const char* buf, int* len, apr_pool_t* pool)
{
    int n = *len;
    const char* p;
    char* line;
    char* last;
    gpcc_net_request_t* req = 0;
    char* colon;
    int empty, completed;

    /* find an empty line */
    *len = 0;
    empty = 1, completed = 0;
    for (p = buf; n > 0 && *p; p++, n--) {
	int ch = *p;
	/* skip spaces */
	if (ch == ' ' || ch == '\t' || ch == '\r') 
	    continue;
	if (ch == '\n') {
	    if (! empty) {
		empty = 1;
		continue;
	    }
	    p++;
	    completed = 1;
	    break;
	}
	empty = 0;
    }
    if (! completed)
	return 0;
    
    /* we have a complete HTTP-style HEADER request (terminated by empty line) */
    *len = n = p - buf; /* consume it */
    line = apr_pstrndup(pool, buf, n); /* dup it */

    /* for first line */
    line = apr_strtok(line, "\n", &last);
    line = gpcc_string_trim(line);

    if (strcasestr(line, GPCC_NET_PROTO)) {
	req = apr_pcalloc(pool, sizeof(gpcc_net_request_t));
	
    } 
    else if (strcasestr(line, HTTP_NET_PROTO)) {
	req = apr_pcalloc(pool, sizeof(gpcc_http_request_t));
    }
    else {
	gpcc_log_error("Protocol not supported: %s", line);
	return 0;
    }

    if (0 != apr_tokenize_to_argv(line, &req->argv, pool))
	return req;
    while (req->argv[req->argc]) 
	req->argc++;

    /* for each subsequent lines */
    while (0 != (line = apr_strtok(0, "\n", &last))) {
	if (*line == ' ' || *line == '\t') {
	    /* continuation */
	    if (req->hc == 0)	/* illegal - missing first header */
		break;
	    line = gpcc_string_trim(line);
	    if (*line == 0)	/* empty line */
		break;
	    /* add to previous hvalue */
	    req->hvalue[req->hc-1] 
		= gpcc_string_trim(apr_pstrcat(pool, req->hvalue[req->hc-1], 
					       " ", line, 0));
	    continue;
	}
	/* find a colon, and break the line in two */
	if (! (colon = strchr(line, ':')))
	    colon = line + strlen(line); 
	else
	    *colon++ = 0;

	line = gpcc_string_trim(line);
	if (*line == 0) 	/* empty line */
	    break;

	/* save name, value pair */
	req->hname[req->hc] = line;
	req->hvalue[req->hc] = gpcc_string_trim(colon);
	req->hc++;

	if (req->hc >= sizeof(req->hname) / sizeof(req->hname[0]))
	    break;
    }

    return req;
}



static const char *_net_request_get_header(gpcc_net_request_t *req, const char *header)
{
    int i;

    for (i = 0 ; i < req->hc ; i++){
	if (apr_strnatcasecmp(req->hname[i], header) == 0) {
	    return req->hvalue[i];
	}
    }
    return 0;
}

APR_DECLARE(const char *) gpcc_net_request_get_header(gpcc_net_request_t *req, const char * header,
						      const char *def)
{
    const char *value = _net_request_get_header(req, header);
    if (value)
	return value;
    return def;
}

APR_DECLARE(int) gpcc_net_request_get_header_int(gpcc_net_request_t *req, const char * header,
						 int def)
{
    const char *value = _net_request_get_header(req, header);
    if (value)
	return atoi(value);
    return def;
}

APR_DECLARE(float) gpcc_net_request_get_header_float(gpcc_net_request_t *req, const char * header,
					       float def)
{
    const char *value = _net_request_get_header(req, header);
    if (value)
	return atof(value);
    return def;
}



APR_DECLARE(gpcc_net_node_id_t) gpcc_net_get_node_id(void)
{
    static gpcc_net_node_id_t* my_node_id;

    if (! my_node_id) {
	gpcc_net_node_id_t* t = malloc(sizeof(gpcc_net_node_id_t));
	apr_sha1_ctx_t sha1ctx;
	char sha1buf[80];

	GPCC_DO(0 == apr_generate_random_bytes((unsigned char*)sha1buf, 20));
	sprintf(sha1buf + 20, " gpcc %lld ", apr_time_now());
	apr_sha1_init(&sha1ctx);
	apr_sha1_update(&sha1ctx, sha1buf, strlen(sha1buf));
	apr_sha1_final(t->opaque, &sha1ctx);

	my_node_id = t;
    }
    return *my_node_id;
}

