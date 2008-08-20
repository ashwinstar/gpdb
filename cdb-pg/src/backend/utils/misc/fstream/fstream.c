#include <fstream/fstream.h>
#include <fstream/gfile.h>
#include <assert.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fstream_t{
    glob_t glob;
    gfile_t*fd;
    int fidx;		/* current index in ffd[] */
    int64_t foff;		/* current offset in ffd[fidx] */
    int fskip;		/* reflect opt.h (skip first line) */
    const char*ferror;	/* error string */
    struct fstream_options options;
    int buffer_size;
    char buffer[FSTREAM_BLOCKSIZE];
};

const char*
fstream_get_error(fstream_t*fs)
{
    return fs->ferror;
}

/* Scan as many record as possible in [p, q), taking quotes and escapes into account. */
static char*
scan_rec(char*p,char*q,int cnt,fstream_t*fs)
{
    int in_quote=0,last_was_esc=0;
    char*last_complete=0;
    int qc=fs->options.quote;
    int xc=fs->options.escape;
    while (p<q && cnt){
	int ch=*p++;
	if (in_quote){
	    if (!last_was_esc){
		if (ch == qc) 
		    in_quote = 0;
		else if (ch == xc) 
		    last_was_esc = 1;
	    }else
		last_was_esc = 0;
	}else if (ch=='\n'){
	    --cnt;
	    last_complete = p;
	}else if (ch == qc)
	    in_quote = 1;
    }
    return last_complete;
}


/* close the file stream */
void
fstream_close(fstream_t* fs)
{
    globfree(&fs->glob);
    if (fs->fd)
	gfile_close(fs->fd);
    free(fs);
}

static int
fpath_all_directories(const glob_t*glob)
{
    int i;
    for (i=0; i<glob->gl_pathc; i++) {
	const char*a=glob->gl_pathv[i];
	if (!*a || a[strlen(a)-1]!='/')
	    return 0;
    }
    return 1;
}

static int
expand_directories(fstream_t*fs)
{
    glob_t g=fs->glob;
    int f=0,i,j,ret=0;
    for (i=0; i<g.gl_pathc; i++){
	const char*a=g.gl_pathv[i];
	char*b=malloc(2*strlen(a)+2),*c;
	if (!b){
	    fprintf(stderr,"fstream out of memory\n");
	    ret = 1;
	    break;
	}
	for (c=b; *a; a++){
	    if (*a=='?' || *a=='*' || *a=='[')
		*c++ = '\\';
	    *c++ = *a;
	}
	*c++ = '*';
	*c++ = 0;
	j = glob(b,f,0,&fs->glob);
	free(b);
	if (j == GLOB_NOMATCH)
	    fprintf(stderr,"fstream %s is empty directory\n",g.gl_pathv[i]);
	else if (j){
	    fprintf(stderr,"fstream glob failed\n");
	    ret = 1;
	    break;
	}
	f = GLOB_APPEND;
    }
    if (f)
	globfree(&g);
    return ret;
}

/* alloc a new file stream */
fstream_t*
fstream_open(const char*path,const struct fstream_options*options)
{
    int i;
    fstream_t*fs;
    if (0 == (fs=malloc(sizeof*fs))){
	fprintf(stderr,"fstream out of memory\n");
	return 0;
    }
    memset(fs,0,sizeof*fs);
    fs->options = *options;
    if (glob(path,GLOB_MARK|GLOB_NOCHECK,0,&fs->glob)){
	fprintf(stderr,"fstream glob failed\n");
	fstream_close(fs);
	return 0;
    }
    if (fpath_all_directories(&fs->glob))
	if (expand_directories(fs)){
	    fstream_close(fs);
	    return 0;
	}
    if (fs->glob.gl_pathc == 0){
	fprintf(stderr,"fstream bad path: %s\n",path);
	fstream_close(fs);
	return 0;
    }
    for (i=0; i<fs->glob.gl_pathc; i++) {
	gfile_t*fd;
	/* check READ access right */
	if (0 == (fd=gfile_open(fs->glob.gl_pathv[i]))) {
	    fprintf(stderr,"fstream unable to open file %s\n",fs->glob.gl_pathv[i]);
	    fstream_close(fs);
	    return 0;
	} 
	if (i > 0) 
	    /* leave the first file opened */
	    gfile_close(fd);
	else
	    fs->fd = fd;
    }
    fs->fskip = fs->options.header;
    return fs;
}

int
fstream_read(fstream_t*fs,void*buffer,int size,struct fstream_filename_and_offset*fos,int read_whole_lines)
{
    size_t s;
    if (fs->ferror)
	return -1;
    if (fs->fidx==fs->glob.gl_pathc)
	return 0;
    for (;;){
	char*p,*q;
	assert(fs->buffer_size < sizeof fs->buffer);
	s = gfile_read(fs->fd,fs->buffer+fs->buffer_size,sizeof fs->buffer-fs->buffer_size);
	if (s < 0){
	    fs->ferror = "cannot read file";
	    return -1;
	}
	fs->buffer_size += s;
	p = fs->buffer;
	q = fs->buffer+fs->buffer_size;
	if (fs->fskip){
	    if (fs->options.is_csv)
		p = scan_rec(p,q,1,fs);
	    else {
		for (; p<q&&*p!='\n'; p++);
		p = p<q?p+1:0;
	    }
	    if (!p){
		if (fs->buffer_size == sizeof fs->buffer){
		    fprintf(stderr,"fstream ERROR: header too long in file %s\n",fs->glob.gl_pathv[fs->fidx]);
		    fs->ferror = "line too long in file";
		    return -1;
		}
		p = q;
	    }
	    fs->foff += p-fs->buffer;
	    memmove(fs->buffer,p,fs->buffer_size=q-p);
	    fs->fskip = 0;
	    continue;
	}
	if (!fs->buffer_size){
	    gfile_close(fs->fd);
	    fs->fd = 0;
	    fs->fidx++;
	    if (fs->fidx == fs->glob.gl_pathc)
		return 0;
	    fs->foff = 0;
	    fs->fskip = fs->options.header;
	    if (0 == (fs->fd=gfile_open(fs->glob.gl_pathv[fs->fidx]))){
		fprintf(stderr,"fstream unable to open file %s\n",fs->glob.gl_pathv[fs->fidx]);
		fs->ferror = "unable to open file";
		return -1;
	    }	
	    continue;
	}
	if (read_whole_lines){
	    assert(size>=FSTREAM_BLOCKSIZE);
	    if (fs->options.is_csv)
		/* go slow, scan byte-by-byte for record boundary */
		p = scan_rec(p,q,sizeof(fs->buffer)/*artificial max*/,fs);
	    else{
		/* go fast, scan for \n for record boundary */
		/* find the last \n from the back */
		for (p=q; fs->buffer<=--p&&*p!='\n';);
		p = fs->buffer<=p?p+1:0;
	    }
	    if (!p){
		if (fs->buffer_size == sizeof fs->buffer){
		    fprintf(stderr,"fstream ERROR: line too long in file %s (%lld bytes)\n",fs->glob.gl_pathv[fs->fidx],(long long)fs->foff);
		    fs->ferror = "line too long in file";
		    return -1;
		}
		p = fs->buffer+fs->buffer_size;
	    }
	}else
	    p = fs->buffer+fs->buffer_size;
	if (fos){
	    fos->foff = fs->foff;
	    strncpy(fos->fname,fs->glob.gl_pathv[fs->fidx],sizeof fos->fname);
	    fos->fname[sizeof fos->fname-1] = 0;
	}
	s = p-fs->buffer;
	if (s > size)
	    s = size;
	memcpy(buffer,fs->buffer,s);
	fs->foff += s;
	memmove(fs->buffer,fs->buffer+s,fs->buffer_size-=s);
	return s;
    }
}
