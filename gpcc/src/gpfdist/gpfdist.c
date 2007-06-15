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
#define SHUT_RD SD_RECEIVE
#endif

#include <signal.h>
#include <gpcc.h>
#include <apr_getopt.h>
#include <apr_strings.h>
#include <gpcc_strings.h>
#include <apr_hash.h>
#include <apr_file_info.h>
#include <apr_time.h>
#include <apr_uri.h>
#include <apr_env.h>
#include <apr_fnmatch.h>
#include <gpcc_net.h>

#include <event.h>
#define BLOCKSIZE  (1024 * 32)

#ifndef GP_VERSION
#define GP_VERSION unknown
#endif

#define GP_VERSIONX APR_STRINGIFY(GP_VERSION)

#ifndef WIN32
#define SOCKET int
#define closesocket(x)   close(x)
#endif

/*  Struct of command line options */
struct {
    int p;			/* port */
    int v;			/* verbose */
    int V;			/* very verbose */
    int h;			/* skip first-row header */
    const char* d;		/* directory */
    const char* q;		/* quote char */
    const char* x;		/* escape char */
} opt = { 8080, 0, 0, 0, ".", 0, 0 };


/*  A data block */
typedef struct block_t block_t;
struct block_t {
    int      bot, top;
    block_t* __next;		/* chaining for freelist */
    char     byte[BLOCKSIZE - 16];
};

/*  Global constrol block */
struct {
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
    int*         ffd;		/* array of fd */
    int          fn;		/* #elements in ffd[] and fpath[] */
    int		 fidx;		/* current index in ffd[] */
    apr_uint64_t foff;		/* current offset in ffd[fidx] */
    int          fskip;		/* reflect opt.h (skip first line) */
    block_t*     fblock;	/* residue block */
    int 	 ferror;	/* error flag */
};


/*  A session */
typedef struct session_t session_t;
struct session_t {
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
    SOCKET sock;		/* the socket */
    apr_pool_t* pool;		/* memory pool container */
    struct event ev;		/* event we are watching for this request */
    const char* peer;		/* peer IP:port string */
    char* path;			/* path to file */
    const char* tid;		/* transaction id */
    session_t* session;		/* the session this request is attached to */

    struct {
	char* hbuf;		/* buffer for raw incoming HTTP request */
	int   hbuftop;		/* # bytes used in hbuf */
	int   hbufmax;		/* size of hbuf[] */
	gpcc_net_request_t* req; /* a parsed HTTP request, NULL if still incomplete. */
    } in;

    struct {
	block_t* block;		/* next block to send out */
    } out;
};


/* Get a block from freelist if possible, otherwise, malloc */
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


/* Put the block back into freelist */
void block_put(block_t* b)
{
    b->__next = gcb.freelist;
    gcb.freelist = b;
    gcb.freecnt++;
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


/* Scan as many record as possible in [p, q), taking quotes and escapes into account. */
const char* scan_rec(const char* p, const char* q, int cnt)
{
    int in_quote = 0, last_was_esc = 0;
    const char* last_complete = 0;
    int xc = opt.x[0];
    int qc = opt.q[0];
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
void fstream_free(fstream_t* fs)
{
    int i;
    for (i = 0; i < fs->fn; i++) {
	if (fs->ffd[i] >= 0) {
	    close(fs->ffd[i]);
	    fs->ffd[i] = 0;
	}
	free((char*)fs->fpath[i]);
    }
    if (fs->fblock) {
	block_put(fs->fblock);
	fs->fblock = 0;
    }
    
    free(fs->fpath);
    free(fs->ffd);
    free(fs);
}


/* alloc a new file stream */
fstream_t* fstream_alloc(char** path, int npath)
{
    int i;
    fstream_t* fs;

    if (0 == (fs = malloc(sizeof(fstream_t))))
	gpcc_fatal(FLINE, "out of memory");
    
    memset(fs, 0, sizeof(*fs));

    fs->fn = npath;
    fs->fpath = malloc(sizeof(char*) * npath);
    fs->ffd = malloc(sizeof(int) * npath);
    if (0 == fs->fpath || 0 == fs->ffd) 
	gpcc_fatal(FLINE, "out of memory");
	
    for (i = 0; i < npath; i++) {
	if (0 == (fs->fpath[i] = strdup(path[i])))
	    gpcc_fatal(FLINE, "out of memory");

	if (opt.V) 
	    printf("   opening %s\n", path[i]);
	if (-1 == (fs->ffd[i] = open(path[i], O_RDONLY, 0600))) {
	    gpcc_warning(FLINE, "unable to open file %s", path[i]);
	    fstream_free(fs);
	    return 0;
	}
    }
    fs->fidx = 0;
    fs->foff = 0;
    fs->fskip = opt.h;

    return fs;
}



/* get a block from the file stream */
block_t* fstream_get_block(fstream_t* fs)
{
    block_t* b;
    int n;
    char* p; 
    char* q;
    int skipfirst;
    int localEOF = 0;
    
    if (fs->ferror || fs->fidx >= fs->fn)
	return 0;

    /* use residue block if available */
    if (0 != (b = fs->fblock)) 
	fs->fblock = 0;
    else
	b = block_get();

    /* on to next file? */
    if (fs->ffd[fs->fidx] < 0) {
	fs->fidx++;
	fs->foff = 0;
	fs->fskip = opt.h;
	if (fs->fidx >= fs->fn)
	    return b;		/* return the residue block */
    }

    /* fill up b */
    while (0 != (n = sizeof(b->byte) - b->top)) {

	/* read into b */
	n = read(fs->ffd[fs->fidx], b->byte + b->top, n);
	if (n < 0) {
	    fs->ferror = 1;
	    return 0;
	}
	if (n == 0) {		/* current file EOF */
	    localEOF = 1;
	    /* close this file */
	    close(fs->ffd[fs->fidx]);
	    fs->ffd[fs->fidx] = -1;
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
    if (skipfirst)
	fs->fskip = 0;

    /* find the split position */
    p = b->byte + b->bot;
    q = b->byte + b->top;
    if (opt.q) {
	/* go slow, scan byte-by-byte for record boundary */
	if (skipfirst) {
	    if (0 != (p = (char*) scan_rec(p, q, 1)))
		b->bot = p - b->byte;
	}
	if (p) 
	    p = (char*) scan_rec(p, q, sizeof(b->byte) /*artificial max */);
    }
    else {
	/* go fast, scan for \n for record boundary */
	if (skipfirst) {
	    for ( ; p < q && *p != '\n'; p++);
	    p = (p < q) ? p + 1 : 0;
	    if (p) 
		b->bot = p - b->byte;
	}
	/* find the last \n from the back */
	if (p) {
	    for (--q; p < q && *q != '\n'; q--);
	    p = (p < q ? q + 1 : 0);
	}
    }

    
    /* no record boundary 
       -> record larger than BLOCKSIZE? unacceptable */
    if (!p) {
	gpcc_warning(FLINE, "ERROR: line too long in file %s (%d bytes)", 
		     fs->fpath[fs->fidx], b->top - b->bot);
	fs->ferror = 1;
	return 0;
    }

    /* split the block at record boundary */
    fs->fblock = block_split(b, p - (b->byte + b->bot));
    return b;
}



/* Print usage */
void usage(const char* msg)
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
	fprintf(stderr, "usage: gpfdist [-?v] [-p port] [-d dir] [-q qchar] [-x xchar] [-h]\n\n");
	fprintf(stderr, 
		"        -?        : print this screen\n"
		"        -v        : verbose mode\n"
		"        -p port   : which port to serve HTTP. Default to 8080.\n"
		"        -d dir    : serve files under the specified directory.\n"
		"                    Default to '.'\n"
		"        -q qchar  : set quote character. If not specified, the\n"
		"                    program will not parse for CSV, and will\n"
		"                    assume each record is separated by newline\n"
		"                    character that never occurs inside a record.\n"
		"        -x xchar  : set escape character (default to qchar)\n"
		"        -h        : flag indicates first data row in all files is\n"
		"                    a header row. skip it.\n"
		"        --version : print version information.\n\n");
    }

    if (msg)
	fprintf(stderr, "%s\n", msg);

    exit(msg ? 1 : 0);
}

void print_version()
{
    printf("gpfdist version \"%s\"\n", GP_VERSIONX);
    exit(0);
}


/* Parse command line */
void parse_command_line(int argc, const char* const argv[], apr_pool_t* pool)
{
    apr_getopt_t* os;
    int ch;
    const char* arg;
    int e;
    static apr_getopt_option_t option[10] = {
	{      NULL, '?', 0, "print help screen" },
	{      NULL, 'V', 0, "very verbose" },
	{      NULL, 'v', 0, "verbose mode" },
	{      NULL, 'p', 1, "which port to serve HTTP" },
	{      NULL, 'd', 1, "serve files under this directory" },
	{      NULL, 'q', 1, "set quote character" },
	{      NULL, 'x', 1, "set escape character" },
	{      NULL, 'h', 0, "flag to skip first row in data files" },
	{ "version", 256, 0, "print version number" }
    };
    
    
    if (0 != apr_getopt_init(&os, pool, argc, argv)) 
	gpcc_fatal(FLINE, "apt_getopt_init failed");

    while (0 == (e = apr_getopt_long(os, option, &ch, &arg))) {
	switch (ch) {
	case '?': usage(0); break;
	case 'v': opt.v = 1; break;
	case 'V': opt.v = opt.V = 1; break;
	case 'h': opt.h = 1; break;
	case 'd': opt.d = arg; break;
	case 'q': opt.q = arg; break;
	case 'x': opt.x = arg; break;
	case 'p': opt.p = atoi(arg); break;
	case 256: print_version(); break;
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
	    usage("Security Error: cannot run under root (/) directory");

	/* strip ending / */
	while (p[0] && p[strlen(p) - 1] == '/')
	    p[strlen(p) - 1] = 0;

	opt.d = p;

	/* check that the dir exists */
	if (0 != apr_filepath_set(opt.d, pool))
	    usage(apr_psprintf(pool, 
			       "Error: cannot access directory '%s'\n"
			       "Please specify a valid directory for -d switch", opt.d));
    }

    /* check quote and escape options */
    if (opt.q && ! (opt.q[0] && !opt.q[1])) 
	usage("Error: please specify a character for -q switch\n"
	      "       e.g. gpfdist -q '\"'");

    if (opt.x && ! (opt.x[0] && !opt.x[1]))
	usage("Error: please specify a character for -x switch\n"
	      "       e.g. gpfdist -q '\"' -x '\\'");

    if (!opt.q && opt.x)
	usage("Error: you must specify -q qchar with -x xchar");

    if (opt.q && !opt.x)
	opt.x = opt.q;

    /* there should not be any more args left */
    if (os->ind != argc) 
	usage("Error: illegal arguments");
}



int setup_read(request_t* r);
int setup_write(request_t* r);
void session_attach(request_t* r);
void session_detach(request_t* r);
void session_end(session_t* s, int error);
void request_end(request_t* r);
block_t* session_get_block(session_t* s);
char* path_basename(const char* path, apr_pool_t* pool);


/* send an error response */
void send_error(request_t* r, int code, const char* msg)
{
    char buf[1024];
    int n;
    printf("%s - %d %s\n", r->peer, code, msg);
    n = apr_snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n"
		     "Content-length: 0\r\n"
		     "Connection: close\r\n\r\n",
		     code, msg);

    send(r->sock, buf, n, 0);
}


/* send an empty response */
void send_empty(request_t* r)
{
    const char* buf = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Content-length: 0\r\n"
	"Connection: close\r\n\r\n";
    static int n = 0;
    int m;

    printf("%s - EMPTY\n", r->peer);

    if (n == 0) 
	n = strlen(buf);
    m = send(r->sock, buf, n, 0);
    if (m != n) 
	gpcc_fatal(FLINE, "assert failed");
}


/* send an OK response */
void send_ok(request_t* r)
{
    const char* buf = "HTTP/1.0 200 ok\r\n"
	"Content-type: text/plain\r\n"
	"Connection: close\r\n\r\n";
    static int n = 0;
    static int m;

    printf("%s - OK\n", r->peer);

    if (n == 0) 
	n = strlen(buf);
    m = send(r->sock, buf, n, 0);
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



/* Finished a request. Close socket and cleanup. */
void request_end(request_t* r)
{
    session_t* s = r->session;

    /* If we still have a block outstanding, the session is corrupted. */
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
    closesocket(r->sock);
    apr_pool_destroy(r->pool);
}





/* Get a block out of the session */
block_t* session_get_block(session_t* s)
{
    block_t* b;

    if (s->is_error || 0 == s->fstream)
	return 0;

    if (0 == (b = fstream_get_block(s->fstream))) {
	if (s->fstream->ferror)
	    session_end(s, 1);
	else
	    session_end(s, 0);
	return 0;
    }
    return b;
}


/* finish the session - close the file */
void session_end(session_t* s, int error)
{
    if (error)
	s->is_error = error;

    if (s->fstream) {
	fstream_free(s->fstream);
	s->fstream = 0;
    }
}


char** glob(char* path, int* retcnt, apr_pool_t* pool)
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
    return fpath;
}



void session_attach(request_t* r)
{
    char key[1024];
    session_t* s = 0;
    
    /* create the key */
    if (sizeof(key) -1 == apr_snprintf(key, sizeof(key), "%s:%s", r->tid, r->path)) {
	send_error(r, 400, "path too long");
	request_end(r);
	return;
    }

    /* check in session hashtable */
    s = apr_hash_get(gcb.session.tab, key, APR_HASH_KEY_STRING);
    if (! s) {
	/* not in hashtable - create new session */
	int n = 0;
	char** fpath = 0;
	fstream_t* fstream = 0;

	/* do we have a pattern in r->path? */
	if (! apr_fnmatch_test(r->path)) {
	    n = 1;
	    fpath = &r->path;
	}
	else {
	    /* glob for all files matching r->path pattern */
	    fpath = glob(r->path, &n, r->pool);
	    if (!fpath || n == 0) {
		gpcc_warning(FLINE, "bad path: %s", r->path);
		send_error(r, 404, "not found");
		request_end(r);
		return;
	    }
	}

	if (n == 1) {
	    /* check if fpath is a directory */
	    apr_finfo_t finfo;
	    if (apr_stat(&finfo, r->path, APR_FINFO_TYPE, r->pool)) {
		/* error in stat. assume non-directory */
		; 
	    }
	    else if (finfo.filetype != APR_DIR) {
		/* not a directory */
		; 
	    }
	    else {
		/* this is a directory - glob for all files in the dir */
		char* newpath = 0;
		if (apr_filepath_merge(&newpath, fpath[0], "*", 0, r->pool)) {
		    gpcc_warning(FLINE, "bad path: %s (filepathmerge)", r->path);
		    send_error(r, 404, "not found");
		    request_end(r);
		    return;
		}
		fpath = glob(newpath, &n, r->pool);
		if (!fpath || n == 0) {
		    gpcc_warning(FLINE, "bad path: %s", r->path);
		    send_error(r, 404, "not found");
		    request_end(r);
		    return;
		}
	    }
	}

	/* try opening the fstream */
	fstream = fstream_alloc(fpath, n);
	if (! fstream) {
	    gpcc_warning(FLINE, "reject request from %s, path %s\n", r->peer, r->path);
	    send_error(r, 404, "not found");
	    request_end(r);
	    return;
	}

	/* allocate session */
	if (0 == (s = malloc(sizeof(session_t)))) 
	    gpcc_fatal(FLINE, "out of memory");

	memset(s, 0, sizeof(*s));

	if (0 == (s->tid = strdup(r->tid)) 
	    || 0 == (s->path = strdup(r->path))
	    || 0 == (s->key = strdup(key))) {
	    gpcc_fatal(FLINE, "out of memory");
	}

	s->fstream = fstream;

	/* insert into hashtable */
	apr_hash_set(gcb.session.tab, s->key, APR_HASH_KEY_STRING, s);

	if (opt.v) 
	    printf("[INFO] new session (%s, %s)\n", s->path, s->tid);
    }

    /* found a session in hashtable*/

    /* if error, send an error and close */
    if (s->is_error) {
	send_error(r, 500, "session error");
	request_end(r);
	return;
    }

    /* session already ended. send an empty response, and close. */
    if (0 == s->fstream) {
	send_empty(r);
	request_end(r);
	return;
    }

    if (opt.v) 
	printf("[INFO] joined session (%s, %s)\n", s->path, s->tid);

    /* setup so receive EV_WRITE events to write to socket */
    if (setup_write(r)) {
	send_error(r, 500, "internal error");
	request_end(r);
	return;
    }

    /* one more request for session */
    s->nrequest++;
    s->mtime = apr_time_now();
    r->session = s;

    /* finally, tell peer that we are ready to send data */
    send_ok(r);
    return;
}


/* deallocate session, remove from hashtable */
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


/* detach a request from a session */
void session_detach(request_t* r)
{
    session_t* s = r->session;
    r->session = 0;
    if (s) {
	if (s->nrequest == 0) 
	    gpcc_fatal(FLINE, "assert failed");
	s->nrequest--;
	s->mtime = apr_time_now();

	/* for auto-tid sessions, we can free it now */
	if (0 == strncmp("auto-tid.", s->tid, 9)) {
	    if (s->nrequest != 0) 
		gpcc_fatal(FLINE, "assert failed");

	    session_free(s);
	}
    }
}



/* Callback when the socket is ready to be written */
void do_write(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n, i;
    block_t* b;

    if (fd != r->sock) 
	gpcc_fatal(FLINE, "assert failed");

    /* Loop at most 3 blocks or until we choke on the socket */
    for (i = 0; i < 3; i++) {

	/* get a block (or find a remaining block) */
	if (! r->out.block) {
	    r->out.block = session_get_block(r->session);
	    if (! r->out.block) {
		request_end(r);
		return;
	    }
	}

	/* write out the block */
	b = r->out.block;
	n = b->top - b->bot;
	n = send(fd, b->byte + b->bot, n, 0);
	if (n < 0) {
#ifdef WIN32
	    int e = WSAGetLastError();
	    int ok = (e == WSAEINTR || e == WSAEWOULDBLOCK);
#else
	    int e = errno;
	    int ok = (e == EINTR || e == EAGAIN);
#endif
	    if (! ok) {
		perror("send");
		block_put(r->out.block);
		r->out.block = 0;
		request_end(r);
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

    /* Set up for this routine to be called again */
    if (setup_write(r)) 
	request_end(r);
    return;
}


/* Callback when a socket is ready to be read. Read the 
 * socket for a complete HTTP request.
 */
void do_read(int fd, short event, void* arg)
{
    request_t* r = (request_t*) arg;
    int n, i;
    char* p;
    char* pp;

    /* If we timeout, close the request. */
    if (event & EV_TIMEOUT) {
	send_error(r, 400, "time out");
	request_end(r);
	return;
    }

    /* how many bytes left in the header buf */
    n = r->in.hbufmax - r->in.hbuftop;
    if (n <= 0) {
	send_error(r, 500, "internal error");
	request_end(r);
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
	    request_end(r);
	    return;
	}
    }
    else if (n == 0) {
	/* didn't read anything */
	;
    }
    else {
	/* check if a complete HTTP request is availble in header buf */
	r->in.hbuftop += n;
	n = r->in.hbuftop;
	r->in.req = gpcc_net_parse_request(r->in.hbuf, &n, r->pool);
	if (!r->in.req && r->in.hbuftop >= r->in.hbufmax) {
	    /* not available, but headerbuf is full - send error and close */
	    send_error(r, 400, "forbidden");
	    request_end(r);
	    return;
	}
    }

    /* if we don't yet have a complete request, set up this function to be
       called again for */
    if (!r->in.req) {
	if (setup_read(r)) {
	    send_error(r, 500, "internal error");
	    request_end(r);
	}
	return;
    }

    /* Hurray, got a request !!! */

    /* we don't need to read from the socket anymore */
    shutdown(r->sock, SHUT_RD);

    if (opt.V) {
	printf("[INFO] request");
	for (i = 0; i < r->in.req->argc; i++)
	    printf(" %s", r->in.req->argv[i]);
	printf("\n");
    }

    /* parse the HTTP request. Expect "GET /path HTTP/1.X" */
    if (r->in.req->argc != 3) {
	gpcc_warning(FLINE, "reject invalid request from %s", r->peer);
	send_error(r, 400, "invalid request");
	request_end(r);
	return;
    }
    if (0 != strncmp("HTTP/1.", r->in.req->argv[2], 7)) {
	gpcc_warning(FLINE, "reject invalid protocol from %s [%s]", r->peer, r->in.req->argv[2]);
	send_error(r, 400, "invalid request");
	request_end(r);
	return;
    }
    if (0 != strcmp("GET", r->in.req->argv[0])) {
	gpcc_warning(FLINE, "reject invalid request from %s [%s %s]", r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	send_error(r, 400, "invalid request");
	request_end(r);
	return;
    }

    /* scrub the path */
    /*   - remove / prefixes */
    for (p = r->in.req->argv[1]; *p == '/'; p++);


    /*   - make the new path relative to opt.d */
    if (apr_filepath_merge(&r->path, opt.d, p, 0, r->pool)) {
	gpcc_warning(FLINE, "reject invalid request from %s [%s %s]", r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	send_error(r, 400, "invalid request");
	request_end(r);
	return;
    }

    /*   - remove query (after ?) and fragment (after #) */
    if (0 != (p = strchr(r->path, '?')))
	*p = 0;

    if (0 != (p = strchr(r->path, '#')))
	*p = 0;

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

    /*   - forbid dot dot  */
    for (p = r->path; 0 != (p = strchr(p, '.')); p++) {
	if (p[1] == '.') {
	    gpcc_warning(FLINE, "reject invalid request from %s [%s %s]", 
			 r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	    send_error(r, 400, "invalid request");
	    request_end(r);
	    return;
	}
    }

    /* Extract X-GP-TID value from HTTP headers */
    {
	const char* xid = 0;
	const char* cid = 0;
	const char* sn = 0;
	for (i = 0; i < r->in.req->hc; i++) {
	    if (0 == strcmp("X-GP-XID", r->in.req->hname[i])) 
		xid = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-CID", r->in.req->hname[i]))
		cid = r->in.req->hvalue[i];
	    else if (0 == strcmp("X-GP-SN", r->in.req->hname[i]))
		sn = r->in.req->hvalue[i];
	}
	if (xid && cid && sn) {
	    r->tid = apr_psprintf(r->pool, "%s.%s.%s", xid, cid, sn);
	}
	else if (xid || cid || sn) {
	    gpcc_warning(FLINE, "reject invalid request from %s [%s %s] - missing X-GP-* header", 
			 r->peer, r->in.req->argv[0], r->in.req->argv[1]);
	    send_error(r, 400, "invalid request");
	    request_end(r);
	    return;
	}
	else {
	    r->tid = apr_psprintf(r->pool, "auto-tid.%d", gcb.session.gen++);
	}
    }

    if (opt.v) 
	printf("[INFO] %s requests %s\n\tTID %s\n", r->peer, r->path, r->tid);

    /* Attach to a session */
    session_attach(r);
}


/* Callback when the listen socket is ready to accept connections. */
void do_accept(int fd, short event, void* arg)
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
	gpcc_warning(FLINE, "accept failed");
	return;
    }

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

    /* create a pool container for this socket */
    if (apr_pool_create(&pool, gcb.pool)) 
	gpcc_fatal(FLINE, "out of memory");

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
	send_error(r, 500, "internal error");
	request_end(r);
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


/* Create HTTP port and start to receive request */
void http_setup(void)
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
	gpcc_fatal(FLINE, "cannot create socket on port %d", opt.p);

#ifndef WIN32
    if (fcntl(f, F_SETFD, 1) == -1) 
	gpcc_fatal(FLINE, "cannot create socket on port %d - fcntl(F_SETFD) failed", opt.p);

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
	gpcc_fatal(FLINE, "cannot create socket on port %d", opt.p);

    /* listen with a big queue */
    if (listen(f, 256)) 
	gpcc_fatal(FLINE, "cannot listen on port %d", opt.p);

    gcb.listen_sock = f;

    /* setup event priority */
    if (event_priority_init(10)) 
	gpcc_warning(FLINE, "event_priority_init failed");

    /* when this socket is ready, do accept */
    event_set(&gcb.listen_event, gcb.listen_sock, EV_READ | EV_PERSIST, do_accept, 0);

    /* high priority so we accept as fast as possible */
    if (event_priority_set(&gcb.listen_event, 0)) 
	gpcc_warning(FLINE, "event_priority_set failed");

    /* start watching this event */
    if (event_add(&gcb.listen_event, 0))
	gpcc_fatal(FLINE, "cannot set up event on listen socket");

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

    printf("Serving HTTP on port %d, directory %s\n", opt.p, opt.d);
    event_dispatch();

    return 0;
}

char* path_basename(const char* path, apr_pool_t* pool)
{
    char* idx = strrchr(path, '/');
    if (! idx) 
	return apr_pstrdup(pool, ".");

    return apr_pstrndup(pool, path, idx - path);
}

