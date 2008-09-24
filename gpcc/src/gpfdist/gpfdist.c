#include <apr_env.h>
#include <apr_file_info.h>
#include <apr_getopt.h>
#include <apr_hash.h>
#include <apr_pools.h>
#include <apr_signal.h>
#include <apr_strings.h>
#include <apr_time.h>
#include <event.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define SOCKET int
#define closesocket(x)   close(x)
#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#define SHUT_WR SD_SEND
#define SHUT_RD SD_RECEIVE
#endif
#include <fstream/fstream.h>

/*  A data block */
typedef struct blockhdr_t blockhdr_t;
struct blockhdr_t{
    char	hbyte[293];
    int         hbot,htop;
};

typedef struct block_t block_t;
struct block_t {
    blockhdr_t  hdr;
    int         bot, top;
    char        byte[FSTREAM_BLOCKSIZE];
};

#define FLINE		__FILE__":"APR_STRINGIFY(__LINE__)

int gprint(const char* fmt, ...);
void gfatal(const char* fline, const char* fmt, ...);
int gwarning(const char* fline, const char* fmt, ...);

/**************

  NOTE on GP_PROTO
  ================
  When a gpdb segment connects to gpfdist, it provides the following parameters:
	X-GP-XID   - transaction ID
	X-GP-CID   - command ID
	X-GP-SN    - session ID
	X-GP-PROTO - protocol number

  X-GP-PROTO = 0 or not provided:
	return the content of the file without any kind of meta info

  X-GP-PROTO = 1
	each data block is tagged by meta info like this:
	byte 0: type (can be 'F'ilename, 'O'ffset, 'D'ata, 'E'rror)
	byte 1-4: length. # bytes of following data block. in network-order.
	byte 5-X: the block itself.

	The stream is terminated by a Data block of length 0. If the stream is
	not property terminated, then gpfdist encountered some error, and caller
	should check the gpfdist error log.

 **************/

typedef struct gnet_request_t gnet_request_t;
struct gnet_request_t {
    int argc;
    char** argv;
    int hc;
    char* hname[50];
    char* hvalue[50];
};

static gnet_request_t*gnet_parse_request(const char* buf, int* len, apr_pool_t* pool);
static char*gstring_trim(char* s);

#ifndef GP_VERSION
#define GP_VERSION unknown
#endif

#define GP_VERSIONX APR_STRINGIFY(GP_VERSION)

/*  Struct of command line options */
static struct {
    int p;			/* port */
    int last_port;
    int v;			/* verbose */
    int V;			/* very verbose */
    const char* d;		/* directory */
    const char* l;              /* log filename */
    int g;			/* gp_proto (0 or 1) (internal, not documented) */
} opt = { 8080,8080, 0, 0, ".", 0, -1 };

#if APR_IS_BIGENDIAN
#define local_htonll(n)  (n)
#define local_ntohll(n)  (n)
#else
#define local_htonll(n)  ((((apr_uint64_t) htonl(n)) << 32LL) | htonl((n) >> 32LL))
#define local_ntohll(n)  ((((apr_uint64_t) ntohl(n)) << 32LL) | (apr_uint32_t) ntohl(((apr_uint64_t)n) >> 32LL))
#endif

#define TR(x)  while (opt.V){printf x;break;}

static void
block_fill_header(block_t*b,const struct fstream_filename_and_offset*fos)
{
    blockhdr_t*h = &b->hdr;
    apr_int32_t len;
    apr_int64_t len8;
    char*p=h->hbyte;
    int fname_len=strlen(fos->fname);
    h->hbot = 0;
    /* 'F' + len + fname */
    *p++ = 'F';
    len = htonl(fname_len);
    memcpy(p,&len,4);
    p += 4;
    memcpy(p,fos->fname,fname_len);
    p += fname_len;
    TR(("F %d %s\n",ntohl(len),fos->fname));
    /* 'O' + len + foff */
    *p++ = 'O';
    len = htonl(8);
    len8 = local_htonll(fos->foff);
    memcpy(p,&len,4);
    p += 4;
    memcpy(p,&len8,8);
    p += 8;
    TR(("O %d\n",(int)local_ntohll(len8)));
    *p++ = 'L';
    len8 = local_htonll(fos->line_number);
    memcpy(p,&len,4);
    p += 4;
    memcpy(p,&len8,8);
    p += 8;
    TR(("L %d\n",(int)local_ntohll(len8)));
    /* 'D' + len */
    *p++ = 'D';
    len = htonl(b->top-b->bot);
    memcpy(p,&len,4);
    p += 4;
    TR(("D %d\n",ntohl(len)));
    h->htop = p-h->hbyte;
    if (h->htop > sizeof(h->hbyte))
	gfatal(FLINE,"assert failed, h->htop = %d, max = %d\n",h->htop,sizeof(h->hbyte));
    TR(("header size: %d\n",h->htop-h->hbot));
}

/*  Global constrol block */
static struct {
    apr_pool_t* pool;
    SOCKET listen_sock;
    struct event listen_event;
    struct {
	int gen;
	apr_hash_t* tab;
    } session;
    apr_int64_t read_bytes;
    apr_int64_t total_bytes;
    int total_sessions;
}gcb;

/*  A session */
typedef struct session_t session_t;
struct session_t {
    apr_pool_t* pool;
    const char* key;
    const char* tid;
    const char* path;
    fstream_t*  fstream;
    int         is_error; 	/* error flag */
    int         nrequest;	/* # requests attached to this session */
    apr_time_t  mtime;		/* time when nrequest was modified */
};

/*  A http request */
typedef struct request_t request_t;
struct request_t {
    SOCKET         sock;  	/* the socket */
    apr_pool_t*    pool;	/* memory pool container */
    struct timeval tm;		/* timeout for struct event */
    struct event   ev;		/* event we are watching for this request */
    const char*    peer;	/* peer IP:port string */
    char*          path;	/* path to file */
    const char*    tid;		/* transaction id */
    const char*    csvopt;	/* options for csv file */
    session_t*     session;     /* the session this request is attached to */
    int            gp_proto;

    struct {
	char* hbuf;		/* buffer for raw incoming HTTP request */
	int   hbuftop;		/* # bytes used in hbuf */
	int   hbufmax;		/* size of hbuf[] */
	gnet_request_t* req; /* a parsed HTTP request, NULL if still incomplete. */
    } in;

    block_t out;		/* next block to send out */
};



/* Print usage */
static void usage(const char* msg)
{
    char* GPHOME = 0;
    FILE* fp = 0;

    if (gcb.pool && apr_env_get(&GPHOME, "GPHOME", gcb.pool))
	GPHOME = 0;

    if (GPHOME) {
	char* path = apr_psprintf(gcb.pool, "%s/docs/cli_help/gpfdist_help", GPHOME);
	if (path) 
	    fp = fopen(path, "r");
    }
    
    if (fp) {
	int i;
	while ((i=getc(fp)) != EOF)
	    putchar(i);
	fclose(fp);
    }
    else {
	fprintf(stderr,"gpfdist -- file distribution web server\n\n"
		"usage: gpfdist [-d <directory>] [-p <http_port>] [-l <log_file>] [-v | -V]\n\n"
		"       gpfdist -? | --version\n\n"
		"        -?        : print this screen\n"
		"        -v        : verbose mode\n"
		"        -V        : more verbose\n"
		"        -p port   : port to serve HTTP, default is 8080\n"
		"        -d dir    : serve files under the specified directory,  default is '.'\n"
		"        -l logfn  : log filename\n"
		"        --version : print version information\n\n");
    }

    if (msg)
	fprintf(stderr, "%s\n", msg);

    exit(msg ? 1 : 0);
}

static void print_version(void)
{
    printf("gpfdist version \"%s\"\n", GP_VERSIONX);
    exit(0);
}

static void print_q_x_h_are_gone(void)
{
    fprintf(stderr,"The -q, -h and -x options are gone.  Please specify these as in this example:\n");
    fprintf(stderr,"create external table a (a int) location ('gpfdist://...') format 'csv' (escape as '\"' quote as '\"' header);\n");
    exit(1);
}


/* Parse command line */
static void parse_command_line(int argc, const char* const argv[], apr_pool_t* pool)
{
    apr_getopt_t* os;
    int ch;
    const char* arg;
    int e;
    static const apr_getopt_option_t option[] = {
	{      NULL, '?', 0, "print help screen" },
	{      NULL, 'V', 0, "very verbose" },
	{      NULL, 'v', 0, "verbose mode" },
	{      NULL, 'p', 1, "which port to serve HTTP" },
	{      NULL, 'P', 1, "last port of range of ports to serve HTTP" },
	{      NULL, 'd', 1, "serve files under this directory" },
	{      NULL, 'q', 0, "gone" },
	{      NULL, 'x', 0, "gone" },
	{      NULL, 'h', 0, "gone" },
	{      NULL, 'l', 1, "log filename" },
	{      NULL, 'g', 1, "internal - gp_proto number" },
	{ "version", 256, 0, "print version number" }
    };
    
    
    if (0 != apr_getopt_init(&os, pool, argc, argv)) 
	gfatal(FLINE, "apt_getopt_init failed");

    while (0 == (e = apr_getopt_long(os, option, &ch, &arg))) {
	switch (ch) {
	case '?': usage(0); break;
	case 'v': opt.v = 1; break;
	case 'V': opt.v = opt.V = 1; break;
	case 'h': print_q_x_h_are_gone(); break;
	case 'd': opt.d = arg; break;
	case 'q': print_q_x_h_are_gone(); break;
	case 'x': print_q_x_h_are_gone(); break;
	case 'p': opt.last_port = opt.p = atoi(arg); break;
	case 'P': opt.last_port = atoi(arg); break;
	case 'l': opt.l = arg; break;
	case 'g': opt.g = atoi(arg); break;
	case 256: print_version(); break;
	}
    }
    if (e != APR_EOF) 
	usage("Error: illegal arguments"); 

    if (! (0 < opt.p && opt.p < (1 << 16)))
	usage("Error: please specify a valid port number for -p switch");

    if (-1 != opt.g) {
	if (! (0 == opt.g || 1 == opt.g)) 
	    usage("Error: please specify 0 or 1 for -g switch (note: this is internal)");
    }

    /* validate opt.d */
    {
	char* p = gstring_trim(apr_pstrdup(pool, opt.d));

	/* collapse // */
	while (p[0] == '/' && p[1] == '/') p++;

	/* disallow / */
	if (0 == strcmp(p, "/")) 
	    usage("Security Error: cannot run under root (/) directory");

	/* strip ending / */
	while (p[0] && p[strlen(p) - 1] == '/')
	    p[strlen(p) - 1] = 0;
	opt.d = p;

	if (0 == strlen(opt.d))
	    opt.d = ".";

	/* check that the dir exists */
	if (0 != apr_filepath_set(opt.d, pool))
	    usage(apr_psprintf(pool, 
			       "Error: cannot access directory '%s'\n"
			       "Please specify a valid directory for -d switch", opt.d));

	if (0 != apr_filepath_get(&p, 0, pool))
	    usage(apr_psprintf(pool, 
			       "Error: cannot access directory '%s'\n"
			       "Please specify a valid directory for -d switch", opt.d));
	opt.d = p;
    }

    /* validate opt.l */
    if (opt.l) {
	FILE *f;

	char* p = gstring_trim(apr_pstrdup(pool, opt.l));
	
	/* collapse // */
	while (p[0] == '/' && p[1] == '/') p++;
	
	/* disallow / */
	if (0 == strcmp(p, "/")) 
	    usage("Security Error: cannot run under root (/) directory");
	
	/* strip ending / */
	while (p[0] && p[strlen(p) - 1] == '/')
	    p[strlen(p) - 1] = 0;
	opt.l = p;
	
	if (0 == strlen(opt.l))
	    opt.l = ".";
	
	/* check that the file exists */
	f = fopen(opt.l, "a");
	if (!f) {
	    fprintf(stderr, "unable to create log file %s\n", opt.l);
	    exit(1);
	}
	fclose(f);

    }
    /* there should not be any more args left */
    if (os->ind != argc) 
	usage("Error: illegal arguments");
}

/* send gp-proto==1 ctl info */
static void gp1_send_eof(request_t* r);
static void gp1_send_errmsg(request_t* r, const char* msg);

static int setup_read(request_t* r);
static int setup_write(request_t* r);
static void session_attach(request_t* r);
static void session_detach(request_t* r);
static void session_end(session_t* s, int error);
static void session_free(session_t* s);

/* send an error response */
static void http_error(request_t* r, int code, const char* msg)
{
    char buf[1024];
    int n;
    gprint("%s - %d %s\n", r->peer, code, msg);
    n = apr_snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n"
		     "Content-length: 0\r\n"
		     "Expires: 0\r\n"
		     "X-GPFDIST-VERSION: " GP_VERSIONX "\r\n"
		     "Cache-Control: no-cache\r\n"
		     "Connection: close\r\n\r\n",
		     code, msg);

    send(r->sock, buf, n, 0);
}


/* send an empty response */
static void http_empty(request_t* r)
{
    static const char buf[]="HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Content-length: 0\r\n"
	"Expires: 0\r\n"
	"X-GPFDIST-VERSION: " GP_VERSIONX "\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n\r\n";
    gprint("%s %s %s - OK\n", r->peer, r->in.req->argv[0], r->in.req->argv[1]);
    send(r->sock, buf, sizeof buf-1, 0);
}

static apr_status_t
send_gpfdist_status(request_t*r)
{
    char buf[1024];
    int m, n;
    n = apr_snprintf(buf,sizeof(buf),
	"HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Expires: 0\r\n"
	"X-GPFDIST-VERSION: " GP_VERSIONX "\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n\r\n"
	"read_bytes %"APR_INT64_T_FMT"\r\n"
	"total_bytes %"APR_INT64_T_FMT"\r\n"
	"total_sessions %d\r\n",
	gcb.read_bytes,gcb.total_bytes,gcb.total_sessions);
    if (n >= sizeof buf-1)
	gfatal(FLINE, "internal error - buffer overflow");
    m = send(r->sock, buf, n, 0);
    if (m != n) {
	gprint("%s - socket error\n", r->peer);
	return APR_EGENERAL;
    }
    return 0;
}

/* send an OK response */
static apr_status_t http_ok(request_t* r)
{
    const char* fmt = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Expires: 0\r\n"
	"X-GPFDIST-VERSION: " GP_VERSIONX "\r\n"
	"X-GP-PROTO: %d\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n\r\n";
    char buf[1024];
    int m, n;

    n = apr_snprintf(buf, sizeof(buf), fmt, r->gp_proto);
    if (n >= sizeof(buf) - 1)
	gfatal(FLINE, "internal error - buffer overflow");


    m = send(r->sock, buf, n, 0);
    if (m != n) {
	gprint("%s - socket error\n", r->peer);
	return APR_EGENERAL;
    }
    gprint("%s %s %s - OK\n", r->peer, r->in.req->argv[0], r->in.req->argv[1]);

    return 0;
}

/* Finished a request. Close socket and cleanup. 
   Note: ending a request does not mean that the session is ended.
   Maybe there is a request out there (of the same session), that still 
   has a block to send out.
 */
static void request_end(request_t* r, int error, const char* errmsg)
{
    session_t* s = r->session;

    if (r->gp_proto == 1) {
	if (!error)
	    gp1_send_eof(r);
	else if (errmsg)
	    gp1_send_errmsg(r, errmsg);
    }

    TR(("[%d] request end\n", r->sock));

    /* If we still have a block outstanding, the session is corrupted. */
    if (r->out.top != r->out.bot) {
	gwarning(FLINE, "request failure resulting in session failure");
	if (s) 
	    session_end(s, 1);
    }
    else {
	session_detach(r);
    }
    event_del(&r->ev);
    shutdown(r->sock, SHUT_WR | SHUT_RD);
    closesocket(r->sock);
    r->sock = -1;
    apr_pool_destroy(r->pool);
}

static int
local_send(int fd, const char* buf, int buflen)
{
    int n = send(fd, buf, buflen, 0);
    if (n < 0) {
#ifdef WIN32
	int e = WSAGetLastError();
	int ok = (e == WSAEINTR || e == WSAEWOULDBLOCK);
#else
	int e = errno;
	int ok = (e == EINTR || e == EAGAIN);
#endif
	return ok ? 0 : -1;
    }
    return n;
}

static int
local_sendall(int fd, const char* buf, int buflen)
{
    int oldlen=buflen;
    while (buflen){
	int i=local_send(fd,buf,buflen);
	if (i < 0)
	    return i;
	buf += i;
	buflen -= i;
    }
    return oldlen;
}

static int
gp1_send_header(request_t*r,char letter,int length)
{
    char hdr[5];
    const char*p=hdr;
    hdr[0] = letter;
    length = htonl(length);
    memcpy(hdr+1,&length,4);
    return local_sendall(r->sock,p,5)<0?-1:0;
}

static void gp1_send_eof(request_t* r)
{
    if (!gp1_send_header(r,'D',0))
	TR(("[%d] sent EOF\n", r->sock));
}

static void
gp1_send_errmsg(request_t* r, const char* errmsg)
{
    apr_int32_t len=strlen(errmsg);
    if (!gp1_send_header(r,'E',len))
	local_sendall(r->sock,errmsg,len);
}

/* Get a block out of the session. return error string. */
static const char*
session_get_block(session_t* s, block_t* retblock)
{
    int size;
    struct fstream_filename_and_offset fos;
    retblock->bot = retblock->top = 0;
    if (s->is_error || 0 == s->fstream) {
	session_end(s, 0);
	return 0;
    }
    size = fstream_read(s->fstream,retblock->byte,sizeof retblock->byte,&fos,1);
    if (size < 0){
	const char*ferror=fstream_get_error(s->fstream);
	session_end(s, 1);
	return ferror;
    }
    if (size == 0){
	session_end(s, 0);
	return 0;
    }
    gcb.read_bytes += size;
    retblock->top = size;
    block_fill_header(retblock,&fos);
    return 0;
}


/* finish the session - close the file */
static void session_end(session_t* s, int error)
{
    if (error)
	s->is_error = error;

    if (s->fstream) {
	fstream_close(s->fstream);
	s->fstream = 0;
    }
}


/* deallocate session, remove from hashtable */
static void session_free(session_t* s)
{
    gprint("free session %s\n", s->key);

    if (s->fstream) {
	fstream_close(s->fstream);
	s->fstream = 0;
    }

    apr_hash_set(gcb.session.tab, s->key, APR_HASH_KEY_STRING, 0);
    apr_pool_destroy(s->pool);
}


/* detach a request from a session */
static void session_detach(request_t* r)
{
    session_t* s = r->session;
    r->session = 0;
    if (s) {
	if (s->nrequest <= 0) 
	    gfatal(FLINE, "assert failed");
	s->nrequest--;
	s->mtime = apr_time_now();

	/* for auto-tid sessions, we can free it now */
	if (0 == strncmp("auto-tid.", s->tid, 9)) {
	    if (s->nrequest != 0) 
		gfatal(FLINE, "assert failed");

	    session_free(s);
	}
    }
}

static void sessions_cleanup(void) 
{
    apr_hash_index_t *hi;
    int i, n = 0;
    void* entry;
    session_t **session, *s;
    int nses;

    nses = apr_hash_count(gcb.session.tab);
    if (nses == 0)
	return;
    
    if (! (session = malloc(sizeof(session_t *) * nses))) 
	gfatal(FLINE, "out of memory");

    hi = apr_hash_first(gcb.pool, gcb.session.tab);
    for (i = 0; hi && i < nses; i++, hi = apr_hash_next(hi)) {
	apr_hash_this(hi, 0, 0, &entry);
	s = (session_t*) entry;
	if (s->nrequest == 0 && (s->mtime < apr_time_now() - 300 * APR_USEC_PER_SEC)) {
	    session[n++] = s;
	}
    }

    for (i = 0 ; i < n ; i++) {
	gprint("remove out-dated session %s\n", session[i]->key);
	session_free(session[i]);
	session[i] = 0;
    }

    free(session);
}

static void session_attach(request_t* r)
{
    char key[1024];
    session_t* s = 0;
    
    /* create the key */
    if (sizeof(key) -1 == apr_snprintf(key, sizeof(key), "%s:%s", r->tid, r->path)) {
	http_error(r, 400, "path too long");
	request_end(r, 1, 0);
	return;
    }

    /* check in session hashtable */
    s = apr_hash_get(gcb.session.tab, key, APR_HASH_KEY_STRING);
    if (! s) {
	/* not in hashtable - create new session */
	fstream_t* fstream = 0;
	apr_pool_t* pool;
	struct fstream_options fstream_options;
	int64_t size;
	/* remove any outdated sessions*/
	sessions_cleanup();

	if (apr_pool_create(&pool, gcb.pool)) {
	    gwarning(FLINE, "out of memory");
	    http_error(r, 500, "internal error - out of memory");
	    request_end(r, 1, 0);
	    return;
	}
	/* parse csvopt header */
	memset(&fstream_options,0,sizeof fstream_options);
	fstream_options.verbose = opt.v;
	{
	    int quote=0,escape=0;
	    sscanf(r->csvopt,"m%dx%dq%dh%d",&fstream_options.is_csv,&escape,&quote,&fstream_options.header);
	    fstream_options.quote = quote;
	    fstream_options.escape = escape;
	}
	/* try opening the fstream */
	fstream = fstream_open(r->path,&fstream_options);
	if (! fstream) {
	    gwarning(FLINE, "reject request from %s, path %s\n", r->peer, r->path);
	    http_error(r, 404, "not found");
	    request_end(r, 1, 0);
	    apr_pool_destroy(pool);
	    return;
	}
	gcb.total_sessions++;
	size = fstream_get_size(fstream);
	if (size == -1)
	    gcb.total_bytes = -1;
	else
	    gcb.total_bytes += size;
	/* allocate session */
	if (0 == (s = apr_palloc(pool, sizeof(session_t))))
	    gfatal(FLINE, "out of memory");
	memset(s, 0, sizeof(*s));

	if (0 == (s->tid = apr_pstrdup(pool, r->tid)) 
	    || 0 == (s->path = apr_pstrdup(pool, r->path))
	    || 0 == (s->key = apr_pstrdup(pool, key))) {
	    gfatal(FLINE, "out of memory");
	}

	s->fstream = fstream;
	s->pool = pool;

	/* insert into hashtable */
	apr_hash_set(gcb.session.tab, s->key, APR_HASH_KEY_STRING, s);

	gprint("new session (%s, %s)\n", s->path, s->tid);
    }

    /* found a session in hashtable*/

    /* if error, send an error and close */
    if (s->is_error) {
	http_error(r, 500, "session error");
	request_end(r, 1, 0);
	return;
    }

    /* session already ended. send an empty response, and close. */
    if (0 == s->fstream) {
	http_empty(r);
	request_end(r, 0, 0);
	return;
    }

    gprint("joined session (%s, %s)\n", s->path, s->tid);

    /* setup so receive EV_WRITE events to write to socket */
    if (setup_write(r)) {
	http_error(r, 500, "internal error");
	request_end(r, 1, 0);
	return;
    }

    /* one more request for session */
    s->nrequest++;
    s->mtime = apr_time_now();
    r->session = s;

    /* finally, tell peer that we are ready to send data */
    if (0 != http_ok(r))
	request_end(r, 1, 0);
}





/* Callback when the socket is ready to be written */
static void do_write(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n, i;
    block_t* b;

    if (fd != r->sock) 
	gfatal(FLINE, "assert failed");

    /* Loop at most 3 blocks or until we choke on the socket */
    for (i = 0; i < 3; i++) {
	/* get a block (or find a remaining block) */
	if (r->out.top == r->out.bot) {
	    const char* ferror = session_get_block(r->session, &r->out);
	    if (ferror) {
		request_end(r, 1, ferror);
		return;
	    }
	    if (! r->out.top) {
		request_end(r, 0, 0);
		return;
	    }
	}

	b = &r->out;
	if (r->gp_proto == 1) {
	    n = b->hdr.htop - b->hdr.hbot;
	    if (n > 0) {
		n = local_send(fd, b->hdr.hbyte + b->hdr.hbot, n);
		if (n < 0) {
		    if (errno==EPIPE || errno==ECONNRESET)
			r->out.bot = r->out.top;
		    perror("send");
		    request_end(r, 1, 0);
		    return;
		}

		TR(("[%d] send header bytes %d .. %d (top %d)\n", 
		    fd, b->hdr.hbot, b->hdr.hbot + n, b->hdr.htop));

		b->hdr.hbot += n;
		n = b->hdr.htop - b->hdr.hbot;
		if (n > 0) 
		    break;	/* network chocked */
	    }
	}
	/* write out the block */
	n = b->top - b->bot;
	/*
	if (n == 0) 
	    gfatal(FLINE, "internal error: 0 bytes");
	*/

	n = local_send(fd, b->byte + b->bot, n);
	if (n < 0) {
/* EPIPE (or ECONNRESET some computers) indicates remote socket intentionally
shut down half of the pipe.  If this was because of something like
"select ... limit 10;", then it is fine that we couldn't transmit all the
data--the segment didn't want it anyway.  If it is because the segment
crashed or something like that, hopefully we would find out about that in
some other way anyway, so it is okay if we don't poison the session. */
	    if (errno==EPIPE || errno==ECONNRESET)
		r->out.bot = r->out.top;
	    perror("send");
	    request_end(r, 1, 0);
	    return;
	}
	TR(("[%d] send data bytes off buf %d .. %d (top %d)\n", 
	    fd, b->bot, b->bot + n, b->top));
	b->bot += n;

	if (b->top != b->bot) {	/* network chocked */
	    TR(("[%d] network full\n", fd));
	    break;
	}
    }

    /* Set up for this routine to be called again */
    if (setup_write(r)) 
	request_end(r, 1, 0);
}


/* Callback when a socket is ready to be read. Read the 
 * socket for a complete HTTP request.
 */
static void do_read(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n, i;
    char* p;
    char* pp;

    /* If we timeout, close the request. */
    if (event & EV_TIMEOUT) {
	http_error(r, 400, "time out");
	request_end(r, 1, 0);
	return;
    }

    /* how many bytes left in the header buf */
    n = r->in.hbufmax - r->in.hbuftop;
    if (n <= 0) {
	http_error(r, 500, "internal error");
	request_end(r, 1, 0);
	return;
    }

    /* read into header buf */
    n = recv(r->sock, r->in.hbuf + r->in.hbuftop, n, 0);
    if (n < 0) {
#ifdef WIN32
	int e = WSAGetLastError();
	int ok = (e == WSAEINTR || e == WSAEWOULDBLOCK);
#else
	int e = errno;
	int ok = (e == EINTR || e == EAGAIN);
#endif
	if (! ok) {
	    request_end(r, 1, 0);
	    return;
	}
    }
    else if (n == 0) {
	/* socket close by peer will return 0 */
	request_end(r, 1, 0);
	return;
    }
    else {
	/* check if a complete HTTP request is availble in header buf */
	r->in.hbuftop += n;
	n = r->in.hbuftop;
	r->in.req = gnet_parse_request(r->in.hbuf, &n, r->pool);
	if (!r->in.req && r->in.hbuftop >= r->in.hbufmax) {
	    /* not available, but headerbuf is full - send error and close */
	    http_error(r, 400, "forbidden");
	    request_end(r, 1, 0);
	    return;
	}
    }

    /* if we don't yet have a complete request, set up this function to be
       called again for */
    if (!r->in.req) {
	if (setup_read(r)) {
	    http_error(r, 500, "internal error");
	    request_end(r, 1, 0);
	}
	return;
    }

    /* Hurray, got a request !!! */
    gprint("%s requests %s\n", 
	       r->peer, r->in.req->argv[1] ? r->in.req->argv[1] : "(none)");

    /* we don't need to read from the socket anymore */
    shutdown(r->sock, SHUT_RD);

    if (opt.V) {
	gprint("request");
	for (i = 0; i < r->in.req->argc; i++)
	    gprint(" %s", r->in.req->argv[i]);
	gprint("\n");
    }

    /* parse the HTTP request. Expect "GET /path HTTP/1.X" */
    if (r->in.req->argc != 3) {
	gwarning(FLINE, "reject invalid request from %s", r->peer);
	http_error(r, 400, "invalid request");
	request_end(r, 1, 0);
	return;
    }
    if (0 != strncmp("HTTP/1.", r->in.req->argv[2], 7)) {
	gwarning(FLINE, "reject invalid protocol from %s [%s]", r->peer, r->in.req->argv[2]);
	http_error(r, 400, "invalid request");
	request_end(r, 1, 0);
	return;
    }
    if (0 != strcmp("GET", r->in.req->argv[0])) {
	gwarning(FLINE, "reject invalid request from %s [%s %s]", 
		 r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	http_error(r, 400, "invalid request");
	request_end(r, 1, 0);
	return;
    }

    /* make a copy of the path */
    r->path = apr_pstrdup(r->pool, r->in.req->argv[1]);

    /*   - decode %xx to char */
    for (p = pp = r->path; *pp; p++, pp++) {
	if ('%' == (*p = *pp)) {
	    if (pp[1] && pp[2]) {
		int x = pp[1], y = pp[2];
		if ('0' <= x && x <= '9') 
		    x -= '0';
		else if ('a' <= x && x <= 'f')
		    x = x - 'a' + 10;
		else if ('A' <= x && x <= 'F')
		    x = x - 'A' + 10;
		else 
		    x = -1;
		if ('0' <= y && y <= '9') 
		    y -= '0';
		else if ('a' <= y && y <= 'f')
		    y = y - 'a' + 10;
		else if ('A' <= y && y <= 'F')
		    y = y - 'A' + 10;
		else 
		    y = -1;
		if (x >= 0 && y >= 0) {
		    x = (x << 4) + y;
		    *p = (char) x;
		    pp++, pp++;
		}
	    }
	}
    }
    *p = 0;

    /*   - remove / prefixes */
    for (;*r->path=='/';r->path++);
    if (!strcmp(r->path,"gpfdist_status")){
	send_gpfdist_status(r);
	request_end(r,0,0);
	return;
    }
    /*   - forbid .. dot dot  */
    if (strstr(r->path,"..")){
	gwarning(FLINE, "reject invalid request from %s [%s %s]",
		     r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	http_error(r, 400, "invalid request");
	request_end(r, 1, 0);
	return;
    }
    
    /*   - make the new path relative to opt.d */
    r->path = apr_psprintf(r->pool, "%s/%s", opt.d, r->path);

    /* Extract X-GP-TID value from HTTP headers */
    {
	const char* xid = 0;
	const char* cid = 0;
	const char* sn = 0;
	const char* gp_proto = "0";
	r->csvopt = "";
	for (i = 0; i < r->in.req->hc; i++) {
	    if (0 == strcmp("X-GP-XID", r->in.req->hname[i])) 
		xid = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-CID", r->in.req->hname[i]))
		cid = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-SN", r->in.req->hname[i]))
		sn = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-CSVOPT", r->in.req->hname[i]))
		r->csvopt = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-PROTO", r->in.req->hname[i]))
		gp_proto = r->in.req->hvalue[i];

	    if (r->in.req->hname[i][0] == 'X')
		TR(("%s: %s\n", r->in.req->hname[i], r->in.req->hvalue[i]));
	}
	r->gp_proto = strtol(gp_proto, 0, 0);
	if (r->gp_proto != 0 && r->gp_proto != 1) {
	    gwarning(FLINE, "reject invalid request from %s [%s %s] - X-GP-PROTO invalid '%s'",
		     r->peer, r->in.req->argv[0], r->in.req->argv[1], gp_proto);
	    http_error(r, 400, "invalid request");
	    request_end(r, 1, 0);
	    return;
	}
	if (opt.g != -1)	/* override?  */
	    r->gp_proto = opt.g; 
	if (xid && cid && sn) {
	    r->tid = apr_psprintf(r->pool, "%s.%s.%s.%d", xid, cid, sn, r->gp_proto);
	}
	else if (xid || cid || sn) {
	    gwarning(FLINE, "reject invalid request from %s [%s %s] - missing X-GP-* header", 
			 r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	    http_error(r, 400, "invalid request");
	    request_end(r, 1, 0);
	    return;
	}
	else {
	    r->tid = apr_psprintf(r->pool, "auto-tid.%d", gcb.session.gen++);
	}
    }

    /* Attach to a session */
    session_attach(r);
}


/* Callback when the listen socket is ready to accept connections. */
static void do_accept(int fd, short event, void* arg)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    SOCKET sock;
    request_t* r;
    apr_pool_t* pool;
    int addr;
    int on = 1;
    struct linger linger;

    /* do the accept */
    if ((sock = accept(fd, (void*) &a, &len)) < 0) {
	gwarning(FLINE, "accept failed");
	goto failure;
    }

    /* set to non-blocking, and close-on-exec */
#ifdef WIN32
    {
	unsigned long nonblocking = 1;
	ioctlsocket(sock, FIONBIO, &nonblocking);
    }
#else
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
	gwarning(FLINE, "fcntl(F_SETFL, O_NONBLOCK) failed");
	closesocket(sock);
	goto failure;
    }
    if (fcntl(sock, F_SETFD, 1) == -1) {
	gwarning(FLINE, "fcntl(F_SETFD) failed");
	closesocket(sock);
	goto failure;
    }
#endif

    /* set keepalive, reuseaddr, and linger */
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    /* create a pool container for this socket */
    if (apr_pool_create(&pool, gcb.pool)) 
	gfatal(FLINE, "out of memory");

    /* create the request in pool */
    r = apr_pcalloc(pool, sizeof(request_t));
    r->pool = pool;
    r->sock = sock;

    r->in.hbufmax = 1024*4;	/* 4K for reading the headers */
    r->in.hbuf = apr_palloc(pool, r->in.hbufmax);

    addr = ntohl(a.sin_addr.s_addr);
    r->peer = apr_psprintf(r->pool, "%d.%d.%d.%d", 
			   (addr >> 24) & 0xff, 
			   (addr >> 16) & 0xff,
			   (addr >> 8) & 0xff,
			   addr & 0xff);

    /* set up for callback when socket ready for reading */
    if (setup_read(r)) {
	http_error(r, 500, "internal error");
	request_end(r, 1, 0);
    }

    return;

    failure:
    gwarning(FLINE, "accept failed");
    return;
}

static int setup_write(request_t* r)
{
    if (r->sock < 0) 
	gfatal(FLINE, "assert failed");
    event_del(&r->ev);
    event_set(&r->ev, r->sock, EV_WRITE, do_write, r);
    return (event_add(&r->ev, 0));
}

static int setup_read(request_t* r)
{
    if (r->sock < 0) 
	gfatal(FLINE, "assert failed");
    event_del(&r->ev);
    event_set(&r->ev, r->sock, EV_READ, do_read, r);

    r->tm.tv_sec = 2;
    r->tm.tv_usec = 0;
    return (event_add(&r->ev, &r->tm));
}


/* Create HTTP port and start to receive request */
static void http_setup(void)
{
    struct sockaddr_in a;
    SOCKET f;
    int on = 1;
    struct linger linger;

    /* create socket */
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_family = AF_INET;

    f = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (f < 0) 
	gfatal(FLINE, "cannot create socket on port %d", opt.p);

#ifndef WIN32
    if (fcntl(f, F_SETFD, 1) == -1) 
	gfatal(FLINE, "cannot create socket on port %d - fcntl(F_SETFD) failed", opt.p);

	/* For the Windows case, we could use SetHandleInformation to remove
       the HANDLE_INHERIT property from fd.  
       But for our purposes this does not matter,
       as by default handles are *not* inherited. */

#endif

    setsockopt(f, SOL_SOCKET, SO_KEEPALIVE, (void*) &on, sizeof(on));
    setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (void*) &on, sizeof(on));
    linger.l_onoff = 1;
    linger.l_linger = 5;
    setsockopt(f, SOL_SOCKET, SO_LINGER, (void*) &linger, sizeof(linger));

    for (;;){
	a.sin_port = htons(opt.p);
    /* bind */
	if (!bind(f, (void*) &a, sizeof(a))) 
	    break;
	if (opt.p >= opt.last_port)
	    gfatal(FLINE, "cannot create socket on port %d", opt.p);
	opt.p++;
    }

    /* listen with a big queue */
    if (listen(f, 256)) 
	gfatal(FLINE, "cannot listen on port %d", opt.p);

    gcb.listen_sock = f;

    /* setup event priority */
    if (event_priority_init(10)) 
	gwarning(FLINE, "event_priority_init failed");

    /* when this socket is ready, do accept */
    event_set(&gcb.listen_event, gcb.listen_sock, EV_READ | EV_PERSIST, do_accept, 0);

    /* high priority so we accept as fast as possible */
    if (event_priority_set(&gcb.listen_event, 0)) 
	gwarning(FLINE, "event_priority_set failed");

    /* start watching this event */
    if (event_add(&gcb.listen_event, 0))
	gfatal(FLINE, "cannot set up event on listen socket");

}


void process_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
	gwarning(FLINE, "signal %d received. gpfdist exits", sig);
	if (gcb.listen_sock > 0) {
	    closesocket(gcb.listen_sock);
	}
	exit(1);
    }
}

int main(int argc, const char* const argv[])
{
    if (0 != apr_app_initialize(&argc, &argv, 0)) 
	gfatal(FLINE, "apr_app_initialize failed");
    atexit(apr_terminate);

    if (0 != apr_pool_create(&gcb.pool, 0))
	gfatal(FLINE, "apr_app_initialize failed");

    apr_signal_init(gcb.pool);

    gcb.session.tab = apr_hash_make(gcb.pool);

    parse_command_line(argc, argv, gcb.pool);

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    /*
    apr_signal(SIGINT, process_signal);
    */
    apr_signal(SIGTERM, process_signal);
    if (opt.V)
	setenv("EVENT_SHOW_METHOD", "1",1);
    setenv("EVENT_NOKQUEUE", "1",1);
    
    event_init();

    http_setup();

    printf("Serving HTTP on port %d, directory %s\n", opt.p, opt.d);
    fflush(stdout);

    /* redirect stderr and stdout to log */
    if (opt.l) {
	freopen(opt.l, "a", stderr);
	setlinebuf(stderr);
	freopen(opt.l, "a", stdout);
	setlinebuf(stdout);
    }

    return event_dispatch();
}

static gnet_request_t*gnet_parse_request(const char* buf, int* len, apr_pool_t* pool)
{
    int n = *len;
    const char* p;
    char* line;
    char* last;
    gnet_request_t* req = 0;
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
    req = apr_pcalloc(pool, sizeof(gnet_request_t));

    /* for first line */
    line = apr_strtok(line, "\n", &last);
    if (!line) 
	line = apr_pstrdup(pool, "");
    line = gstring_trim(line);
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
	    line = gstring_trim(line);
	    if (*line == 0)	/* empty line */
		break;
	    /* add to previous hvalue */
	    req->hvalue[req->hc-1] 
		= gstring_trim(apr_pstrcat(pool, req->hvalue[req->hc-1], 
					       " ", line, 0));
	    continue;
	}
	/* find a colon, and break the line in two */
	if (! (colon = strchr(line, ':')))
	    colon = line + strlen(line); 
	else
	    *colon++ = 0;

	line = gstring_trim(line);
	if (*line == 0) 	/* empty line */
	    break;

	/* save name, value pair */
	req->hname[req->hc] = line;
	req->hvalue[req->hc] = gstring_trim(colon);
	req->hc++;

	if (req->hc >= sizeof(req->hname) / sizeof(req->hname[0]))
	    break;
    }

    return req;
}


static char*gstring_trim(char* s)
{
    char* p;
    s += strspn(s, " \t\r\n");
    for (p = s + strlen(s) - 1; p > s; p--) {
	if (strchr(" \t\r\n", *p))
	    *p = 0;
	else
	    break;
    }
    return s;
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

int
gprint(const char* fmt, ...)
{
    va_list ap;
    if (opt.v){
	va_start(ap, fmt);
	printf("[%s] ", datetime());
	vprintf(fmt, ap);
	va_end(ap);
    }
    return 0;
}


void
gfatal(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] [INTERNAL ERROR %s] ", datetime(), fline);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n          ... exiting\n"); 
    exit(1);
}


int
gwarning(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] [WRN %s] ", datetime(), fline); 
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    return 0;
}

void
gfile_printf_then_putc_newline(const char*format,...)
{
    va_list va;
    va_start(va,format);
    vprintf(format,va);
    va_end(va);
    putchar('\n');
}
void*
fstream_malloc(size_t size)
{
    return malloc(size);
}
void
fstream_free(void*a)
{
    free(a);
}
