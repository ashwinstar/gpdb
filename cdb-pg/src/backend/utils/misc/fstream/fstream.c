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
    int64_t line_number;
    int fskip;		/* skip first line */
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
scan_rec(char*p,char*q,int cnt,fstream_t*fs,int64_t*line_number)
{
    int in_quote=0,last_was_esc=0;
    char*last_complete=0;
    int qc=fs->options.quote;
    int xc=fs->options.escape;
    while (p<q && cnt){
	int ch=*p++;
	if (ch=='\n')
	    ++*line_number;
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
    gfile_free(fs);
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
	char*b=gfile_malloc(2*strlen(a)+2),*c;
	if (!b){
	    gfile_printf_then_putc_newline("fstream out of memory");
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
	gfile_free(b);
	if (j == GLOB_NOMATCH)
	    gfile_printf_then_putc_newline("fstream %s is empty directory",g.gl_pathv[i]);
	else if (j){
	    gfile_printf_then_putc_newline("fstream glob failed");
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
    if (0 == (fs=gfile_malloc(sizeof*fs))){
	gfile_printf_then_putc_newline("fstream out of memory");
	return 0;
    }
    memset(fs,0,sizeof*fs);
    fs->options = *options;
    if (glob(path,GLOB_MARK|GLOB_NOCHECK,0,&fs->glob)){
	gfile_printf_then_putc_newline("fstream glob failed");
	fstream_close(fs);
	return 0;
    }
    if (fpath_all_directories(&fs->glob))
	if (expand_directories(fs)){
	    fstream_close(fs);
	    return 0;
	}
    if (fs->glob.gl_pathc == 0){
	gfile_printf_then_putc_newline("fstream bad path: %s",path);
	fstream_close(fs);
	return 0;
    }
    for (i=0; i<fs->glob.gl_pathc; i++) {
	gfile_t*fd;
	/* check READ access right */
	if (0 == (fd=gfile_open(fs->glob.gl_pathv[i]))) {
	    gfile_printf_then_putc_newline("fstream unable to open file %s",fs->glob.gl_pathv[i]);
	    fstream_close(fs);
	    return 0;
	} 
	if (i > 0) 
	    /* leave the first file opened */
	    gfile_close(fd);
	else
	    fs->fd = fd;
    }
    fs->line_number = 1;
    fs->fskip = options->header;
    return fs;
}

static void
fillFo(fstream_t*fs,struct fstream_filename_and_offset**fo)
{
    if (*fo){
	(*fo)->foff = fs->foff;
	(*fo)->line_number = fs->line_number;
	strncpy((*fo)->fname,fs->glob.gl_pathv[fs->fidx],sizeof (*fo)->fname);
	(*fo)->fname[sizeof (*fo)->fname-1] = 0;
	*fo = 0;
    }
}

static void
nextFile(fstream_t*fs)
{
    gfile_close(fs->fd);
    fs->fd = 0;
    fs->foff = 0;
    fs->line_number = 1;
    fs->fidx++;
    if (fs->fidx < fs->glob.gl_pathc){
	fs->fskip = fs->options.header;
	if (0 == (fs->fd=gfile_open(fs->glob.gl_pathv[fs->fidx]))){
	    gfile_printf_then_putc_newline("fstream unable to open file %s",fs->glob.gl_pathv[fs->fidx]);
	    fs->ferror = "unable to open file";
	}
    }
}

int
fstream_read(fstream_t*fs,void*buffer,int size,struct fstream_filename_and_offset*fo,int read_whole_lines)
{
    int osize=size;
    size_t s;
    while (size && fs->fidx!=fs->glob.gl_pathc && !fs->ferror){
	char*p,*q;
	if (read_whole_lines || fs->fskip){
	    assert(fs->buffer_size < sizeof fs->buffer);
	    s = gfile_read(fs->fd,fs->buffer+fs->buffer_size,sizeof fs->buffer-fs->buffer_size);
	    if (s < 0){
		fs->ferror = "cannot read file";
		break;
	    }
	    fs->buffer_size += s;
	    p = fs->buffer;
	    q = fs->buffer+fs->buffer_size;
	    if (fs->fskip){
		if (fs->options.is_csv)
		    p = scan_rec(p,q,1,fs,&fs->line_number);
		else {
		    for (; p<q&&*p!='\n'; p++);
		    p = p<q?p+1:0;
		    fs->line_number++;
		}
		if (!p){
		    if (fs->buffer_size == sizeof fs->buffer){
			gfile_printf_then_putc_newline("fstream ERROR: header too long in file %s",fs->glob.gl_pathv[fs->fidx]);
			fs->ferror = "line too long in file";
			break;
		    }
		    p = q;
		}
		fs->foff += p-fs->buffer;
		memmove(fs->buffer,p,fs->buffer_size=q-p);
		fs->fskip = 0;
		continue;
	    }
	    if (!fs->buffer_size){
		nextFile(fs);
		continue;
	    }
	}
	if (read_whole_lines){
	    assert(size>=FSTREAM_BLOCKSIZE);
	    fillFo(fs,&fo);
	    if (fs->options.is_csv)
		/* go slow, scan byte-by-byte for record boundary */
		p = scan_rec(p,q,sizeof(fs->buffer)/*artificial max*/,fs,&fs->line_number);
	    else{
		/* go fast, scan for \n for record boundary */
		/* find the last \n from the back */
		for (p=q; fs->buffer<=--p&&*p!='\n';);
		p = fs->buffer<=p?p+1:0;
		fs->line_number = 0;
	    }
	    if (!p){
		if (fs->buffer_size == sizeof fs->buffer){
		    gfile_printf_then_putc_newline("fstream ERROR: line too long in file %s (%lld bytes)",fs->glob.gl_pathv[fs->fidx],(long long)fs->foff);
		    fs->ferror = "line too long in file";
		    break;
		}
		p = fs->buffer+fs->buffer_size;
	    }
	    s = p-fs->buffer;
	    memcpy(buffer,fs->buffer,s);
	    fs->foff += s;
	    memmove(fs->buffer,fs->buffer+s,fs->buffer_size-=s);
	    size -= s;
	    break;
	}else if (fs->buffer_size){
	    fillFo(fs,&fo);
	    s = fs->buffer_size;
	    if (s > size)
		s = size;
	    memcpy(buffer,fs->buffer,s);
	    buffer = (char*)buffer+s;
	    size -= s;
	    memmove(fs->buffer,fs->buffer+s,fs->buffer_size-=s);
	    fs->foff += s;
	    fs->line_number = 0;
	    break;
	}else{
	    s = gfile_read(fs->fd,buffer,size);
	    if (s < 0){
		fs->ferror = "cannot read file";
		break;
	    }
	    if (s == 0)
		nextFile(fs);
	    else{
		fillFo(fs,&fo);
		fs->foff += s;
		fs->line_number = 0;
		buffer = (char*)buffer+s;
		size -= s;
		break;
	    }
	}
    }
    if (fs->ferror)
	return -1;
    return osize-size;
}

int
fstream_eof(fstream_t*fs)
{
    return fs->fidx==fs->glob.gl_pathc;
}

int
fstream_rewind(fstream_t*fs)
{
    fs->fidx = 0;
    fs->foff = 0;
    fs->line_number = 1;
    fs->ferror = 0;
    fs->fskip = fs->options.header;
    fs->buffer_size = 0;
    if (fs->fd)
	gfile_close(fs->fd);
    if (0 == (fs->fd=gfile_open(fs->glob.gl_pathv[0]))){
	gfile_printf_then_putc_newline("fstream unable to open file %s",fs->glob.gl_pathv[0]);
	fs->ferror = "unable to open file";
	return -1;
    }	
    return 0;
}
