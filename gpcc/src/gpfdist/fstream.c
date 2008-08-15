#include "fstream.h"
#include "gfile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// We need to get rid of apr.
#include <apr_file_info.h>
#include <apr_fnmatch.h>
#include <apr_strings.h>
#include <apr_tables.h>

struct fstream_t {
    const char** fpath;		/* array of fpath */
    gfile_t*fd;
    int          fn;		/* #elements in ffd[] and fpath[] */
    int		 fidx;		/* current index in ffd[] */
    apr_uint64_t foff;		/* current offset in ffd[fidx] */
    int          fskip;		/* reflect opt.h (skip first line) */
    block_t     fblock;	/* residue block */
    const char*ferror;	/* error string */
    struct fstream_options options;
};

const char*
fstream_get_error(fstream_t*fs)
{
    return fs->ferror;
}

static char* path_basename(const char* path, apr_pool_t* pool)
{
    char* idx = strrchr(path, '/');
    if (! idx) 
	return apr_pstrdup(pool, ".");

    return apr_pstrndup(pool, path, idx - path);
}

static void
block_get(block_t*b)
{
    b->i = 1;
    b->bot = b->top = 0;
    b->hdr.fname[0] = 0;
    b->hdr.fname_len = 0;
    b->hdr.foff = 0;
    b->hdr.htop = b->hdr.hbot = 0;
}

/* split the block at byte n (relative to bot). 
 *   b - left side: byte[0:n]
 *   bb - right side: byte[n:]
 */
static void
block_split(block_t*bb,block_t* b, int n)
{
    int m = b->top - b->bot - n;
    assert(m>=0);

    if (m == 0)
	return;

    block_get(bb);
    memcpy(bb->byte, b->byte + b->bot + n, m);
    strcpy(bb->hdr.fname, b->hdr.fname);
    bb->hdr.foff = b->hdr.foff + n;
    b->top = b->bot + n;
    bb->top = m;
}

/* Scan as many record as possible in [p, q), taking quotes and escapes into account. */
static const char* scan_rec(const char* p, const char* q, int cnt,fstream_t*fs)
{
    int in_quote = 0, last_was_esc = 0;
    const char* last_complete = 0;
    int qc=fs->options.quote;
    int xc=fs->options.escape;
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


/* close the file stream */
void
fstream_close(fstream_t* fs)
{
    if (fs->fd) {
	gfile_close(fs->fd);
	fs->fd = 0;
    }
}

static int fpath_compare(const void *a, const void *b)
{
    return strcmp(*((char **) a), *((char **) b));
}

static char** glob(const char* path, int* retcnt, apr_pool_t* pool)
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

    if (0 == (fpath = apr_palloc(p, sizeof(char *) * (n1 + n2)))) {
	fprintf(stderr,"fstream out of memory\n");
	return 0;
    }

    *n = n1 + n2;

    memcpy(fpath, fpath1, sizeof(char*) * n1);
    memcpy(fpath+n1, fpath2, sizeof(char*) * n2);

    return fpath;
}


/* alloc a new file stream */
fstream_t*
fstream_open(const char*path, apr_pool_t* pool,const struct fstream_options*options)
{
    int i,npath;
    fstream_t* fs;
    char**fpath;

    if (0 == (fs = apr_palloc(pool, sizeof(fstream_t)))){
	fprintf(stderr,"fstream out of memory\n");
	return 0;
    }
    
    memset(fs, 0, sizeof(*fs));
    fs->options = *options;

	/* do we have a pattern in path? */
	if (! apr_fnmatch_test(path)) {
	    npath = 1;
	    fpath = (char**)&path;
	}
	else {
	    /* glob for all files matching path pattern */
	    fpath = glob(path, &npath, pool);
	    if (!fpath || npath == 0) {
		fprintf(stderr,"fstream bad path: %s\n",path);
		return 0;
	    }
	}
	if (fpath_all_directories(fpath, npath, pool)) {
	    int i;
	    int nn = 0;
	    char **ffpath = 0;

	    for (i = 0  ; i < npath ; i++) {
		/* this is a directory - glob for all files in the dir */
		char* newpath = 0;
		char **dirfpath = 0;
		int dirnf = 0;
		if (apr_filepath_merge(&newpath, fpath[i], "*", 0, pool)) {
		    fprintf(stderr,"fstream bad path: %s (filepathmerge)\n",path);
		    return 0;
		}
		dirfpath = glob(newpath, &dirnf, pool);
		if (!dirfpath || dirnf == 0) {
		    fprintf(stderr,"fstream %s is empty directory\n",fpath[i]);
		    continue;
		}

		/* append dirfpath to fpath */
		ffpath = fpath_merge(ffpath, nn, dirfpath, dirnf, &nn, pool);
		if (!ffpath)
		    return 0;
	    }

	    fpath = ffpath;
	    npath = nn;

	    if (npath == 0) {
		fprintf(stderr,"fstream bad path: %s\n",path);
		return 0;
	    }
	}


    fs->fn = npath;
    fs->fpath = apr_palloc(pool, sizeof(char*) * npath);
    if (0 == fs->fpath){
	fprintf(stderr,"fstream out of memory\n");
	return 0;
    }

    memset(fs->fpath, 0, sizeof(char*) * npath);
	
    for (i = 0; i < npath; i++) {
	gfile_t*fd;
	if (0 == (fs->fpath[i] = apr_pstrdup(pool, fpath[i]))){
	    fprintf(stderr,"fstream out of memory\n");
	    return 0;
	}

	/* check READ access right */
	if (0 == (fd=gfile_open(fpath[i]))) {
	    fprintf(stderr,"fstream unable to open file %s\n",fpath[i]);
	    fstream_close(fs);
	    return 0;
	} 
	if (i > 0) 
	    /* leave the first file opened */
	    gfile_close(fd);
	else
	    fs->fd = fd;
    }
    fs->fidx = 0;
    fs->foff = 0;
    fs->fskip = fs->options.header;

    return fs;
}



/* get a block from the file stream */
int
fstream_read(fstream_t* fs, apr_pool_t* pool,block_t*b)
{
    int n;
    char* p; 
    char* q;
    int skipfirst;
    
    if (fs->ferror || fs->fidx >= fs->fn)
	return 1;

    /* use residue block if available. otherwise, alloc new block. */
    if (fs->fblock.i) {
	*b = fs->fblock;
	fs->fblock.i = 0;
    }
    else {
	block_get(b);
	b->hdr.foff = fs->foff;
    }

    /* on to next file? */
    if (fs->fd == 0) {
	if (b->bot != b->top) {
	    /* complete the previous file by returning the last residue block */
	    return 0;
	}

	fs->fidx++;
	if (fs->fidx >= fs->fn)  {
	    b->i = 0;
	    return 1;
	}

	fs->foff = b->hdr.foff = 0;
	fs->fskip = fs->options.header;

	if (0 == (fs->fd = gfile_open(fs->fpath[fs->fidx]))) {
	    fprintf(stderr,"fstream unable to open file %s\n",fs->fpath[fs->fidx]);
	    fs->ferror = "unable to open file";
	    b->i = 0;
	    return 1;
	}	
    }

    strncpy(b->hdr.fname, fs->fpath[fs->fidx], sizeof(b->hdr.fname) - 1);
    b->hdr.fname[sizeof(b->hdr.fname)-1] = 0;
    assert(b->hdr.foff + (b->top - b->bot) == fs->foff);

    /* fill up b */
    while (0 != (n = sizeof(b->byte) - b->top)) {

	/* read into b */
	n = gfile_read(fs->fd, b->byte + b->top, n);
	if (n < 0) {
	    fs->ferror = "cannot read file";
	    b->i = 0;
	    return 1;
	}
	if (n == 0) {  /* current file EOF */
	    /* close this file */
	    gfile_close(fs->fd);
	    fs->fd = 0;
	    break;
	}
	b->top += n;
	fs->foff += n;
    }

    skipfirst = fs->fskip;
    fs->fskip = 0;

    /* find the split position */
    p = b->byte + b->bot;
    q = b->byte + b->top;
    if (skipfirst && p < q) {
	/* first record is header - skip it. */
	if (fs->options.is_csv) 
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

    if (fs->fd == 0) {
	/* EOF, send the whole buffer */
	return 0;
    }

    if (fs->options.is_csv) {
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
	fprintf(stderr,"fstream ERROR: line too long in file %s (%d bytes)\n", 
		     fs->fpath[fs->fidx], b->top - b->bot);
	fs->ferror = "line too long in file";
	b->i = 0;
	return 1;
    }

    /* split the block at record boundary */
    block_split(&fs->fblock,b, p - (b->byte + b->bot));
    return 0;
}
