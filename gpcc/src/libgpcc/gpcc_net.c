#include <gpcc_net.h>
#include <gpcc_spin.h>
#include <apr_network_io.h>
#include <apr_thread_proc.h>
#include <apr_poll.h>
#include <apr_strings.h>
#include <gpcc_strings.h>
#include <stdlib.h>
#include <apr_sha1.h>


static unsigned short unique_gen = 1;


typedef struct buf_t buf_t;
struct buf_t {
    char buf[1024*64];
    int bot, top;
};


typedef struct item_t item_t;
struct item_t {
    gpcc_net_conn_t conn;
    apr_socket_t*   sock;
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
    apr_sockaddr_t* sa;
    apr_socket_t* listen_sock;
    apr_time_t exp;		/* time up expiration */
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


static void mkfd(apr_pollfd_t* fd, apr_socket_t* sock, intptr_t idx, int flag)
{
    fd->p = net.pool;
    fd->desc_type = APR_POLL_SOCKET;
    fd->reqevents = flag | APR_POLLERR | APR_POLLHUP | APR_POLLNVAL;
    fd->desc.s = sock;
    fd->client_data = (void*) idx;
}


static gpcc_net_conn_t mkitem(apr_socket_t* sock, void* arg, 
			      int is_connecting, 
			      int is_open,
			      int self_origin,
			      apr_pool_t* pool)
{
    apr_uint32_t* spin;
    int i, e;
    item_t* item;
    gpcc_net_conn_t conn;

    if (0 != (e = apr_socket_opt_set(sock, APR_SO_NONBLOCK, 1)))
	return 0;
    
    if (0 != (e = apr_socket_opt_set(sock, APR_SO_KEEPALIVE, 1)))
	return 0;
    
    if (0 != (e = apr_socket_timeout_set(sock, 0)))
	return 0;
    
    spin = gpcc_spin_secure(net.net_spin);
    for (i = 0; i < GPCC_NET_CONN_MAX && net.tab.item[i]; i++);
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
    conn = item->conn;
    spin = gpcc_spin_release(net.net_spin);
    return conn;
}




static apr_status_t do_read(item_t* item, apr_uint32_t* spin)
{
    buf_t* bb = &item->in;
    int e = 0;
    int bbfree = sizeof(bb->buf) - bb->top; /* freespace in buf */
    
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
	e = apr_socket_recv(item->sock, bb->buf + bb->top, &n);
	bb->top += n;
	bbfree -= n;
	if (e) {
	    if (e == APR_EOF) 
		return APR_EINVAL;
	    if (e != APR_EAGAIN) {
		gpcc_warning(FLINE, "net: recv error %d\n", e);
		return APR_EINVAL;
	    }
	}
    }

    if (item->is_connecting) {
	item->is_connecting = 0;
	item->is_open = 1;
	gpcc_spin_release(spin);
	item->arg = net.cb.opened(item->conn, item->arg, 
				  item->self_origin, item->pool);
	gpcc_spin_secure(spin);
	if (item->is_closing) 
	    return APR_EINVAL;
    }

    if (bb->bot < bb->top) {
	gpcc_spin_release(spin);
	net.cb.recv(item->conn, item->arg);
	gpcc_spin_secure(spin);
	if (item->is_closing)
	    return APR_EINVAL;
    }
    return 0;
}





static apr_status_t do_write(item_t* item)
{
    buf_t* bb = &item->out;
    apr_size_t n = bb->top - bb->bot;
    int e = 0;
    
    if (n > 0) {
	e = apr_socket_send(item->sock, bb->buf + bb->bot, &n);
	bb->bot += n;
	if (e) {
	    if (APR_STATUS_IS_EAGAIN(e)) 
		return 0;

	    if (APR_STATUS_IS_EOF(e))
		return APR_EINVAL;

	    gpcc_warning(FLINE, "net: send error %d\n", e);
	    return APR_EINVAL;
	}
    }
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_once(void)
{
    apr_pollfd_t fd[2 + GPCC_NET_CONN_MAX];
    item_t* item;
    apr_uint32_t* spin = 0;
    int fdtop, nsds, i, e;
    int count = 0;

    while (count == 0) {
	apr_time_t now = apr_time_now();
	if (net.exp <= now) {
	    net.cb.tick(now);
	    count++;
	    net.exp = now + 1000*1000;
	    continue;
	}

	fdtop = nsds = 0;
	mkfd(&fd[fdtop++], net.listen_sock, -1, APR_POLLIN);
	
	/* go thru items -
	   - if closing, close it
	   - otherwise, add to fd[] pollset */
	spin = gpcc_spin_secure(net.net_spin);
	for (i = 0; i < GPCC_NET_CONN_MAX; i++) {
	    int flag = 0;
	    item = net.tab.item[i];
	    if (!item) continue;
	    if (item->is_closing) {
		gpcc_spin_release(spin);
		net.cb.closed(item->conn, item->arg);
		count++;
		gpcc_spin_secure(spin);
		apr_pool_destroy(item->pool);
		net.tab.item[i] = 0;
		continue;
	    }
	    if (item->in.top == item->in.bot + sizeof(item->in.buf))
		flag |= APR_POLLIN;    /* inbuf not full, we can read some more */
	    if (item->out.top != item->out.bot) 
		flag |= APR_POLLOUT;   /* outbuf not empty, we can write */
	    mkfd(&fd[fdtop++], item->sock, i, flag);
	}
	spin = gpcc_spin_release(spin);

	if (fdtop == 0) {
	    apr_sleep(100 * 1000); /* 0.1 second */
	    continue;
	}
	
	/* poll */
	if (0 != (e = apr_poll(fd, fdtop, &nsds, 100 * 1000))) {
	    if (e == APR_TIMEUP || e == APR_EINTR) continue;
	    gpcc_warning(FLINE, "apr_poll error %d\n", e);
	    continue;
	}
	
	/* process fd[] */
	for (i = 0; i < fdtop && nsds; i++) {
	    const int rtnevents = fd[i].rtnevents;
	    if (!rtnevents) continue;
	    nsds--;
	    if (i == 0) {
		/* accept */
		if (rtnevents & APR_POLLIN) {
		    apr_pool_t* pool;
		    apr_socket_t* sock;
		    gpcc_net_conn_t conn;
		    if (0 != (e = apr_pool_create(&pool, net.pool))) {
			gpcc_warning(FLINE, "net_run: out of memory - reject new connection\n");
			continue;
		    }
		    if (0 != (e = apr_socket_accept(&sock, net.listen_sock, pool))) {
			gpcc_warning(FLINE, "net_run: apr_socket_accept error %d\n", e);
			apr_pool_destroy(pool);
			continue;
		    }
		    conn = mkitem(sock, 0 /* arg */,
				  0 /*is_connecting*/, 
				  1 /*is_open*/, 
				  0 /*self_origin*/, 
				  pool);
		    if (! conn) {
			gpcc_warning(FLINE, 
				     "net_run: unable to accept new connection, error %d\n", e);
			apr_pool_destroy(pool);
			continue;
		    }
		    spin = gpcc_spin_secure(net.net_spin);
		    item = getitem(conn);
		    if (item) {
			gpcc_spin_release(spin);
			item->arg = net.cb.opened(item->conn, item->arg, 
						  item->self_origin, item->pool);
			count++;
			gpcc_spin_secure(spin);
		    }
		    spin = gpcc_spin_release(spin);
		    continue;
		}
		gpcc_warning(FLINE, "net_run: listen_socket closed\n");
		break;
	    }

	    spin = gpcc_spin_secure(net.net_spin);
	    item = net.tab.item[(int)fd[i].client_data];
	    GPCC_DO(item && item->sock == fd[i].desc.s);
	    if (rtnevents & (APR_POLLERR | APR_POLLHUP | APR_POLLNVAL)) 
		item->is_closing = 1;
	    else {
		if ((rtnevents & APR_POLLIN) && ! item->is_closing) {
		    if (do_read(item, spin)) {
			item->is_closing = 1;
		    }
		    count++;
		}
		if ((rtnevents & APR_POLLOUT) && ! item->is_closing) {
		    if (do_write(item)) {
			item->is_closing = 1;
		    }
		    count++;
		}
	    }
	    spin = gpcc_spin_release(spin);
	}
    }
    return 0;
}



    
static void* APR_THREAD_FUNC net_run(apr_thread_t* thread, void* arg)
{
    while (net.thread == thread) {
	if (gpcc_net_once())
	    break;
    }
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_start(int port, 
					 gpcc_net_opened_t* opened,
					 gpcc_net_closed_t* closed,
					 gpcc_net_recv_t* recv,
					 gpcc_net_tick_t* tick,
					 int threaded)
{
    int e = 0;
    apr_sockaddr_t* sa;
    apr_socket_t* sock;
    apr_threadattr_t* ta;
    apr_uint32_t* spin = 0;

    if (port <= 0) return APR_EINVAL;

    spin = gpcc_spin_secure(net.net_spin);
    if (net.pool) {
	spin = gpcc_spin_release(spin);
	return 0;
    }
    GPCC_DO(0 == apr_pool_create(&net.pool, 0));
    GPCC_DO(0 == apr_sockaddr_info_get(&sa, APR_ANYADDR, APR_INET, 
				       (apr_port_t) port, 0, net.pool));
    if (0 != apr_socket_create(&sock, sa->family, SOCK_STREAM, APR_PROTO_TCP,
			       net.pool))
	goto bail;
    
    if (0 != apr_socket_bind(sock, sa))
	goto bail;

    if (0 != apr_socket_listen(sock, 5))
	goto bail;

    net.listen_sock = sock;
    net.cb.opened = opened;
    net.cb.closed = closed;
    net.cb.recv = recv;
    net.cb.tick = tick;
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
    if (thread) {
	net.thread = 0;
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
    apr_socket_t* sock;

    spin = gpcc_spin_secure(net.net_spin);
    if (0 != (e = apr_pool_create(&pool, net.pool)))
	goto bail;
    spin = gpcc_spin_release(spin);

    /* resolve addr */
    if (0 != (e = apr_parse_addr_port(&addr2, &scope, &port, addr, pool)))
	goto bail;

    if (0 != (e = apr_sockaddr_info_get(&sa, addr2, APR_INET, port, 0, pool)))
	goto bail;

    if (0 != (e = apr_socket_create(&sock, sa->family, SOCK_STREAM, APR_PROTO_TCP, pool)))
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
		goto bail;
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
    
    /* we have a complete HTTP-style request (terminated by empty line) */
    *len = n = p - buf; /* consume it */
    line = apr_pstrndup(pool, buf, n); /* dup it */
    req = apr_pcalloc(pool, sizeof(gpcc_net_request_t));

    /* for first line */
    line = apr_strtok(line, "\n", &last);
    line = gpcc_string_trim(line);
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

