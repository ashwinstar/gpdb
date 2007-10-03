#include <gpcc_net.h>
#include <gpcc_spin.h>
#include <gpcc_strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "event.h"		/* libevent */
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>


APR_DECLARE(apr_status_t) gpcc_net_parse_request(gpcc_net_header_t** hdr, const char* buf, int* len, 
						 apr_pool_t* pool);

static gpcc_net_header_t* gpcc_net_header_copy(int argc, char** argv, int hc, 
					       char** hname, char** hvalue, apr_pool_t* pool)
{
    gpcc_net_header_t* hdr = apr_palloc(pool, sizeof(*hdr));
    int i;

    if (argc >= GPCC_NET_HEADER_ENTRYMAX) 
	return 0;

    if (hc >= GPCC_NET_HEADER_ENTRYMAX) 
	return 0;

    for (i = 0; i < argc; i++) 
	hdr->argv[i] = argv[i];
    hdr->argv[i] = 0;
    
    for (i = 0; i < hc; i++) {
	hdr->hname[i] = hname[i];
	hdr->hvalue[i] = hvalue[i];
    }
    hdr->hname[i] = 0;
    hdr->hvalue[i] = 0;

    return hdr;
}

static gpcc_net_header_t* gpcc_net_header_dup(gpcc_net_header_t* src, char* buf, int buflen)
{
    int i;
    gpcc_net_header_t* hp;
    int len;

    len = sizeof(*hp);
    for (i = 0; i < src->argc; i++)
	len += strlen(src->argv[i]) + 1;
    for (i = 0; i < src->hc; i++)
	len += strlen(src->hname[i]) + 1 + strlen(src->hvalue[i]) + 1;
    if (len > buflen)
	return 0;

    hp = (gpcc_net_header_t*) buf;
    buf += sizeof(*hp);
    memset(hp, 0, sizeof(*hp));
    hp->argc = src->argc;
    for (i = 0; i < src->argc; i++) {
	strcpy(buf, src->argv[i]);
	hp->argv[i] = buf;
	buf += strlen(buf) + 1;

    }
    hp->argv[i] = 0;

    hp->hc = src->hc;
    for (i = 0; i < src->hc; i++) {
	strcpy(buf, src->hname[i]);
	hp->hname[i] = buf;
	buf += strlen(buf) + 1;
	strcpy(buf, src->hvalue[i]);
	hp->hvalue[i] = buf;
	buf += strlen(buf) + 1;
    }
    hp->hname[i] = 0;
    hp->hvalue[i] = 0;

    return hp;
}


								  

typedef int SOCKET;

#define closesocket close

#define CONN_MAX  256
#define CONN_INDEX(conn)     (((int)(conn & 0xffff)) < 256 ? (int)(conn & 0xffff) : -1)
#define CONN_UNIQUE(conn)    ((conn >> 16) & 0xffff)
#define CONN_DEFINE(unique, index)   ((((apr_uint32_t) unique) << 16) | (index & 0xffff))
#define CONN_TO_PTR(conn)    ((void*) ((char*)0 + (conn)))
#define CONN_FROM_PTR(ptr)   ((gpcc_net_conn_t) ((char*)(ptr) - (char*)0))


#define STATE_NULL       0
#define STATE_OPENING    1
#define STATE_ACCEPTING  2
#define STATE_READING    4
#define STATE_WRITING    8


typedef struct buf_t buf_t;
struct buf_t {
    char byte[1024*16];
    int bot, top;
};


typedef struct conncb_t conncb_t;
struct conncb_t {
    gpcc_net_conn_t conn;
    apr_uint32_t spin;
    SOCKET sock;
    apr_pool_t* pool;
    char peer_addr[24];  /* format IP:port -- 123.456.789.012:34567 */
    struct event ev;
    struct {
	buf_t* buf;
	gpcc_net_header_t* hdr;
    } to_peer, from_peer;
    int do_read_header_attempts;
};

typedef struct net_t net_t;
struct net_t {
    gpcc_net_opened_t* opened;
    gpcc_net_accepted_t* accepted;
    gpcc_net_received_t* received;
    gpcc_net_closed_t* closed;
    apr_pool_t* pool;
    SOCKET listen_sock;
    struct event ev_listen;
    struct event ev_break;
    int send_break_fd;
    int recv_break_fd;
    struct {
	apr_thread_mutex_t* mutex;
	apr_thread_cond_t*  notfull;
    } wait;

    apr_uint16_t unique;
    apr_uint32_t conntab_spin;
    conncb_t conntab[CONN_MAX];

};
static net_t net = {0};


static int ev_add(struct event* ev, struct timeval* tm)
{
    int e;
    e = event_add(ev, tm);
    return e;
}


static inline apr_uint32_t* conn_secure(gpcc_net_conn_t conn, conncb_t** ppconn)
{
    int index = CONN_INDEX(conn);
    conncb_t* pconn;

    *ppconn = NULL;
    if (index < 0)
	return NULL;

    pconn = &net.conntab[index];
    gpcc_spin_secure(&pconn->spin);
    if (pconn->conn != conn) {
	gpcc_spin_release(&pconn->spin);
	return NULL;
    }

    *ppconn = pconn;
    return &pconn->spin;
}



static void zap(conncb_t* pconn, gpcc_net_conn_t conn)
{
    gpcc_spin_secure(&pconn->spin);
    if (pconn->conn == conn) {
	pconn->conn = 0;
	event_del(&pconn->ev);
	if (pconn->sock) {
	    shutdown(pconn->sock, SHUT_WR | SHUT_RD);
	    closesocket(pconn->sock);
	    pconn->sock = 0;
	}
	if (pconn->pool) {
	    apr_pool_destroy(pconn->pool);
	    pconn->pool = 0;
	}
	pconn->to_peer.buf = 0;
	pconn->to_peer.hdr = 0;
	pconn->from_peer.buf = 0;
	pconn->from_peer.hdr = 0;
    }
    gpcc_spin_release(&pconn->spin);
    net.closed(conn);
}

static void do_ready(int fd, short event, void* arg);


static void conn_setup_event(gpcc_net_conn_t conn)
{
    conncb_t* pconn;
    apr_uint32_t* spin = 0;
    buf_t* out;
    buf_t* in;
    struct timeval tm;
    int flag = 0;
    struct timeval* ptm = 0;

    tm.tv_sec = 0;
    tm.tv_usec = 500 * 1000;

    if (! (spin = conn_secure(conn, &pconn)))
	return;

    out = pconn->to_peer.buf;
    in = pconn->from_peer.buf;

    if (out->bot != out->top) 
	flag |= EV_WRITE;
    if (in->top - in->bot != sizeof(in->byte))
	flag |= EV_READ;
    if (0 != (flag & EV_READ) && !pconn->to_peer.hdr && !pconn->from_peer.hdr)
	ptm = &tm;

    event_del(&pconn->ev);
    event_set(&pconn->ev, pconn->sock, flag, do_ready, CONN_TO_PTR(conn));
    if (event_add(&pconn->ev, ptm)) {
	gpcc_warning(FLINE, "event_add failed");
	goto failure;
    }

    spin = gpcc_spin_release(spin);
    return;

    failure:
    if (spin) 
	spin = gpcc_spin_release(spin);
    zap(pconn, conn);
}

static void conn_read_header(gpcc_net_conn_t conn)
{
    buf_t* buf;
    int len;
    apr_uint32_t* spin = 0;
    conncb_t* pconn;

    if (! (spin = conn_secure(conn, &pconn)))
	return;

    buf = pconn->from_peer.buf;
    if (pconn->from_peer.hdr) {
	gpcc_fatal(FLINE, "internal error: already have a header");
    }
    buf = pconn->from_peer.buf;
    if (buf->top == sizeof(buf->byte)) {
	/* overflow while waiting to read header */
	gpcc_warning(FLINE, "buffer full while reading header");
	goto failure;
    }
    if (pconn->do_read_header_attempts++ > 2) {
	/* don't let this go on forever */
	gpcc_warning(FLINE, "timeout while reading header");
	goto failure;
    }

    len = buf->top - buf->bot;
    if (gpcc_net_parse_request(&pconn->from_peer.hdr, 
			       buf->byte + buf->bot, &len, pconn->pool))
	goto failure;
    buf->bot += len;

    if (! pconn->from_peer.hdr) {
	spin = gpcc_spin_release(spin);
	return;
    }

    {
	/* we have a header ... do the callback */
	/* make a copy of net_header to pass back. it is not safe to pass 
	   pconn->from_peer.hdr to the 'accepted' callback because we must
	   release the spin lock before calling the callback. */
	char from_peer_buf[1024*4], to_peer_buf[1024];
	char peer_addr[sizeof(pconn->peer_addr)];
	gpcc_net_header_t* to_peer_hdr = 0;
	gpcc_net_header_t* from_peer_hdr = 0;

	strncpy(peer_addr, pconn->peer_addr, sizeof(peer_addr));
	peer_addr[sizeof(peer_addr) - 1] = 0;

	if (pconn->from_peer.hdr) {
	    from_peer_hdr = gpcc_net_header_dup(pconn->from_peer.hdr, from_peer_buf, sizeof(from_peer_buf));
	    if (! from_peer_hdr) {
		gpcc_warning(FLINE, "error creating header from peer");
		goto failure;
	    }
	}

	if (pconn->to_peer.hdr) {
	    to_peer_hdr = gpcc_net_header_dup(pconn->to_peer.hdr, to_peer_buf, sizeof(to_peer_buf));
	    if (! to_peer_hdr) {
		gpcc_warning(FLINE, "error creating header to peer");
		goto failure;
	    }
	}

	spin = gpcc_spin_release(spin); /* release prior to callback */

	if (to_peer_hdr) 
	    net.opened(conn, peer_addr, to_peer_hdr, from_peer_hdr);
	else 
	    net.accepted(conn, peer_addr, from_peer_hdr);
    }
    return;

failure:
    if (spin) 
	spin = gpcc_spin_release(spin);
    zap(pconn, conn);
    return;
}



static void conn_fire_read(gpcc_net_conn_t conn)
{
    buf_t* in;
    apr_uint32_t* spin;
    conncb_t* pconn;

    if (! (spin = conn_secure(conn, &pconn)))
	return;
    
    in = pconn->from_peer.buf;
    if (in->bot != in->top) {
	if (pconn->to_peer.hdr == 0 && pconn->from_peer.hdr == 0) {
	    /* newly accepted connection */
	    spin = gpcc_spin_release(spin);
	    conn_read_header(conn);
	    return;
	}
	else {
	    gpcc_fatal(FLINE, "not implemented");
	}
    }

    gpcc_spin_release(spin);
    return;
}


static void do_ready(int fd, short event, void* arg)
{
    gpcc_net_conn_t conn = CONN_FROM_PTR(arg);
    apr_uint32_t* spin = 0;
    conncb_t* pconn;

    if (! (spin = conn_secure(conn, &pconn)))
	return;

    if (0 != (event & EV_READ)) {
	buf_t* in = pconn->from_peer.buf;
	if (in->top == in->bot) 
	    in->top = in->bot = 0;
	else if (in->top == sizeof(in->byte) && in->bot != 0) {
	    const int n = in->top - in->bot;
	    memmove(in->byte, in->byte + in->bot, n);
	    in->top = n;
	    in->bot = 0;
	}
	if (in->top != sizeof(in->byte)) {
	    int n = sizeof(in->byte) - in->top;
	    n = recv(fd, in->byte + in->top, n, 0);
	    if (n == 0) {
		goto failure;	/* closed by remote peer */
	    }
	    if (n < 0 && errno != EAGAIN) {
		gpcc_warning(FLINE, "read failed");
		goto failure;
	    }
	    in->top += n;
	}
    }
    if (0 != (event & EV_WRITE)) {
	buf_t* out = pconn->to_peer.buf;
	int      n = out->top - out->bot;
	if (n > 0) {
	    n = send(fd, out->byte + out->bot, n, 0);
	    if (n == 0) {
		goto failure;	/* closed by remote peer */
	    }
	    if (n < 0 && errno != EAGAIN) {
		gpcc_warning(FLINE, "write failed");
		goto failure;
	    }
	    out->bot += n;
	}
    }

    spin = gpcc_spin_release(spin);
    if (0 != (event & EV_READ))
	conn_fire_read(conn);
    conn_setup_event(conn);

    return;
    failure:
    if (spin) gpcc_spin_release(spin);
    zap(pconn, conn);
}




static void do_break(int fd, short event, void* arg)
{
    gpcc_net_conn_t conn;

    if (sizeof(conn) != read(fd, &conn, sizeof(conn))) 
	gpcc_fatal(FLINE, "internal error: read failed");
    
    conn_setup_event(conn);
}




static apr_status_t set_sockopt_nonblock(int sock)
{
    /* set to non-blocking, and close-on-exec */
#ifdef WIN32
    {
	unsigned long nonblocking = 1;
	ioctlsocket(sock, FIONBIO, &nonblocking);
    }
#else
    {
	if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
	    gpcc_warning(FLINE, "fcntl(F_SETFL, O_NONBLOCK) failed");
	    return APR_FROM_OS_ERROR(errno);
	}
	if (fcntl(sock, F_SETFD, 1) == -1) {
	    gpcc_warning(FLINE, "fcntl(F_SETFD) failed");
	    return APR_FROM_OS_ERROR(errno);
	}
    }
#endif

    /* set keepalive, reuseaddr, and linger */
    {
	int on = 1;
	struct linger linger;
	setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
	linger.l_onoff = 1;
	linger.l_linger = 5;
	setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));
    }

    return 0;
}


static void do_accept(int fd, short event, void* arg)
{
    struct sockaddr_in sockaddr;
    socklen_t sockaddr_len = sizeof(sockaddr);
    SOCKET sock = -1;
    apr_pool_t* pool = 0;
    int i, unique;
    conncb_t* pconn = 0;
    apr_uint32_t* spin = 0;
    gpcc_net_conn_t conn = 0;

    /* do the accept */
    if ((sock = accept(fd, (void*) &sockaddr, &sockaddr_len)) < 0) {
	gpcc_warning(FLINE, "accept failed");
	goto failure;
    }

    /* set to non-blocking, and close-on-exec */
    if (set_sockopt_nonblock(sock)) {
	gpcc_warning(FLINE, "unable to set sockopt");
	goto failure;
    }

    /* create a pool container for this socket */
    if (apr_pool_create(&pool, net.pool)) {
	gpcc_warning(FLINE, "unable to accept connection - out of memory");
	goto failure;
    }

    /* find a free slot */
    spin = gpcc_spin_secure(&net.conntab_spin);
    unique = net.unique++;
    if (unique == 0) unique = net.unique++;
    pconn = 0;
    for (i = 0; i < CONN_MAX; i++) {
	int index = (unique + i) % CONN_MAX;
	pconn = &net.conntab[index];
	if (pconn->conn) {
	    pconn = 0;
	    continue;
	}
	gpcc_spin_secure(&pconn->spin);
	if (pconn->conn) {
	    gpcc_spin_release(&pconn->spin);
	    pconn = 0;
	    continue;
	}
	pconn->conn = CONN_DEFINE(unique, index);
	break;
    }
    if (!pconn) {
	gpcc_warning(FLINE, "unable to accept connection - connection table full");
	goto failure;
    }
    conn = pconn->conn;

    /* crab over to pconn */
    gpcc_spin_release(spin);
    spin = &pconn->spin;	/* we grabbed the spin earlier in the loop above */

    pconn->pool = pool;
    pconn->sock = sock;
    memset(&pconn->to_peer, 0, sizeof(pconn->to_peer));
    memset(&pconn->from_peer, 0, sizeof(pconn->from_peer));
    pconn->to_peer.buf = apr_palloc(pool, sizeof(buf_t));
    pconn->from_peer.buf = apr_palloc(pool, sizeof(buf_t));
    pconn->peer_addr[0] = 0;
    if (! inet_ntop(sockaddr.sin_family, &sockaddr.sin_addr, pconn->peer_addr, sizeof(pconn->peer_addr) - 7)) {
	gpcc_warning(FLINE, "inet_ntop failed");
	goto failure;
    }
    sprintf(pconn->peer_addr + strlen(pconn->peer_addr), ":%d", ntohs(sockaddr.sin_port));
    
    pconn->do_read_header_attempts = 0;
    conn = pconn->conn;
    spin = gpcc_spin_release(spin);

    conn_setup_event(conn);

    return;			/* success */

failure:
    if (pconn)
	pconn->conn = 0;
    if (spin) 
	gpcc_spin_release(spin);
    if (pool)
	apr_pool_destroy(pool);
    if (sock >= 0)
	closesocket(sock);
}



APR_DECLARE(apr_status_t) gpcc_net_sendv(gpcc_net_conn_t conn,
					 gpcc_net_datum_t* datum, int dcnt)
{
    int tot, i;
    const int max = sizeof(((buf_t*)0)->byte);
    apr_uint32_t* spin = 0;
    conncb_t* pconn;
    buf_t* buf;
    int e = 0;
    int avail;

    /* compute total length */
    for (i = tot = 0; i < dcnt; i++)
	tot += datum[i].len;

    /* too long ? */
    if (tot >= max) {
	gpcc_warning(FLINE, "datum longer than %d", max);
	e = APR_EINVAL;
	goto failure;
    }

    again:
    /* grab the conn */
    if (! (spin = conn_secure(conn, &pconn)))
	return APR_EINVAL;

    /* while full wait */
    buf = pconn->to_peer.buf;
    avail = max - (buf->top - buf->bot);
    if (avail < tot) {
	spin = gpcc_spin_release(spin);
	if (0 != (e = apr_thread_mutex_lock(net.wait.mutex)))
	    goto failure;
	if (0 != (e = apr_thread_cond_wait(net.wait.notfull, net.wait.mutex))) {
	    apr_thread_mutex_unlock(net.wait.mutex);
	    goto failure;
	}
	goto again;		/* retry  */
    }

    /* compact if necessary */
    if (buf->top + tot > max) {
	const int n = buf->top - buf->bot;
	if (n < 0) 
	    gpcc_fatal(FLINE, "internal error: top %d, bot %d", buf->top, buf->bot);
	else if (n > 0) 
	    memmove(buf->byte + buf->bot, buf->byte, n);
	buf->bot = 0, buf->top = n;
    }
    avail = max - (buf->top - buf->bot);
    if (avail < tot) 
	gpcc_fatal(FLINE, "internal error: avail %d, tot %d", avail, tot);
    
    /* copy into buf */
    for (i = 0; i < dcnt; i++) {
	const int n = datum[i].len;
	memcpy(buf->byte + buf->top, datum[i].ptr, n);
	buf->top += n;
    }

    /* the buffer was empty when we started. may need to kick the event loop */
    if (avail == max) {
	if (sizeof(conn) != write(net.send_break_fd, &conn, sizeof(conn)))  {
	    gpcc_warning(FLINE, "unable to send conn to break event loop");
	    goto failure;
	}
    }

    gpcc_spin_release(spin);
    return 0;
    
 failure:
    if (spin)
	gpcc_spin_release(spin);
    return e;
}





APR_DECLARE(apr_status_t) gpcc_net_init(int port,
					gpcc_net_opened_t* opened,
					gpcc_net_accepted_t* accepted,
					gpcc_net_received_t* received,
					gpcc_net_closed_t* closed)
{
    apr_pool_t* pool;
    int e;
    struct sockaddr_in a;
    SOCKET sock = 0;

    /* check param */
    if (net.pool) {
	gpcc_log_error("gpcc_net_init: already initialized (%s)", FLINE);
	return APR_EINVAL;
    }

    if (!opened) {
	gpcc_log_error("gpcc_net_init: invalid 'opened' param");
	return APR_EINVAL;
    }

    if (!accepted) {
	gpcc_log_error("gpcc_net_init: invalid 'accepted' param");
	return APR_EINVAL;
    }

    if (!received) {
	gpcc_log_error("gpcc_net_init: invalid 'received' param");
	return APR_EINVAL;
    }

    if (!closed) {
	gpcc_log_error("gpcc_net_init: invalid 'closed' param");
	return APR_EINVAL;
    }

    /* make our own pool */
    if (0 != (e = apr_pool_create(&pool, 0)))
	return e;

    /* set up the server socket */
    if (port > 0) {
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	a.sin_family = AF_INET;

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
	    gpcc_fatal(FLINE, "cannot create socket on port %d", port);

	if (0 != (e = set_sockopt_nonblock(sock)))
	    gpcc_fatal(FLINE, "unable to set socket to nonblocking on port %d", port);

	/* bind */
	if (bind(sock, (void*) &a, sizeof(a)))
	    gpcc_fatal(FLINE, "cannot create socket on port %d - bind failed", port);

	/* listen */
	if (listen(sock, 64))
	    gpcc_fatal(FLINE, "cannot listen on port %d", port);
    }

    /* save our callback functions */
    memset(&net, 0, sizeof(net));
    net.pool = pool;
    net.opened = opened;
    net.accepted = accepted;
    net.received = received;
    net.closed = closed;
    net.listen_sock = sock;

    /* create the mutex & cond */
    if (0 != (e = apr_thread_mutex_create(&net.wait.mutex, 0, pool))) 
	gpcc_fatal(FLINE, "apr_thread_mutex_create failed, error %d", e);
    
    if (0 != (e = apr_thread_cond_create(&net.wait.notfull, pool)))
	gpcc_fatal(FLINE, "apr_thread_cond_create failed, error %d", e);

    /* create a pipe to break the event loop */
    {
	int pfd[2];
	if (pipe(pfd) < 0) 
	    gpcc_fatal(FLINE, "pipe failed, errno %d", errno);
	net.send_break_fd = pfd[1];
	net.recv_break_fd = pfd[0];
    }

    /* initialize libevent */
    if (! event_init()) 
	gpcc_fatal(FLINE, "event_init failed");
    
    /* set up to accept connections listen_sock */
    event_set(&net.ev_listen, net.listen_sock, EV_READ | EV_PERSIST, do_accept, 0);
    if (ev_add(&net.ev_listen, 0))
	gpcc_fatal(FLINE, "event_add: cannot set up event on listen socket");

    /* set up to get break */
    event_set(&net.ev_break, net.recv_break_fd, EV_READ | EV_PERSIST, do_break, 0);
    if (ev_add(&net.ev_break, 0))
	gpcc_fatal(FLINE, "event_add: cannot set up event on listen socket");

    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_once(int blocking)
{
    return event_loop(EVLOOP_ONCE | (blocking ? 0 : EVLOOP_NONBLOCK));
}


APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* conn,
					char* peer_addr_str,
					int argc, char** argv,
					int hc, char** hname, char** hvalue)
{
    SOCKET sock = -1;
    struct sockaddr_in peer_addr;
    int port = 0;
    char* p;
    char addr[100];
    struct hostent* hp;
    int i, e = 0;
    apr_pool_t* pool = 0;
    apr_uint32_t* spin = 0;
    int unique = 0;
    conncb_t* pconn = 0;
    gpcc_net_header_t* hdr = 0;

    if (0 != (e = apr_pool_create(&pool, net.pool))) 
	goto failure;

    /* copy argc, argv, hc, hname, hvalue into gpcc_net_header */
    hdr = gpcc_net_header_copy(argc, argv, hc, hname, hvalue, pool);
    if ( ! hdr) {
	e = APR_EINVAL;
	goto failure;
    }

    /* parse the peer_addr_str into (hostname, ip) pair*/
    strncpy(addr, peer_addr_str, sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = 0;

    if (0 == (p = strchr(addr, ':'))) {
	e = APR_EINVAL;
	goto failure;
    }

    *p++ = 0;
    port = strtol(p, 0, 0);
    if (! (0 <= port && port < (1 << 16)))  {
	e = APR_EINVAL;
	goto failure;
    }
    
    /* get the host ip */
    if (0 == (hp = gethostbyname(addr))) {
	e = APR_EINVAL;
	goto failure;
    }
    
    /* make peer address */
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    peer_addr.sin_addr = * (struct in_addr*) hp->h_addr_list[0];
    memset(&peer_addr.sin_zero, 0, sizeof(peer_addr.sin_zero));

    /* create socket */
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	e = APR_FROM_OS_ERROR(errno);
	goto failure;
    }

    if (0 != (e = set_sockopt_nonblock(sock))) 
	goto failure;

    /* find a free slot */
    spin = gpcc_spin_secure(&net.conntab_spin);
    unique = net.unique++;
    if (unique == 0) unique = net.unique++;
    pconn = 0;
    for (i = 0; i < CONN_MAX; i++) {
	int index = (unique + i) % CONN_MAX;
	if (0 == net.conntab[index].conn) {
	    pconn = &net.conntab[index];
	    pconn->conn = CONN_DEFINE(unique, index);
	    break;
	}
    }
    if (!pconn) {
	gpcc_warning(FLINE, "unable to open connection - connection table full");
	goto failure;
    }

    /* crab over to pconn */
    gpcc_spin_secure(&pconn->spin);
    gpcc_spin_release(spin);
    spin = &pconn->spin;

    pconn->pool = pool;
    pconn->sock = sock;
    memset(&pconn->to_peer, 0, sizeof(pconn->to_peer));
    memset(&pconn->from_peer, 0, sizeof(pconn->from_peer));
    pconn->to_peer.buf = apr_palloc(pool, sizeof(buf_t));
    pconn->from_peer.buf = apr_palloc(pool, sizeof(buf_t));
    pconn->do_read_header_attempts = 0;
    pconn->to_peer.hdr = hdr;

    /* connect */
    if (-1 == connect(sock, (struct sockaddr *) &peer_addr, sizeof(peer_addr))) {
	if (e != EINPROGRESS) {
	    e = APR_FROM_OS_ERROR(errno);
	    goto failure;
	}
    }
    if (sizeof(conn) != write(net.send_break_fd, &conn, sizeof(conn)))  {
	gpcc_warning(FLINE, "unable to send conn to break event loop");
	goto failure;
    }

    gpcc_spin_release(spin);
    return 0;

failure:
    if (pconn)
	pconn->conn = 0;
    if (spin)
	gpcc_spin_release(spin);
    if (pool) 
	apr_pool_destroy(pool);
    if (sock >= 0) {
	shutdown(sock, SHUT_WR | SHUT_RD);
	closesocket(sock);
    }
    return e;
}

APR_DECLARE(void) gpcc_net_close(gpcc_net_conn_t conn)
{
    conncb_t* pconn = 0;
    apr_uint32_t* spin = 0;
    if (! (spin = conn_secure(conn, &pconn)))
	return;
    spin = gpcc_spin_release(spin);
    zap(pconn, conn);
    return;
}



    
APR_DECLARE(apr_status_t) gpcc_net_parse_request(gpcc_net_header_t** hdr, const char* buf, int* len, apr_pool_t* pool)
{
    int n = *len;
    const char* p;
    char* line;
    char* last;
    char* colon;
    int empty, completed;
    char** argv = 0;
    int e;
    gpcc_net_header_t* req = 0;

    *hdr = 0;

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
    req = apr_pcalloc(pool, sizeof(gpcc_net_header_t));

    /* for first line */
    line = apr_strtok(line, "\n", &last);
    line = gpcc_string_trim(line);
    if (0 != (e = apr_tokenize_to_argv(line, &argv, pool)))
	return e;

    for ( ;argv[req->argc]; req->argc++)  {
	if (req->argc >= GPCC_NET_HEADER_ENTRYMAX)
	    return APR_EINVAL;
	req->argv[req->argc] = argv[req->argc];
    }

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
	req->hname[req->hc] = gpcc_string_tolower(line);
	req->hvalue[req->hc] = gpcc_string_trim(colon);
	req->hc++;

	if (req->hc >= GPCC_NET_HEADER_ENTRYMAX)
	    return APR_EINVAL;
    }

    *hdr = req;
    return 0;
}

