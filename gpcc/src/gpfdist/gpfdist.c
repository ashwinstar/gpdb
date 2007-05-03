#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <gpcc.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <gpcc_strings.h>
#include <apr_hash.h>
#include <apr_file_info.h>
#include <apr_time.h>
#include <event.h>
#include <gpcc_net.h>

#define BLOCKSIZE  (1024 * 32)


struct {
    int p;			/* port */
    int v;			/* verbose */
    int h;			/* skip first-row header */
    const char* d;		/* directory */
    const char* q;		/* quote char */
    const char* x;		/* escape char */
} opt = { 8080, 0, 0, ".", 0, 0 };

typedef struct block_t block_t;
struct block_t {
    int      bot, top;
    block_t* __next;
    char     byte[BLOCKSIZE - 16];
};

struct {
    apr_pool_t* pool;
    int listen_sock;
    struct event listen_event;

    block_t* freelist;
    int      freecnt;
    
    struct {
	int gen;
	apr_hash_t* tab;
    } session;
} gcb = {0};



typedef struct session_t session_t;
struct session_t {
    const char* key;
    const char* tid;
    const char* path;
    int ffd;			/* file input */
    apr_uint64_t fsize, foff;
    int is_error;

    int nrequest;		/* # requests attached to this session */
    apr_time_t mtime;		/* time when nrequest was modified */

    block_t* residue_block;
};


typedef struct request_t request_t;
struct request_t {
    int sock;
    apr_pool_t* pool;
    struct event ev;
    const char* peer;
    const char* path;
    const char* tid;
    session_t* session;

    struct {
	char* hbuf;
	int   hbuftop, hbufmax;
	gpcc_net_request_t* req;
    } in;

    struct {
	block_t* block;
    } out;
};


block_t* block_get()
{
    block_t* b;
    if (0 != (b = gcb.freelist)) {
	gcb.freelist = b->__next;
	gcb.freecnt--;
    }
    else {
	if (! (b = malloc(sizeof(block_t)))) 
	    gpcc_fatal(FLINE, "out of memory");
    }

    b->bot = b->top = 0;
    b->__next = 0;
    return b;
}


void block_put(block_t* b)
{
    b->__next = gcb.freelist;
    gcb.freelist = b;
    gcb.freecnt++;
}


void usage(const char* msg)
{
    fprintf(stderr, "gpfdist -- file distribution web server\n\n");
    fprintf(stderr, "usage: gpfdist [-?v] [-p port] [-d dir] [-q qchar] [-x xchar] [-h]\n\n");
    fprintf(stderr, 
	    "        -?       : print this screen\n"
	    "        -v       : verbose mode\n"
	    "        -p port  : which port to serve HTTP. Default to 8080.\n"
	    "        -d dir   : serve files under the specified directory.\n"
	    "                   Default to '.'\n"
	    "        -q qchar : set quote character. If not specified, the\n"
	    "                   program will not parse for CSV, and will\n"
	    "                   assume each record is separated by newline\n"
	    "                   character that never occurs inside a record.\n"
	    "        -x xchar : set escape character (default to qchar)\n"
	    "        -h       : flag indicates first data row in all files is\n"
	    "                   a header row. skip it.\n\n");
    if (msg)
	fprintf(stderr, "%s\n", msg);

    exit(msg ? 1 : 0);
}


void parse_command_line(int argc, const char* const argv[], apr_pool_t* pool)
{
    apr_getopt_t* os;
    char ch;
    const char* arg;
    int e;
    
    if (0 != apr_getopt_init(&os, pool, argc, argv)) 
	gpcc_fatal(FLINE, "apt_getopt_init failed");

    while (0 == (e = apr_getopt(os, "?vp:d:q:x:h", &ch, &arg))) {
	switch (ch) {
	case '?': usage(0); break;
	case 'v': opt.v = 1; break;
	case 'h': opt.h = 1; break;
	case 'd': opt.d = arg; break;
	case 'q': opt.q = arg; break;
	case 'x': opt.x = arg; break;
	case 'p': opt.p = atoi(arg); break;
	}
    }
    if (e != APR_EOF) 
	usage("Error: illegal arguments"); 

    if (! (0 < opt.p && opt.p < (1 << 16)))
	usage("Error: please specify a valid port number for -p switch");


    /* validate opt.d */
    {
	char* p = gpcc_string_trim(apr_pstrdup(pool, opt.d));

	/* disallow / */
	if (0 == strcmp(p, "/")) 
	    usage("Error: cannot run under root (/) directory");

	/* strip ending / */
	while (p[0] && p[strlen(p) - 1] == '/')
	    p[strlen(p) - 1] = 0;

	opt.d = p;
    }


    /* check that the dir exists */
    if (0 != apr_filepath_set(opt.d, pool))
	usage("Error: please specify a valid directory for -d switch");

    if (opt.q && ! (opt.q[0] && !opt.q[1])) 
	usage("Error: please specify a character for -q switch");

    if (opt.x && ! (opt.x[0] && !opt.x[1]))
	usage("Error: please specify a character for -x switch");

    if (!opt.q && opt.x)
	usage("Error: you must specify -q qchar with -x xchar");

    if (opt.q && !opt.x)
	opt.x = opt.q;

    if (os->ind != argc) 
	usage("Error: illegal arguments");
}



int setup_read(request_t* r);
int setup_write(request_t* r);
void session_attach(request_t* r);
void session_detach(request_t* r);
void session_end(session_t* s, int error);
void request_end(request_t* r, int error);
block_t* session_get_block(session_t* s);

void send_error(request_t* r, int code, const char* msg)
{
    char buf[1024];
    int n;
    printf("%s - %d %s\n", r->peer, code, msg);
    n = apr_snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n"
		     "Connection: close\r\n\r\n",
		     code, msg);

    write(r->sock, buf, n);
}


void send_empty(request_t* r)
{
    const char* buf = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Content-length: 0\r\n"
	"Connection: close\r\n\r\n";
    static int n = 0;
    int m;

    printf("%s - 200 empty\n", r->peer);

    if (n == 0) 
	n = strlen(buf);
    m = write(r->sock, buf, n);
    if (m != n) 
	gpcc_fatal(FLINE, "assert failed");
}


void send_ok(request_t* r)
{
    const char* buf = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Connection: close\r\n\r\n";
    static int n = 0;
    static int m;

    printf("%s - 200 ok\n", r->peer);

    if (n == 0) 
	n = strlen(buf);
    m = write(r->sock, buf, n);
    if (m != n) 
	gpcc_fatal(FLINE, "assert failed");
}


void print_request(gpcc_net_request_t* r)
{
    int i;
    for (i = 0; i < r->argc; i++) {
        printf("  arg[%d] = %s\n", i, r->argv[i]);
    }

    for (i = 0; i < r->hc; i++) {
        printf("  hdr[%s] = %s\n", r->hname[i], r->hvalue[i]);
    }
}


void request_end(request_t* r, int error) 
{
    session_t* s = r->session;
    if (r->out.block) {
	gpcc_warning(FLINE, "request failure resulting in session failure");
	if (s) 
	    session_end(s, 1);
	block_put(r->out.block);
	r->out.block = 0;
    }
    else {
	session_detach(r);
    }
    event_del(&r->ev);
    close(r->sock);
    apr_pool_destroy(r->pool);
}




/* split the block at byte n (relative to bot) */
block_t* block_split(block_t* b, int n)
{
    block_t* bb;
    int m = b->top - b->bot - n;
    if (m < 0)
	gpcc_fatal(FLINE, "assert failed, n = %d, bot %d, top %d", n, b->bot, b->top);

    if (m == 0)
	return 0;
    
    bb = block_get();
    memcpy(bb->byte, b->byte + b->bot + n, m);
    b->top = b->bot + n;
    bb->top = m;
    return bb;
}

const char* scan_rec_boundary(const char* p, const char* q)
{
    int in_quote, last_was_esc;
    const char* last_complete = 0;
    int xc = opt.x[0];
    int qc = opt.q[0];

    again:
    in_quote = 0;
    last_was_esc = 0;
    
    while (p < q) {
	int ch = *p++;
	if (ch == '\n' && !in_quote) {
	    last_complete = p;
	    goto again;
	}
	if (in_quote && ch == xc) 
	    last_was_esc = !last_was_esc;
	if (ch == qc && !last_was_esc)
	    in_quote = !in_quote;
	if (ch != xc)
	    last_was_esc = 0;
    }

    return last_complete;
}



block_t* session_get_block(session_t* s)
{
    block_t* b;
    int n;
    char* p;
    char* q;


    if (s->is_error || s->ffd < 0) 
	return 0;

    if (0 != (b = s->residue_block)) {
	s->residue_block = 0;
    }
    else {
	b = block_get();
    }

    n = sizeof(b->byte) - b->top;
    while (n > 0) {
	n = read(s->ffd, b->byte + b->top, n);
	if (n < 0) {
	    session_end(s, 1);
	    return 0;
	}
	if (n == 0) {		/* EOF */
	    session_end(s, 0);
	    return b;
	}
	b->top += n;
	s->foff += n;
	n = sizeof(b->byte) - b->top;
	if (opt.v) 
	    printf("[INFO] %s [%lld/%lld]\n", s->path, s->foff, s->fsize);
    }

    /* find the split position */
    if (opt.q) {
	p = b->byte + b->bot;
	q = b->byte + b->top;
	p = (char*) scan_rec_boundary(p, q);
    }
    else {
	/* find the last \n */
	p = b->byte + b->bot;
	q = b->byte + b->top - 1;
	for ( ; p < q && *q != '\n'; q--);
	p = (p < q ? q + 1: 0);
    }

    if (!p) {
	gpcc_warning(FLINE, "ERROR - %s - line too long", s->path);
	session_end(s, 1);
	return 0;
    }

    s->residue_block = block_split(b, p - b->byte + b->bot);
    return b;
}


void session_end(session_t* s, int error)
{
    if (error)
	s->is_error = error;
    if (s->residue_block) {
	block_put(s->residue_block);
	s->residue_block = 0;
    }
    if (s->ffd >= 0) {
	close(s->ffd);
	s->ffd = -1;
    }
}




void session_attach(request_t* req)
{
    char key[1024];
    session_t* s = 0;
    int fd = -1;
    off_t fsize;
    

    if (sizeof(key) -1 == apr_snprintf(key, sizeof(key), "%s:%s", req->tid, req->path)) {
	send_error(req, 400, "path too long");
	request_end(req, 1);
	return;
    }

    s = apr_hash_get(gcb.session.tab, key, APR_HASH_KEY_STRING);
    if (s) {
	if (s->is_error) {
	    send_error(req, 500, "session error");
	    request_end(req, 1);
	    return;
	}

	if (s->ffd < 0 || s->foff >= s->fsize) {
	    send_empty(req);
	    request_end(req, 0);
	    return;
	}

	if (opt.v) 
	    printf("[INFO] joined session (%s, %s)\n", s->path, s->tid);
    }
    else {
	/* not in hashtable - create new session */
	if (-1 == (fd = open(req->path, O_RDONLY, 0600))) {
	    gpcc_warning(FLINE, "reject request from %s, path %s\n", req->peer, req->path);
	    send_error(req, 404, "not found");
	    request_end(req, 1);
	    return;
	}

	if (-1 == (fsize = lseek(fd, 0, SEEK_END))
	    || -1 == lseek(fd, 0, SEEK_SET)) {
	    gpcc_warning(FLINE, "reject request from %s, path %s\n", req->peer, req->path);
	    send_error(req, 500, "internal error");
	    request_end(req, 1);
	    return;
	}

	if (0 == (s = malloc(sizeof(session_t)))) 
	    gpcc_fatal(FLINE, "out of memory");

	memset(s, 0, sizeof(*s));
	s->ffd = fd;
	s->fsize = fsize;

	if (0 == (s->tid = strdup(req->tid)) 
	    || 0 == (s->path = strdup(req->path))
	    || 0 == (s->key = strdup(key))) {
	    gpcc_fatal(FLINE, "out of memory");
	}

	apr_hash_set(gcb.session.tab, s->key, APR_HASH_KEY_STRING, s);

	if (opt.v) 
	    printf("[INFO] new session (%s, %s)\n", s->path, s->tid);
    }

    if (setup_write(req)) {
	send_error(req, 500, "internal error");
	request_end(req, 1);
	return;
    }
    s->nrequest++;
    s->mtime = apr_time_now();
    req->session = s;
    send_ok(req);
    return;
}

void session_free(session_t* s)
{
    if (opt.v) 
	printf("[INFO] free session %s\n", s->key);

    apr_hash_set(gcb.session.tab, s->key, APR_HASH_KEY_STRING, 0);
    free((void*) s->key);
    free((void*) s->tid);
    free((void*) s->path);
    free(s);
}


void session_detach(request_t* r)
{
    session_t* s = r->session;
    r->session = 0;
    if (s) {
	if (s->nrequest == 0) 
	    gpcc_fatal(FLINE, "assert failed");
	s->nrequest--;
	s->mtime = apr_time_now();

	if (0 == strncmp("auto-tid.", s->tid, 9)) {
	    if (s->nrequest != 0) 
		gpcc_fatal(FLINE, "assert failed");

	    session_free(s);
	}
    }
}




void do_write(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n;
    block_t* b;

    if (fd != r->sock) 
	gpcc_fatal(FLINE, "assert failed");

    for (;;) {
	/* get a block (or find a remaining block) */
	if (! r->out.block) {
	    r->out.block = session_get_block(r->session);
	    if (! r->out.block) {
		request_end(r, 0);
		return;
	    }
	}

	/* write out the block */
	b = r->out.block;
	n = b->top - b->bot;
	n = write(fd, b->byte + b->bot, n);
	if (n < 0) {
	    if (errno != EINTR && errno != EAGAIN) {
		perror("write");
		request_end(r, 1);
		return;
	    }
	    n = 0;
	}
	b->bot += n;
	n = b->top - b->bot;
	if (n > 0) 		/* network chocked */
	    break;
	block_put(r->out.block);
	r->out.block = 0;
    }

    if (setup_write(r)) 
	request_end(r, 1);
    return;
}



void do_read(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n, i;
    char* p;

    if (event & EV_TIMEOUT) {
	send_error(r, 400, "time out");
	request_end(r, 1);
	return;
    }

    n = r->in.hbufmax - r->in.hbuftop;
    if (n <= 0) {
	send_error(r, 500, "internal error");
	request_end(r, 1);
	return;
    }

    n = read(r->sock, r->in.hbuf + r->in.hbuftop, n);
    if (n < 0) {
	if (errno != EINTR && errno != EAGAIN) {
	    request_end(r, 1);
	    return;
	}
    }
    else if (n == 0) {
	;
    }
    else {
	r->in.hbuftop += n;
	n = r->in.hbuftop;
	r->in.req = gpcc_net_parse_request(r->in.hbuf, &n, r->pool);
	if (!r->in.req && r->in.hbuftop >= r->in.hbufmax) {
	    send_error(r, 400, "forbidden");
	    request_end(r, 1);
	    return;
	}
    }

    if (!r->in.req) {
	if (setup_read(r)) {
	    send_error(r, 500, "internal error");
	    request_end(r, 1);
	}
	return;
    }

    /* GOT A REQUEST */
    shutdown(r->sock, SHUT_RD);
    if (r->in.req->argc != 3) {
	gpcc_warning(FLINE, "reject invalid request from %s", r->peer);
	send_error(r, 400, "invalid request");
	request_end(r, 1);
	return;
    }
    if (0 != strncmp("HTTP/1.", r->in.req->argv[2], 7)) {
	gpcc_warning(FLINE, "reject invalid protocol from %s [%s]", r->peer, r->in.req->argv[2]);
	send_error(r, 400, "invalid request");
	request_end(r, 1);
	return;
    }
    if (0 != strcmp("GET", r->in.req->argv[0])) {
	gpcc_warning(FLINE, "reject invalid request from %s [%s %s]", r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	send_error(r, 400, "invalid request");
	request_end(r, 1);
	return;
    }

    /* scrub the path */
    /*   - forbid .. */
    for (p = r->in.req->argv[1]; 0 != (p = strchr(p, '.')); p++) {
	if (p[1] == '.') {
	    gpcc_warning(FLINE, "reject invalid request from %s [%s %s]", r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	    send_error(r, 400, "invalid request");
	    request_end(r, 1);
	    return;
	}
    }

    /*   - remove / prefixes */
    for (p = r->in.req->argv[1]; *p == '/'; p++);

    /*   - make the new path relative to opt.d */
    r->path = apr_psprintf(r->pool, "%s/%s", opt.d, p);

    /*   - remove ? and # */
    if (0 != (p = strchr(r->path, '?')))
	*p = 0;
    if (0 != (p = strchr(r->path, '#')))
	*p = 0;
    
    if (opt.v) 
	printf("[INFO] %s requests %s\n", r->peer, r->path);

    r->tid = 0;
    for (i = 0; i < r->in.req->hc; i++) {
	if (0 == strcmp("X-GP-TID", r->in.req->hname[i])) {
	    r->tid = r->in.req->hvalue[i];
	    break;
	}
    }

    if (!r->tid)
	r->tid = apr_psprintf(r->pool, "auto-tid.%d", gcb.session.gen++);

    session_attach(r);
}



void do_accept(int fd, short event, void* arg)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int sock;
    request_t* r;
    apr_pool_t* pool;
    int addr;
    int on = 1;
    struct linger linger;

    if ((sock = accept(fd, (void*) &a, &len)) < 0) {
	gpcc_warning(FLINE, "accept failed");
	return;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
	gpcc_warning(FLINE, "fcntl(F_SETFL, O_NONBLOCK) failed");
	close(sock);
	return;
    }
    if (fcntl(sock, F_SETFD, 1) == -1) {
	gpcc_warning(FLINE, "fcntl(F_SETFD) failed");
	close(sock);
	return;
    }

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    if (apr_pool_create(&pool, gcb.pool)) 
	gpcc_fatal(FLINE, "out of memory");

    r = apr_pcalloc(pool, sizeof(request_t));
    r->pool = pool;
    r->sock = sock;

    r->in.hbufmax = 1024*4;	/* for reading the headers */
    r->in.hbuf = apr_palloc(pool, r->in.hbufmax);

    addr = ntohl(a.sin_addr.s_addr);
    r->peer = apr_psprintf(r->pool, "%d.%d.%d.%d:%d", 
			   (addr >> 24) & 0xff, 
			   (addr >> 16) & 0xff,
			   (addr >> 8) & 0xff,
			   addr & 0xff, 
			   ntohs(a.sin_port));
    if (setup_read(r)) {
	send_error(r, 500, "internal error");
	request_end(r, 1);
    }
}

int setup_write(request_t* r)
{
    event_set(&r->ev, r->sock, EV_WRITE, do_write, r);
    return (event_add(&r->ev, 0));
}

int setup_read(request_t* r)
{
    struct timeval tm;
    event_set(&r->ev, r->sock, EV_READ, do_read, r);

    tm.tv_sec = 0;
    tm.tv_usec = 500 * 1000;
    return (event_add(&r->ev, &tm));
}




void http_setup(void)
{
    struct sockaddr_in a;
    int f;
    int on = 1;
    struct linger linger;


    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(opt.p);
    a.sin_family = AF_INET;

    f = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (f < 0) 
	gpcc_fatal(FLINE, "cannot create socket on port %d", opt.p);

    if (fcntl(f, F_SETFD, 1) == -1) 
	gpcc_fatal(FLINE, "cannot create socket on port %d - fcntl(F_SETFD) failed", opt.p);

    setsockopt(f, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(f, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    if (bind(f, (void*) &a, sizeof(a))) 
	gpcc_fatal(FLINE, "cannot create socket on port %d", opt.p);


    if (listen(f, 256)) 
	gpcc_fatal(FLINE, "cannot listen on port %d", opt.p);

    gcb.listen_sock = f;
    event_set(&gcb.listen_event, gcb.listen_sock, EV_READ | EV_PERSIST, do_accept, 0);
    if (event_add(&gcb.listen_event, 0))
	gpcc_fatal(FLINE, "cannot set up event on listen socket");

    if (event_priority_init(10)) 
	gpcc_warning(FLINE, "event_priority_init failed");

    if (event_priority_set(&gcb.listen_event, 0)) 
	gpcc_warning(FLINE, "event_priority_set failed");
}


int main(int argc, const char* const argv[])
{
    if (0 != apr_app_initialize(&argc, &argv, 0)) 
	gpcc_fatal(FLINE, "apr_app_initialize failed");
    atexit(apr_terminate);

    if (0 != apr_pool_create(&gcb.pool, 0))
	gpcc_fatal(FLINE, "apr_app_initialize failed");

    gcb.session.tab = apr_hash_make(gcb.pool);

    parse_command_line(argc, argv, gcb.pool);

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    event_init();

    http_setup();

    printf("Serving HTTP on port %d, directory %s ...\n", opt.p, opt.d);
    event_dispatch();

    return 0;
}

