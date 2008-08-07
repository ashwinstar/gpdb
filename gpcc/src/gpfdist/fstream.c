#include "header.h"

static block_t*gcbdotfreelist;

/* Put the block back into freelist */
void
block_put(block_t* b)
{
    b->__next = gcbdotfreelist;
    gcbdotfreelist = b;
}

/* Get a block from freelist if possible, otherwise, malloc */
static block_t*
block_get(void)
{
    block_t* b;
    if (0 != (b = gcbdotfreelist)) {
	gcbdotfreelist = b->__next;
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

/* split the block at byte n (relative to bot). 
 *   b - left side: byte[0:n]
 *   bb - right side: byte[n:]
 */
static block_t*
block_split(block_t* b, int n)
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
void
fstream_free(fstream_t* fs)
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
fstream_t*
fstream_alloc(char** path, int npath, apr_pool_t* pool,int opth)
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
    fs->fskip = fs->opth = opth;

    return fs;
}



/* get a block from the file stream */
block_t*
fstream_get_block(fstream_t* fs, apr_pool_t* pool)
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
	fs->fskip = fs->opth;

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
