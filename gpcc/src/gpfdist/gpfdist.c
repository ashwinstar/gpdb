#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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
#define SHUT_WR SD_SEND
#define SHUT_RD SD_RECEIVE
#endif

#include <signal.h>
#include <apr_signal.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_file_info.h>
#include <apr_time.h>
/*#include <apr_uri.h>*/
#include <apr_env.h>
#include <apr_fnmatch.h>
#include "gfile.h"

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

static APR_DECLARE(gnet_request_t*) gnet_parse_request(const char* buf, int* len, apr_pool_t* pool);
static APR_DECLARE(char*) gstring_trim(char* s);


#include <event.h>
#define BLOCKSIZE  (1024 * 32)

#ifndef GP_VERSION
#define GP_VERSION unknown
#endif

#define GP_VERSIONX APR_STRINGIFY(GP_VERSION)

#ifndef WIN32
#define SOCKET int
#define closesocket(x)   close(x)
#define O_BINARY 0
#endif

/*  Struct of command line options */
static struct {
    int p;			/* port */
    int v;			/* verbose */
    int V;			/* very verbose */
    int h;			/* skip first-row header */
    const char* d;		/* directory */
    const char* l;              /* log filename */
    int g;			/* gp_proto (0 or 1) (internal, not documented) */
} opt = { 8080, 0, 0, 0, ".", 0, -1 };


/*  A data block */
typedef struct blockhdr_t blockhdr_t;
struct blockhdr_t {
    char        fname[256];
    int         fname_len;
    apr_int64_t foff;
    char	hbyte[280];
    int         hbot, htop;
};

typedef struct block_t block_t;
struct block_t {
    blockhdr_t  hdr;
    int         bot, top;
    block_t*    __next;		/* chaining for freelist */
    char        byte[BLOCKSIZE - sizeof(blockhdr_t) - sizeof(int)*2 - sizeof(block_t*)];
};

#if APR_IS_BIGENDIAN
#define local_htonll(n)  (n)
#define local_ntohll(n)  (n)
#else
#define local_htonll(n)  ((((apr_uint64_t) htonl(n)) << 32LL) | htonl((n) >> 32LL))
#define local_ntohll(n)  ((((apr_uint64_t) ntohl(n)) << 32LL) | (apr_uint32_t) ntohl(((apr_uint64_t)n) >> 32LL))
#endif

#define TR(x)  if (!opt.V) ; else printf x


#define TR2(x) 

static void block_fill_header(block_t* b)
{
    blockhdr_t* h = &b->hdr;
    apr_int32_t len;
    apr_int64_t len8;
    char* p;

    h->hbot = h->htop = 0;
    if (h->fname_len == 0) 
	h->fname_len = strlen(h->fname);

    p = h->hbyte;
    /* 'F' + len + fname */
    *p = 'F';                              p++;
    len = htonl(h->fname_len);
    memcpy(p, &len, 4);                    p += 4;
    memcpy(p, h->fname, h->fname_len);     p += h->fname_len;
    TR(("F %d %s\n", ntohl(len), h->fname));
    
    /* 'O' + len + foff */
    *p = 'O';                              p++;
    len = htonl(8);
    len8 = local_htonll(h->foff);
    memcpy(p, &len, 4);                    p += 4;
    memcpy(p, &len8, 8);                   p += 8;
    TR(("O %d\n", (int) local_ntohll(len8)));
    
    /* 'D' + len */
    *p = 'D';                              p++;
    len = htonl(b->top - b->bot);
    memcpy(p, &len, 4);                    p += 4;
    TR(("D %d\n", ntohl(len)));

    h->htop = (p - h->hbyte);
    if (h->htop > sizeof(h->hbyte)) {
	gfatal(FLINE, "assert failed, h->htop = %d, max = %d\n", h->htop, sizeof(h->hbyte));
    }

    TR(("header size: %d\n", h->htop - h->hbot));
}

/*  Global constrol block */
static struct {
    apr_pool_t* pool;
    SOCKET listen_sock;
    struct event listen_event;

    block_t* freelist;
    int      freecnt;
    
    struct {
	int gen;
	apr_hash_t* tab;
    } session;
} gcb = {0};


/* A file stream - data may come from several files */
typedef struct fstream_t fstream_t;
struct fstream_t {
    const char** fpath;		/* array of fpath */
    gfile_t**fd;            /* array of fd */
    int          fn;		/* #elements in ffd[] and fpath[] */
    int		 fidx;		/* current index in ffd[] */
    apr_uint64_t foff;		/* current offset in ffd[fidx] */
    int          fskip;		/* reflect opt.h (skip first line) */
    block_t*     fblock;	/* residue block */
    char* 	 ferror;	/* error flag */
    int 	 is_csv;	/* is the file csv? */
    char 	 quote;		/* quote char */
    char 	 escape;	/* escape char */
};


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

    struct {
	block_t* block;		/* next block to send out */
    } out;
};


/* Get a block from freelist if possible, otherwise, malloc */
static block_t* block_get(void)
{
    block_t* b;
    if (0 != (b = gcb.freelist)) {
	gcb.freelist = b->__next;
	gcb.freecnt--;
    }
    else {
	if (! (b = malloc(sizeof(block_t)))) 
	    gfatal(FLINE, "out of memory");
    }

    b->bot = b->top = 0;
    b->__next = 0;
    b->hdr.fname[0] = 0;
    b->hdr.fname_len = 0;
    b->hdr.foff = 0;
    b->hdr.htop = b->hdr.hbot = 0;
    return b;
}


/* Put the block back into freelist */
static void block_put(block_t* b)
{
    b->__next = gcb.freelist;
    gcb.freelist = b;
    gcb.freecnt++;
}

/* split the block at byte n (relative to bot). 
 *   b - left side: byte[0:n]
 *   bb - right side: byte[n:]
 */
static block_t* block_split(block_t* b, int n)
{
    block_t* bb;	       
    int m = b->top - b->bot - n;
    if (m < 0)
	gfatal(FLINE, "assert failed, n = %d, bot %d, top %d", n, b->bot, b->top);

    if (m == 0)
	return 0;

    bb = block_get();
    memcpy(bb->byte, b->byte + b->bot + n, m);
    strcpy(bb->hdr.fname, b->hdr.fname);
    bb->hdr.foff = b->hdr.foff + n;
    b->top = b->bot + n;
    bb->top = m;
    return bb;
}


/* Scan as many record as possible in [p, q), taking quotes and escapes into account. */
static const char* scan_rec(const char* p, const char* q, int cnt,fstream_t*fs)
{
    int in_quote = 0, last_was_esc = 0;
    const char* last_complete = 0;
    int qc=fs->quote;
    int xc=fs->escape;
    int ch;
    
    if (xc == qc) 
	xc = 0;

    while (p < q && 0 < cnt) {
	ch = *p++;
	if (ch == '\n' && !in_quote) {
	    cnt--;
	    last_complete = p;
	    last_was_esc = 0;
	    continue;
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


/* free the file stream */
static void fstream_free(fstream_t* fs)
{
    int i;

    for (i = 0; i < fs->fn; i++) {
	if (fs->fd[i]) {
	    gfile_close(fs->fd[i]);
	    fs->fd[i] = 0;
	}
    }
    if (fs->fblock) {
	block_put(fs->fblock);
	fs->fblock = 0;
    }
}


/* alloc a new file stream */
static fstream_t* fstream_alloc(char** path, int npath, apr_pool_t* pool)
{
    int i;
    fstream_t* fs;

    if (0 == (fs = apr_palloc(pool, sizeof(fstream_t))))
	gfatal(FLINE, "out of memory");
    
    memset(fs, 0, sizeof(*fs));

    fs->fn = npath;
    fs->fpath = apr_palloc(pool, sizeof(char*) * npath);
    fs->fd = apr_palloc(pool, sizeof(gfile_t*) * npath);
    if (0 == fs->fpath || 0 == fs->fd) 
	gfatal(FLINE, "out of memory");

    memset(fs->fpath, 0, sizeof(char*) * npath);
    memset(fs->fd, 0, sizeof(gfile_t*) * npath);
	
    for (i = 0; i < npath; i++) {
	if (0 == (fs->fpath[i] = apr_pstrdup(pool, path[i])))
	    gfatal(FLINE, "out of memory");

	/* check READ access right */
	if (0 == (fs->fd[i] = gfile_open(path[i], O_RDONLY|O_BINARY, pool))) {
	    gwarning(FLINE, "unable to open file %s", path[i]);
	    fstream_free(fs);
	    return 0;
	} 
	
	if (i > 0) {
	    /* leave the first file opened */
	    gfile_close(fs->fd[i]);
	    fs->fd[i] = 0;
	}
    }
    fs->fidx = 0;
    fs->foff = 0;
    fs->fskip = opt.h;

    return fs;
}



/* get a block from the file stream */
static block_t* fstream_get_block(fstream_t* fs, apr_pool_t* pool)
{
    block_t* b;
    int n;
    char* p; 
    char* q;
    int skipfirst;
    
    if (fs->ferror || fs->fidx >= fs->fn)
	return 0;

    /* use residue block if available. otherwise, alloc new block. */
    if (0 != (b = fs->fblock)) {
	TR2(("popped foff = %" APR_INT64_T_FMT ", len %d, full foff %" APR_INT64_T_FMT "\n",
	     b->hdr.foff, b->top - b->bot, fs->foff));
	fs->fblock = 0;
    }
    else {
	b = block_get();
	b->hdr.foff = fs->foff;
    }

    /* on to next file? */
    if (fs->fd[fs->fidx] == 0) {
	if (b->bot != b->top) {
	    /* complete the previous file by returning the last residue block */
	    return b;
	}

	fs->fidx++;
	if (fs->fidx >= fs->fn)  {
	    block_put(b);
	    return 0;
	}

	fs->foff = b->hdr.foff = 0;
	fs->fskip = opt.h;

	if (opt.V) 
	    gprint("   opening %s\n", fs->fpath[fs->fidx]);
	if (0 == (fs->fd[fs->fidx] = gfile_open(fs->fpath[fs->fidx], O_RDONLY|O_BINARY, pool))) {
	    gwarning(FLINE, "unable to open file %s", fs->fpath[fs->fidx]);
	    fs->ferror = "unable to open file";
	    block_put(b);
	    return 0;
	}	
    }

    strncpy(b->hdr.fname, fs->fpath[fs->fidx], sizeof(b->hdr.fname) - 1);
    b->hdr.fname[sizeof(b->hdr.fname)-1] = 0;
    if (b->hdr.foff + (b->top - b->bot) != fs->foff) {
	gfatal(FLINE, "internal error: offset mismatch %" 
	       APR_INT64_T_FMT " + (%d - %d) != %" APR_INT64_T_FMT, 
	       b->hdr.foff, b->top, b->bot, fs->foff);
    }

    /* fill up b */
    while (0 != (n = sizeof(b->byte) - b->top)) {

	/* read into b */
	n = gfile_read(fs->fd[fs->fidx], b->byte + b->top, n);
	if (n < 0) {
	    fs->ferror = "cannot read file";
	    block_put(b);
	    return 0;
	}
	if (n == 0) {  /* current file EOF */
	    /* close this file */
	    gfile_close(fs->fd[fs->fidx]);
	    fs->fd[fs->fidx] = 0;
	    break;
	}
	b->top += n;
	fs->foff += n;

	/* way too verbose
	if (opt.V)
	    printf("[INFO] %s [off %lld]\n", fs->fpath[fs->fidx], fs->foff);
	*/
    }

    skipfirst = fs->fskip;
    fs->fskip = 0;

    /* find the split position */
    p = b->byte + b->bot;
    q = b->byte + b->top;
    if (skipfirst && p < q) {
	/* first record is header - skip it. */
	if (fs->is_csv) 
	    p = (char*) scan_rec(p, q, 1,fs);
	else {
	    for ( ; p < q && *p != '\n'; p++);
	    p = (p < q) ? p + 1 : 0;
	}
	if (!p)  
	    p = q;
	b->hdr.foff = p - b->byte;
	b->bot = p - b->byte;
	p = b->byte + b->bot;
	q = b->byte + b->top;
    }

    if (fs->fd[fs->fidx] == 0) {
	/* EOF, send the whole buffer */
	return b;
    }

    if (fs->is_csv) {
	/* go slow, scan byte-by-byte for record boundary */
	p = (char*) scan_rec(p, q, sizeof(b->byte) /*artificial max */,fs);
    }
    else {
	/* go fast, scan for \n for record boundary */
	/* find the last \n from the back */
	for (--q; p < q && *q != '\n'; q--);
	p = (p < q ? q + 1 : 0);
    }
    
    /* no record boundary 
       -> record larger than BLOCKSIZE? unacceptable!! */
    if (!p) {
	gwarning(FLINE, "ERROR: line too long in file %s (%d bytes)", 
		     fs->fpath[fs->fidx], b->top - b->bot);
	fs->ferror = "line too long in file";
	block_put(b);
	return 0;
    }

    /* split the block at record boundary */
    fs->fblock = block_split(b, p - (b->byte + b->bot));
    return b;
}



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
	char buf[1000];
	while (0 != fgets(buf, sizeof(buf) - 1, fp)) {
	    buf[sizeof(buf)-1] = 0;
	    puts(buf);
	}
	fclose(fp);
    }
    else {
	fprintf(stderr, "gpfdist -- file distribution web server\n\n");
	fprintf(stderr, "usage: gpfdist [-?v] [-p port] [-d dir] [-q qchar] [-x xchar] [-h] [-l logfn]\n\n");
	fprintf(stderr, 
		"        -?        : print this screen\n"
		"        -v        : verbose mode\n"
		"        -p port   : which port to serve HTTP. Default to 8080.\n"
		"        -d dir    : serve files under the specified directory.\n"
		"                    Default to '.'\n"
		"        -q qchar  : deprecated and ignored\n"
		"        -x xchar  : deprecated and ignored\n"
		"        -h        : flag indicates first data row in all files is\n"
		"                    a header row. skip it.\n"
		"        -l logfn  : log filename\n"
		"        --version : print version information.\n\n");
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

static void print_q_x_deprecated(void)
{
    fprintf(stderr,"-q and -x are deprecated.  Please specify these as in this example:\n");
    fprintf(stderr,"create external table a (a int) location ('gpfdist://...') format 'csv' (quote as '\"' escape as '\"');\n");
}


/* Parse command line */
static void parse_command_line(int argc, const char* const argv[], apr_pool_t* pool)
{
    apr_getopt_t* os;
    int ch;
    const char* arg;
    int e;
    static apr_getopt_option_t option[11] = {
	{      NULL, '?', 0, "print help screen" },
	{      NULL, 'V', 0, "very verbose" },
	{      NULL, 'v', 0, "verbose mode" },
	{      NULL, 'p', 1, "which port to serve HTTP" },
	{      NULL, 'd', 1, "serve files under this directory" },
	{      NULL, 'q', 1, "deprecated and ignored" },
	{      NULL, 'x', 1, "deprecated and ignored" },
	{      NULL, 'h', 0, "flag to skip first row in data files" },
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
	case 'h': opt.h = 1; break;
	case 'd': opt.d = arg; break;
	case 'q': print_q_x_deprecated(); break;
	case 'x': print_q_x_deprecated(); break;
	case 'p': opt.p = atoi(arg); break;
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
static void request_end(request_t* r, int error, const char* errmsg);
static const char* session_get_block(session_t* s, block_t** retblock);
static char* path_basename(const char* path, apr_pool_t* pool);


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
    const char* buf = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Content-length: 0\r\n"
	"Expires: 0\r\n"
	"X-GPFDIST-VERSION: " GP_VERSIONX "\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n\r\n";

    static int n = 0;
    int m;

    gprint("%s %s %s - OK\n", r->peer, r->in.req->argv[0], r->in.req->argv[1]);

    if (n == 0) 
	n = strlen(buf);
    m = send(r->sock, buf, n, 0);
    if (m != n) 
	gfatal(FLINE, "assert failed");
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

#if 0
// Unused function.
static void print_request(gnet_request_t* r)
{
    int i;
    for (i = 0; i < r->argc; i++) {
        gprint("  arg[%d] = %s\n", i, r->argv[i]);
    }

    for (i = 0; i < r->hc; i++) {
        gprint("  hdr[%s] = %s\n", r->hname[i], r->hvalue[i]);
    }
}
#endif

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
    if (r->out.block) {
	gwarning(FLINE, "request failure resulting in session failure");
	if (s) 
	    session_end(s, 1);
	block_put(r->out.block);
	r->out.block = 0;
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


static int local_send(int fd, const char* buf, int buflen)
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

static void gp1_send_eof(request_t* r)
{
    static char EOFSTR[5] = { 'D', 0, 0, 0, 0 };
    char* p = EOFSTR;
    char* q = EOFSTR + sizeof(EOFSTR);
    while (p < q) {
	int n = q - p;
	n = local_send(r->sock, p, n);
	if (n < 0) 
	    return;
	p += n;
    }
    TR(("[%d] sent EOF\n", r->sock));
}


static void gp1_send_errmsg(request_t* r, const char* errmsg)
{
    char hdr[5];
    apr_int32_t len = strlen(errmsg);
    const char* p;
    const char* q;
    
    hdr[0] = 'E';
    len = htonl(len);
    memcpy(&hdr[1], &len, 4);

    p = hdr;
    q = p + sizeof(hdr);
    while (p < q) {
	int n = q - p;
	n = local_send(r->sock, p, n);
	if (n < 0) 
	    return;
	p += n;
    }

    p = errmsg;
    q = p + strlen(p);
    while (p < q) {
	int n = q - p;
	n = local_send(r->sock, p, n);
	if (n < 0) 
	    return;
	p += n;
    }
}



/* Get a block out of the session. return error string. */
static const char* session_get_block(session_t* s, block_t** retblock)
{
    block_t* b;

    *retblock = 0;
    if (s->is_error || 0 == s->fstream) {
	session_end(s, 0);
	return 0;
    }

    if (0 == (b = fstream_get_block(s->fstream, s->pool))) {
	const char* ferror = s->fstream->ferror;
	if (ferror) {
	    session_end(s, 1);
	    return ferror;
	}
	session_end(s, 0);
	return 0;
    }
    *retblock = b;
    return 0;
}


/* finish the session - close the file */
static void session_end(session_t* s, int error)
{
    if (error)
	s->is_error = error;

    if (s->fstream) {
	fstream_free(s->fstream);
	s->fstream = 0;
    }
}

static int fpath_compare(const void *a, const void *b)
{
    return strcmp(*((char **) a), *((char **) b));
}

static char** glob(char* path, int* retcnt, apr_pool_t* pool)
{
    apr_array_header_t* arr;
    char** elts;
    int i, n;
    char** fpath = 0;
    char* bname = path_basename(path, pool);
    char** p;

    if (apr_match_glob(path, &arr, pool)) 
	return 0;
    n = arr->nelts;
    fpath = apr_pcalloc(pool, sizeof(char*) * (n + 1));
    elts = (char**) arr->elts;
    p = fpath;
    for (i = 0; i < n; i++) {
	if (elts[i][0] == '.') 	/* skip '.' & '..' & '.xxx'  */
	    continue;
	if (apr_filepath_merge(p, bname, elts[i], 0, pool))
	    return 0;
	p++;
    }
    
    if (retcnt) 
	*retcnt = p - fpath;


    qsort(fpath, *retcnt, sizeof(char *), fpath_compare);
    return fpath;
}

static int fpath_all_directories(char **fpath, int n, apr_pool_t *pool)
{
    int i;

    for (i = 0 ; i < n ; i++) {
	/* check if fpath is a directory */
	apr_finfo_t finfo;
	if (apr_stat(&finfo, fpath[i], APR_FINFO_TYPE, pool)) {
	    /* error in stat. assume non-directory */
	    return 0; 
	}
	else if (finfo.filetype != APR_DIR) {
	    /* not a directory */
	    return 0; 
	}
    }
    
    return 1;
}

static char **fpath_merge(char **fpath1, int n1, char **fpath2, int n2, int *n, apr_pool_t *p)
{
    char **fpath;
    
    if (n1 == 0) {
	*n = n2;
	return fpath2;
    }

    if (0 == (fpath = apr_palloc(p, sizeof(char *) * (n1 + n2)))) 
	gfatal(FLINE, "out of memory");

    *n = n1 + n2;

    memcpy(fpath, fpath1, sizeof(char*) * n1);
    memcpy(fpath+n1, fpath2, sizeof(char*) * n2);

    return fpath;
}


/* deallocate session, remove from hashtable */
static void session_free(session_t* s)
{
    if (opt.v) 
	gprint("free session %s\n", s->key);

    if (s->fstream) {
	fstream_free(s->fstream);
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
	if (opt.v) 
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
	int n = 0;
	char** fpath = 0;
	fstream_t* fstream = 0;
	apr_pool_t* pool;

	/* remove any outdated sessions*/
	sessions_cleanup();

	if (apr_pool_create(&pool, gcb.pool)) {
	    gwarning(FLINE, "out of memory");
	    http_error(r, 500, "internal error - out of memory");
	    request_end(r, 1, 0);
	    return;
	}

	/* do we have a pattern in r->path? */
	if (! apr_fnmatch_test(r->path)) {
	    n = 1;
	    fpath = &r->path;
	}
	else {
	    /* glob for all files matching r->path pattern */
	    fpath = glob(r->path, &n, pool);
	    if (!fpath || n == 0) {
		gwarning(FLINE, "bad path: %s", r->path);
		http_error(r, 404, "not found");
		request_end(r, 1, 0);
		apr_pool_destroy(pool);
		return;
	    }
	    if (opt.v) {
		int i;
		const int dlen = strlen(opt.d);
		gprint("  glob root at %s\n", opt.d);
		for (i = 0; i < n; i++) {
		    if (0 == memcmp(opt.d, fpath[i], dlen))
			gprint("  glob: %s\n", fpath[i] + dlen);
		    else
			gprint("  glob: %s\n", fpath[i]);
		}
	    }
	}

				    
	if (fpath_all_directories(fpath, n, pool)) {
	    int i;
	    int nn = 0;
	    char **ffpath = 0;

	    for (i = 0  ; i < n ; i++) {
		/* this is a directory - glob for all files in the dir */
		char* newpath = 0;
		char **dirfpath = 0;
		int dirnf = 0;
		if (apr_filepath_merge(&newpath, fpath[i], "*", 0, pool)) {
		    gwarning(FLINE, "bad path: %s (filepathmerge)", r->path);
		    http_error(r, 404, "not found");
		    request_end(r, 1, 0);
		    apr_pool_destroy(pool);
		    return;
		}
		dirfpath = glob(newpath, &dirnf, pool);
		if (!dirfpath || dirnf == 0) {
		    gwarning(FLINE, "%s is empty directory", fpath[i]);
		    continue;
		}

		/* append dirfpath to fpath */
		ffpath = fpath_merge(ffpath, nn, dirfpath, dirnf, &nn, pool);
	    }

	    fpath = ffpath;
	    n = nn;

	    if (n == 0) {
		gwarning(FLINE, "bad path: %s", r->path);
		http_error(r, 404, "not found");
		request_end(r, 1, 0);
		apr_pool_destroy(pool);
		return;
	    }
	}

	/* try opening the fstream */
	fstream = fstream_alloc(fpath, n, pool);
	if (! fstream) {
	    gwarning(FLINE, "reject request from %s, path %s\n", r->peer, r->path);
	    http_error(r, 404, "not found");
	    request_end(r, 1, 0);
	    apr_pool_destroy(pool);
	    return;
	}
	/* parse csvopt header */
	{
	    const char*a;
	    for (a=r->csvopt; *a; a++)
		if (*a=='q' && a[1]){
		    fstream->quote = *++a;
		    fstream->is_csv = 1;
		}else if (*a=='x' && a[1]){
		    fstream->escape = *++a;
		    fstream->is_csv = 1;
		}else
		    assert(0);
	}
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

	if (opt.v) 
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

    if (opt.v) 
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
    if (0 != http_ok(r)) {
	request_end(r, 1, 0);
	return;
    }


    return;
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
	if (! r->out.block) {
	    const char* ferror = session_get_block(r->session, &r->out.block);
	    if (ferror) {
		request_end(r, 1, ferror);
		return;
	    }
	    if (! r->out.block) {
		request_end(r, 0, 0);
		return;
	    }
	}

	b = r->out.block;
	if (b->bot == b->top) {
	    /* skip empty file */
	    block_put(r->out.block);
	    r->out.block = 0;
	    continue;
	}
	if (r->gp_proto == 1) {
	    if (b->hdr.htop == 0) 
		block_fill_header(b);

	    n = b->hdr.htop - b->hdr.hbot;
	    if (n > 0) {
		n = local_send(fd, b->hdr.hbyte + b->hdr.hbot, n);
		if (n < 0) {
		    perror("send");
		    block_put(r->out.block);
		    r->out.block = 0;
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

	if (b->hdr.htop != b->hdr.hbot) 
	    gfatal(FLINE, "internal error: hbot != htop");

	/* write out the block */
	n = b->top - b->bot;
	/*
	if (n == 0) 
	    gfatal(FLINE, "internal error: 0 bytes");
	*/

	n = local_send(fd, b->byte + b->bot, n);
	if (n < 0) {
	    perror("send");
	    block_put(r->out.block);
	    r->out.block = 0;
	    request_end(r, 1, 0);
	    return;
	}
	TR(("[%d] send data bytes off %" APR_INT64_T_FMT "  buf %d .. %d (top %d)\n", 
	    fd, b->hdr.foff, b->bot, b->bot + n, b->top));
	b->bot += n, b->hdr.foff += n;

	if (b->top != b->bot) {	/* network chocked */
	    TR(("[%d] network full\n", fd));
	    break;
	}
	block_put(r->out.block);
	r->out.block = 0;
    }

    /* Set up for this routine to be called again */
    if (setup_write(r)) 
	request_end(r, 1, 0);
    return;
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
    if (opt.v) 
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
    for (p = r->path; *p == '/'; p++);
    r->path = p;

    /*   - forbid .. dot dot  */
    for (p = r->path; 0 != (p = strchr(p, '.')); p++) {
        if (p[1] == '.') {
            gwarning(FLINE, "reject invalid request from %s [%s %s]",
                         r->peer, r->in.req->argv[0], r->in.req->argv[1]);
            http_error(r, 400, "invalid request");
            request_end(r, 1, 0);
            return;
        }
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

	    if (r->in.req->hname[i][0] == 'X') {
		TR(("%s: %s\n", r->in.req->hname[i], r->in.req->hvalue[i]));
	    }

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

    r->tm.tv_sec = 0;
    r->tm.tv_usec = 2000 * 1000;
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
    a.sin_port = htons(opt.p);
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

    /* bind */
    if (bind(f, (void*) &a, sizeof(a))) 
	gfatal(FLINE, "cannot create socket on port %d", opt.p);

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

    /* kqueue not so stable in libevent */
    setenv("EVENT_NOKQUEUE", "1", 1);
    
    event_init();

    http_setup();

    printf("Serving HTTP on port %d, directory %s\n", opt.p, opt.d);

    /* redirect stderr and stdout to log */
    if (opt.l) {
	freopen(opt.l, "a", stderr);
	setlinebuf(stderr);
	freopen(opt.l, "a", stdout);
	setlinebuf(stdout);
    }

    event_dispatch();

    return 0;
}

static char* path_basename(const char* path, apr_pool_t* pool)
{
    char* idx = strrchr(path, '/');
    if (! idx) 
	return apr_pstrdup(pool, ".");

    return apr_pstrndup(pool, path, idx - path);
}

static APR_DECLARE(gnet_request_t*) gnet_parse_request(const char* buf, int* len, apr_pool_t* pool)
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


static APR_DECLARE(char*) gstring_trim(char* s)
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

APR_DECLARE(int) gprint(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[%s] ", datetime());
    vfprintf(stdout, fmt, ap);
    return 0;
}


APR_DECLARE(int) gfatal(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [INTERNAL ERROR %s] ", datetime(), fline);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n          ... exiting\n"); 
    exit(1);
    return 0;
}


APR_DECLARE(int) gwarning(const char* fline, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] [WRN %s] ", datetime(), fline); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}
