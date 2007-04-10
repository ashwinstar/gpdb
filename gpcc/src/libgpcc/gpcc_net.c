#include <gpcc_net.h>
#include <gpcc_spin.h>
#include <apr_network_io.h>
#include <apr_thread_proc.h>
#include <apr_poll.h>
#include <apr_sha1.h>


#define MAGIC 0x2f39bc2a
#define ITEM_MAX 256


static unsigned short unique_gen = 1;

typedef struct pkthdr_t pkthdr_t;
struct pkthdr_t {
    short pkttype, pktlen;
    long  cmdidx;
};


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
    int is_connecting;
    int is_closing;
    buf_t in, out;
};



static struct {
    apr_uint32_t net_spin[1];
    gpcc_net_node_id_t my_node_id;
    apr_pool_t* pool;
    apr_thread_t* thread;
    apr_sockaddr_t* sa;
    gpcc_net_tick_t* tick;
    gpcc_net_opened_t* opened;
    gpcc_net_closed_t* closed;
    gpcc_net_recv_t* recv;
    apr_socket_t* listen_sock;
    struct {
	int top;
	item_t* item[ITEM_MAX];
    } tab;
} net = {{0}};



static item_t* getitem(gpcc_net_conn_t conn) 
{
    const int idx = conn & 0xffff;
    if (0 <= idx && idx < ITEM_MAX) {
	item_t* item = net.tab.item[idx];
	if (item && item->conn == conn)
	    return item;
    }
    return 0;
}


static void mkfd(apr_pollfd_t* fd, apr_socket_t* sock, int idx, int flag)
{
    fd->p = net.pool;
    fd->desc_type = APR_POLL_SOCKET;
    fd->reqevents = flag | APR_POLLERR | APR_POLLHUP | APR_POLLNVAL;
    fd->desc.s = sock;
    fd->client_data = (void*) idx;
}


static apr_status_t mkitem(item_t** pitem, apr_socket_t* sock, void* arg, apr_pool_t* pool)
{
    apr_uint32_t* spin;
    int i, e;
    item_t* item;

    if (0 != (e = apr_socket_opt_set(sock, APR_SO_NONBLOCK, 1)))
	return e;
    
    if (0 != (e = apr_socket_timeout_set(sock, 0)))
	return e;
    
    spin = gpcc_spin_secure(net.net_spin);
    for (i = 0; i < ITEM_MAX && net.tab.item[i]; i++);
    if (i >= ITEM_MAX) {
	spin = gpcc_spin_release(spin);
	return APR_EBUSY;
    }
    if (i >= net.tab.top) 
	net.tab.top = i+1;
    item = net.tab.item[i] = apr_palloc(pool, sizeof(*item));
    item->conn = (((int)unique_gen++) << 16) | i;
    item->sock = sock;
    item->arg = arg;
    item->pool = pool;
    item->is_connecting = 1;
    item->is_closing = 0;
    item->in.bot = item->in.top = 0;
    item->out.bot = item->out.top = 0;
    *pitem = item;
    spin = gpcc_spin_release(net.net_spin);
    return 0;
}


static void do_read(item_t* item)
{
    buf_t* bb = &item->in;
    int avail = sizeof(bb->buf) - bb->top;
    apr_size_t bcnt;
    int e = 0;
    pkthdr_t pkthdr;
    int pkttype, pktlen, cmdidx;

    if (avail < sizeof(pkthdr) + 16) {
	/* compact the buffer */
	memmove(bb->buf, bb->buf + bb->bot, bb->top - bb->bot);
	bb->top = bb->top - bb->bot;
	bb->bot = 0;
	avail = sizeof(bb->buf) - bb->top;
    }

    /* drain the network to fill buffer */
    if (avail > 0) {
	bcnt = avail;
	e = apr_socket_recv(item->sock, bb->buf + bb->top, &bcnt);
	bb->top += bcnt;
	avail -= bcnt;
    }

    /* if we have more bytes than pkthdr in the buffer, 
       then peek to check packet size */
    if (bb->top - bb->bot >= sizeof(pkthdr)) {
	memcpy(&pkthdr, bb->buf + bb->bot, sizeof(pkthdr));
	pkttype = ntohs(pkthdr.pkttype);
	pktlen = ntohs(pkthdr.pktlen);
	cmdidx = ntohl(pkthdr.cmdidx);
	/* if the packet (pktlen+pkthdr) is bigger than our buffer size, 
	   there is no way we will ever be able to read it. */
	if (sizeof(pkthdr) + pktlen > sizeof(bb->buf)) {
	    item->is_closing = 1;    /* too big or corrupted packet */
	    return;
	}
	/* if the packet fits, consume it along with pkthdr */
	if (bb->top - bb->bot >= sizeof(pkthdr) + pktlen) {
	    bb->bot += sizeof(pkthdr);
	    net.recv(item->conn, item->arg,
		     pkttype, cmdidx, bb->buf + bb->bot, pktlen);
	    bb->bot += pktlen;
	}
    }
    if (e) {
	if (APR_STATUS_IS_EAGAIN(e))
	    ;
	else if (APR_STATUS_IS_EOF(e))
	    item->is_closing = 1;
	else {
	    gpcc_warning(FLINE, "net: recv error %d\n", e);
	    item->is_closing = 1;
	}
    }
}


static void do_write(item_t* item)
{
    buf_t* bb = &item->out;
    apr_size_t n = bb->top - bb->bot;
    int e = 0;
    
    if (n > 0) {
	e = apr_socket_send(item->sock, bb->buf + bb->bot, &n);
	bb->bot += n;
	if (e) {
	    if (APR_STATUS_IS_EAGAIN(e)) 
		;
	    else if (APR_STATUS_IS_EOF(e))
		item->is_closing = 1;
	    else {
		gpcc_warning(FLINE, "net: send error %d\n", e);
		item->is_closing = 1;
	    }
	}
    }
}


    
static void* APR_THREAD_FUNC net_run(apr_thread_t* thread, void* arg)
{
    apr_pollfd_t fd[2 + ITEM_MAX];
    apr_time_t now = apr_time_now();
    apr_time_t exp = now + 1000*1000;
    item_t* item;
    apr_uint32_t* spin = 0;
    int fdtop, nsds, i, e;

    while (net.thread == thread) {
	fdtop = nsds = 0;

	mkfd(&fd[fdtop++], net.listen_sock, -1, APR_POLLIN);
	
	/* go thru items -
	   - if closing, close it
	   - otherwise, add to fd[] pollset */
	spin = gpcc_spin_secure(net.net_spin);
	for (i = 0; i < ITEM_MAX; i++) {
	    item = net.tab.item[i];
	    if (!item) continue;
	    if (item->is_closing) {
		spin = gpcc_spin_release(spin);
		net.closed(item->conn, item->arg);
		spin = gpcc_spin_secure(net.net_spin);
		apr_pool_destroy(item->pool);
		net.tab.item[i] = 0;
		continue;
	    }
	    mkfd(&fd[fdtop++], item->sock, i, item->out.bot == item->out.top ? 0 : APR_POLLOUT);
	}
	spin = gpcc_spin_release(net.net_spin);

	/* check tick */
	now = apr_time_now();
	if (exp <= now) {
	    net.tick(now);
	    exp = now + 1000*1000;
	}
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
		    if (0 != (e = apr_pool_create(&pool, net.pool))) {
			gpcc_warning(FLINE, "net_run: out of memory - reject new connection\n");
			continue;
		    }
		    if (0 != (e = apr_socket_accept(&sock, net.listen_sock, pool))) {
			gpcc_warning(FLINE, "net_run: apr_socket_accept error %d\n", e);
			apr_pool_destroy(pool);
			continue;
		    }
		    if (0 != (e = mkitem(&item, sock, arg, pool))) {
			gpcc_warning(FLINE, 
				     "net_run: unable to accept new connection, error %d\n", e);
			apr_pool_destroy(pool);
			continue;
		    }
		    item->arg = net.opened(item->conn, item->arg);
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
		if (rtnevents & APR_POLLIN) {
		    if (item->is_connecting) {
			spin = gpcc_spin_release(spin);
			item->arg = net.opened(item->conn, item->arg);
			spin = gpcc_spin_secure(net.net_spin);
		    }
		    do_read(item);
		}
		if (rtnevents & APR_POLLOUT) {
		    do_write(item);
		}
	    }
	    spin = gpcc_spin_release(spin);
	}
    }
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_net_start(int port, 
					  gpcc_net_opened_t* opened,
					  gpcc_net_closed_t* closed,
					  gpcc_net_recv_t* recv,
					  gpcc_net_tick_t* tick)
{
    int e = 0;
    apr_sockaddr_t* sa;
    apr_socket_t* sock;
    apr_threadattr_t* ta;
    apr_uint32_t* spin = 0;
    apr_sha1_ctx_t sha1ctx;
    char sha1buf[80];

    if (port <= 0) return APR_EINVAL;

    spin = gpcc_spin_secure(net.net_spin);
    if (net.pool) {
	spin = gpcc_spin_release(spin);
	return 0;
    }
    GPCC_DO(0 == apr_generate_random_bytes((unsigned char*)sha1buf, 20));
    sprintf(sha1buf + 20, " gpcc %lld ", apr_time_now());
    apr_sha1_init(&sha1ctx);
    apr_sha1_update(&sha1ctx, sha1buf, strlen(sha1buf));
    apr_sha1_final(net.my_node_id.opaque, &sha1ctx);
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
    net.opened = opened;
    net.closed = closed;
    net.recv = recv;
    net.tick = tick;
    spin = gpcc_spin_release(spin);

    GPCC_DO(0 == apr_threadattr_create(&ta, net.pool));
    GPCC_DO(0 == apr_threadattr_detach_set(ta, 0));
    GPCC_DO(0 == apr_thread_create(&net.thread, ta, net_run, &net, net.pool));
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
    item_t* item;

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

    if (0 != (e = mkitem(&item, sock, arg, pool))) 
	goto bail;

    *pconn = item->conn;

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
    int e = 0;

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	e = APR_EINVAL;
	goto bail;
    }
    item->is_closing = 1;
    spin = gpcc_spin_release(spin);
    return 0;

    bail:
    if (spin) gpcc_spin_release(spin);
    return e;
}


APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					int pkttype, int cmdidx,
					const char* pkt, int pktlen)
{
    apr_uint32_t* spin;
    int e = 0;
    pkthdr_t pkthdr;
    item_t* item;

    retry:
    pkthdr.pkttype = htons((short) pkttype);
    pkthdr.pktlen = htons((short) pktlen);
    pkthdr.cmdidx = htonl(cmdidx);

    spin = gpcc_spin_secure(net.net_spin);
    if (! (item = getitem(conn))) {
	e = APR_EINVAL;
	goto bail;
    }

    /* need to compact out buf? */
    if (sizeof(item->out.buf) - item->out.top < sizeof(pkthdr) + pktlen) {
	int n = item->out.top - item->out.bot;
	if (n > 0) {
	    memmove(item->out.buf, item->out.buf + item->out.bot, n);
	    item->out.top = n;
	    item->out.bot = 0;
	}
	if (sizeof(item->out.buf) - item->out.top < sizeof(pkthdr) + pktlen) {
	    if (n == 0) {	/* packet too big */
		e = APR_EINVAL;
		goto bail;
	    }
	    spin = gpcc_spin_release(spin);
	    apr_sleep(100);
	    goto retry;
	}
    }
    memcpy(item->out.buf + item->out.top, &pkthdr, sizeof(pkthdr));
    item->out.top += sizeof(pkthdr);
    memcpy(item->out.buf + item->out.top, pkt, pktlen);
    item->out.top += pktlen;
    spin = gpcc_spin_release(spin);
    return 0;

    bail:
    if (spin) spin = gpcc_spin_release(spin);
    return e;
}


APR_DECLARE(gpcc_net_node_id_t) gpcc_net_my_node_id(void)
{
    return net.my_node_id;
}

