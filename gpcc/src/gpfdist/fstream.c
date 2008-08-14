#include "header.h"
#include <apr_file_info.h>
#include <apr_fnmatch.h>
#include <apr_tables.h>

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
    if (m < 0)
	gfatal(FLINE, "assert failed, n = %d, bot %d, top %d", n, b->bot, b->top);

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
void
fstream_free(fstream_t* fs)
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

    if (0 == (fpath = apr_palloc(p, sizeof(char *) * (n1 + n2)))) 
	gfatal(FLINE, "out of memory");

    *n = n1 + n2;

    memcpy(fpath, fpath1, sizeof(char*) * n1);
    memcpy(fpath+n1, fpath2, sizeof(char*) * n2);

    return fpath;
}


/* alloc a new file stream */
fstream_t*
fstream_alloc(const char*path, apr_pool_t* pool,int opth)
{
    int i,npath;
    fstream_t* fs;
    char**fpath;

    if (0 == (fs = apr_palloc(pool, sizeof(fstream_t))))
	gfatal(FLINE, "out of memory");
    
    memset(fs, 0, sizeof(*fs));

	/* do we have a pattern in path? */
	if (! apr_fnmatch_test(path)) {
	    npath = 1;
	    fpath = (char**)&path;
	}
	else {
	    /* glob for all files matching path pattern */
	    fpath = glob(path, &npath, pool);
	    if (!fpath || npath == 0) {
		gwarning(FLINE, "bad path: %s", path);
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
		    gwarning(FLINE, "bad path: %s (filepathmerge)", path);
		    return 0;
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
	    npath = nn;

	    if (npath == 0) {
		gwarning(FLINE, "bad path: %s", path);
		return 0;
	    }
	}


    fs->fn = npath;
    fs->fpath = apr_palloc(pool, sizeof(char*) * npath);
    if (0 == fs->fpath)
	gfatal(FLINE, "out of memory");

    memset(fs->fpath, 0, sizeof(char*) * npath);
	
    for (i = 0; i < npath; i++) {
	gfile_t*fd;
	if (0 == (fs->fpath[i] = apr_pstrdup(pool, fpath[i])))
	    gfatal(FLINE, "out of memory");

	/* check READ access right */
	if (0 == (fd=gfile_open(fpath[i],pool))) {
	    gwarning(FLINE, "unable to open file %s", fpath[i]);
	    fstream_free(fs);
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
    fs->fskip = fs->opth = opth;

    return fs;
}



/* get a block from the file stream */
int
fstream_get_block(fstream_t* fs, apr_pool_t* pool,block_t*b)
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
	fs->fskip = fs->opth;

	if (0 == (fs->fd = gfile_open(fs->fpath[fs->fidx],pool))) {
	    gwarning(FLINE, "unable to open file %s", fs->fpath[fs->fidx]);
	    fs->ferror = "unable to open file";
	    b->i = 0;
	    return 1;
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

    if (fs->fd == 0) {
	/* EOF, send the whole buffer */
	return 0;
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
	b->i = 0;
	return 1;
    }

    /* split the block at record boundary */
    block_split(&fs->fblock,b, p - (b->byte + b->bot));
    return 0;
}
