/*****************************************************************************
 *
 * This example source code introduces a c library buffered I/O interface to
 * URL reads it supports fopen(), fread(), fgets(), feof(), fclose(),
 * rewind(). Supported functions have identical prototypes to their normal c
 * lib namesakes and are preceaded by url_ .
 *
 * Using this code you can replace your program's fopen() with url_fopen()
 * and fread() with url_fread() and it become possible to read remote streams
 * instead of (only) local files. Local files (ie those that can be directly
 * fopened) will drop back to using the underlying clib implementations
 *
 * See the main() function at the bottom that shows an app that retrives from a
 * specified url using fgets() and fread() and saves as two output files.
 *
 * Coyright (c)2003 Simtec Electronics
 *
 * Re-implemented by Vincent Sanders <vince@kyllikki.org> with extensive
 * reference to original curl example code
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This example requires libcurl 7.9.7 or later.
 */
/*
 * Portions Copyright (c) 2007, greenplum inc
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>

#include <curl/curl.h>

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

enum fcurl_type_e { CFTYPE_NONE=0, CFTYPE_FILE=1, CFTYPE_CURL=2, CFTYPE_EXEC=3 };

typedef struct URL_FILE URL_FILE;
struct URL_FILE
{
    enum fcurl_type_e type;     /* type of handle */
	char* url;
    union {
		struct {
			FILE *fp;
		} file;
		struct {
			CURL *handle;
			char *curl_url;
			struct curl_slist *x_httpheader;
			struct {
				char byte[1024*16];
				int bot, top;
			} buffer;
			int still_running;          /* Is background url fetch still in progress */
		} curl;

		struct {
			FILE* fp;
			char *shexec;
		} exec;
	} u;
};


/* exported functions */
URL_FILE *url_fopen(char *url, const char *operation, extvar_t* ev);
int url_fclose(URL_FILE *file);
int url_feof(URL_FILE *file);
size_t url_fread(void *ptr, size_t size, size_t nmemb, URL_FILE *file);
void url_rewind(URL_FILE *file);

/* we use a global one for convenience */
CURLM *multi_handle = 0;

/* curl calls this routine to get more data */
static size_t
write_callback(char *buffer,
               size_t size,
               size_t nitems,
               void *userp)
{
    URL_FILE *url = (URL_FILE *)userp;
	const int bsz = sizeof(url->u.curl.buffer.byte);
	const int used = url->u.curl.buffer.top - url->u.curl.buffer.bot;
	int avail = bsz - url->u.curl.buffer.top;

	if (size != 1) 
	{
		elog(ERROR, "internal error: size param %d should be 1", (int)size);
		return -1;
	}
	size = nitems;

	/* if current buffer is less than 1 quarter full, compress it */
	if (avail < bsz / 4 && url->u.curl.buffer.bot > bsz / 4)
	{
		memmove(url->u.curl.buffer.byte, 
				url->u.curl.buffer.byte + url->u.curl.buffer.bot,
				used);
		url->u.curl.buffer.bot = 0;
		url->u.curl.buffer.top = used;
		avail = bsz - url->u.curl.buffer.top;
	}

    if (size > avail)
		size = avail;
	memcpy(url->u.curl.buffer.byte + url->u.curl.buffer.top, buffer, size);
    url->u.curl.buffer.top += size;
    return size;
}


static int
is_error(URL_FILE* file)
{
	long response_code;
	char* effective_url;
	int e;
	CURL* curl = file->u.curl.handle;

	if (0 != (e = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code))) 
		return -1;

	if (0 != (e = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url)))
		return -1;

	if (! (200 <= response_code && response_code < 300)) {
		if (response_code == 0) 
			elog(ERROR, "connection failed while %s", file->url);
		else
			elog(ERROR, "http response code %ld while %s", response_code, file->url);
		return -1;
	}

	return 0;
}


/* use to attempt to fill the read buffer up to requested number of bytes */
/* return 0 if successful, -1 otherwise */
static int
fill_buffer(URL_FILE *file,int want)
{
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    int maxfd;
    struct timeval timeout;
    int nfds, e;

    /* only attempt to fill buffer if transactions still running and buffer
     * doesnt exceed required size already
     */
	if (! file->u.curl.still_running)
		return 0;
	if (file->u.curl.buffer.top - file->u.curl.buffer.bot >= want)
		return 0;

	/* prime it */
	if (file->u.curl.still_running 
		&& file->u.curl.buffer.top - file->u.curl.buffer.bot < want)
	{
		while (CURLM_CALL_MULTI_PERFORM == 
				   (e = curl_multi_perform(multi_handle, &file->u.curl.still_running)));
	}

    /* attempt to fill buffer */
	while (file->u.curl.still_running 
		   && file->u.curl.buffer.top - file->u.curl.buffer.bot < want)
    {
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* set a suitable timeout to fail on */
        timeout.tv_sec = 60; /* 1 minute */
        timeout.tv_usec = 0;

        /* get file descriptors from the transfers */
        if (0 != (e = curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd)))
		{
			elog(ERROR, "internal error: curl_multi_fdset failed (%d - %s)", 
				 e, curl_easy_strerror(e));
			return -1;
		}

		if (maxfd <= 0)
		{
			elog(ERROR, "internal error: curl_multi_fdset reports no more fd");
			return -1;
		}

        if (-1 == (nfds = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout))) 
		{
			if (errno == EINTR) 
				continue;
			elog(ERROR, "internal error: select failed on curl_multi_fdset (%d - %s)",
				 errno, strerror(errno));
			return -1;
		}

		if (nfds > 0) 
		{
            /* timeout or readable/writable sockets */
            /* note we *could* be more efficient and not wait for
             * CURLM_CALL_MULTI_PERFORM to clear here and check it on re-entry
             * but that gets messy */
			while (CURLM_CALL_MULTI_PERFORM == 
				   (e = curl_multi_perform(multi_handle, &file->u.curl.still_running)));
			if (e != 0) {
				elog(ERROR, "internal error: curl_multi_perform failed (%d - %s)", 
					 e, curl_easy_strerror(e));
				return 1;
			}
        }
    } 
    return 1;
}



static int set_httpheader(URL_FILE* fcurl, const char* name, const char* value)
{
	char tmp[1024];
	if (strlen(name) + strlen(value) + 5 > sizeof(tmp)) {
	   elog(ERROR, "set_httpheader name/value is too long. name = %s, value=%s", name, value);
	   return -1;
	}
	sprintf(tmp, "%s: %s", name, value);
	fcurl->u.curl.x_httpheader = curl_slist_append(fcurl->u.curl.x_httpheader, tmp);
	return 0;
}


static int make_export(char* name, const char* value, char* buf)
{
	char* p = buf;
	int ch;

	if (buf)
		strcpy(p, name);
	p += strlen(name);

	if (buf)
		strcpy(p, "='");
	p += 2;

	for ( ; 0 != (ch = *value); value++) 
	{
		if (ch == '\'' || ch == '\\')
		{
			if (buf)
				*p = '\\';
			p++;
		}
		if (buf)
			*p = ch;
		p++;
	}

	if (buf)
		strcpy(p, "' && export ");
	p += strlen("' && export ");
	
	if (buf)
		strcpy(p, name);
	p += strlen(name);

	if (buf)
		strcpy(p, " && ");
	p += 4;
	
	return p - buf;
}


static int make_command(const char* cmd, extvar_t* ev, char* buf)
{
	int sz = 0;
	sz += make_export("GP_MASTER_HOST", ev->GP_MASTER_HOST, buf ? buf + sz : 0);
	sz += make_export("GP_MASTER_PORT", ev->GP_MASTER_PORT, buf ? buf + sz : 0);
	sz += make_export("GP_SEG_PG_CONF", ev->GP_SEG_PG_CONF, buf ? buf + sz : 0);
	sz += make_export("GP_SEG_DATADIR", ev->GP_SEG_DATADIR, buf ? buf + sz : 0);
	sz += make_export("GP_DATABASE", ev->GP_DATABASE, buf ? buf + sz : 0);
	sz += make_export("GP_DATE", ev->GP_DATE, buf ? buf + sz : 0);
	sz += make_export("GP_TIME", ev->GP_TIME, buf ? buf + sz : 0);
	sz += make_export("GP_XID", ev->GP_XID, buf ? buf + sz : 0);
	sz += make_export("GP_CID", ev->GP_CID, buf ? buf + sz : 0);
	sz += make_export("GP_SN", ev->GP_SN, buf ? buf + sz : 0);

	if (buf)
		strcpy(buf + sz, cmd);
	sz += strlen(cmd);

	return sz + 1;				/* add NUL terminator */
}


static int encode_url(const char* s, char* buf)
{
	char* p = buf;
	int ch;
	for ( ; 0 != (ch = * (unsigned char*) s); s++) 
	{
		if (('a' <= ch && ch <= 'z')
			|| ('A' <= ch && ch <= 'Z')
			|| ('0' <= ch && ch <= '9')
			|| ('-' == ch)
			|| ('.' == ch)
			|| ('_' == ch)
			|| ('~' == ch))
		{
			if (buf)
				*p = (char) ch;
			p++;
		}
		else if (' ' == ch)
		{
			if (buf)
				*p = '+';
			p++;
		}
		else 
		{
			if (buf)
			{
				int x = ch >> 4;
				int y = ch & 0xf;
				x = (x < 10) ? x + '0' : x - 10 + 'A';
				y = (y < 10) ? y + '0' : y - 10 + 'A';
				p[0] = '%';
				p[1] = x;
				p[2] = y;
			}
			p += 3;
		}
	}
	if (buf)
		*p = 0;
	p++;

	return p - buf;
}

static int make_url(const char* url, extvar_t* ev, char* buf)
{
	char* p = buf;
	for ( ; *url; url++)
	{
		if (strncmp(url, "GP_", 3) == 0)
		{
			if (0 == strcmp(url, "GP_MASTER_HOST"))
				p += encode_url(ev->GP_MASTER_HOST, p);
			else if (0 == strcmp(url, "GP_MASTER_PORT"))
				p += encode_url(ev->GP_MASTER_PORT, p);
			else if (0 == strcmp(url, "GP_SEG_PG_CONF"))
				p += encode_url(ev->GP_SEG_PG_CONF, p);
			else if (0 == strcmp(url, "GP_SEG_DATADIR"))
				p += encode_url(ev->GP_SEG_DATADIR, p);
			else if (0 == strcmp(url, "GP_DATABASE"))
				p += encode_url(ev->GP_DATABASE, p);
			else if (0 == strcmp(url, "GP_DATE"))
				p += encode_url(ev->GP_DATE, p);
			else if (0 == strcmp(url, "GP_TIME"))
				p += encode_url(ev->GP_TIME, p);
			else if (0 == strcmp(url, "GP_XID"))
				p += encode_url(ev->GP_XID, p);
			else if (0 == strcmp(url, "GP_CID"))
				p += encode_url(ev->GP_CID, p);
			else if (0 == strcmp(url, "GP_SN"))
				p += encode_url(ev->GP_SN, p);
			else
				return -1;
		}
		else 
		{
			if (buf)
				*p = *url;
			p++;
		}
	}

	if (buf)
		*p = 0;					/* NUL terminator */
	p++;

	return p - buf;
}



URL_FILE *
url_fopen(char *url, const char *operation, extvar_t* ev)
{
    /* this code checks for URLs or types in the 'url' and
     * basicly use the real fopen() for standard files, or
	 * if the url happens to be a command to execute it uses
	 * popen to execute it.
	 */
	
	/* an EXECUTE string will always be prefixed like this */
	const char *exec_prefix = "execute:";
	
	/*char  sessionIDBuf[80];*/
	URL_FILE *file;
	int e;
    (void)operation;
	

    if (! (file = (URL_FILE *)malloc(sizeof(URL_FILE) + strlen(url) + 1)))
	{
		elog(ERROR, "out of memory");
        return NULL;
	}
    memset(file, 0, sizeof(URL_FILE));

	file->url = ((char*) file) + sizeof(URL_FILE);
	strcpy(file->url, url);

	/* 
	 * if 'url' starts with "execute:" then it's a command to execute and 
	 * not a url (the command specified in CREATE EXTERNAL TABLE .. EXECUTE) 
	 */
	if (pg_strncasecmp(url, exec_prefix, strlen(exec_prefix)) == 0)
	{
		char* cmd = url + strlen(exec_prefix);
		int sz = make_command(cmd, ev, 0);
		char *shexec = palloc(sz);
		make_command(cmd, ev, shexec);
 		
		file->type = CFTYPE_EXEC; /* marked as a EXEC */	
		file->u.exec.fp = popen(shexec, "r");
		file->u.exec.shexec = shexec;
		if (file->u.exec.fp == NULL)
		{
			elog( ERROR, "External Table error executing cmd: '%s'", cmd );
			return NULL;
		}
	}
    else if (IS_FILE_URI(url))
    {
		char* path = strchr(url + strlen(PROTOCOL_FILE), '/');
		if (!path) 
		{
			elog(ERROR, "External Table error opening file: '%s'", url);
			return NULL;
		}

		file->type = CFTYPE_FILE; /* marked as local FILE */
		file->u.file.fp = fopen(path, operation);
		
		/* couldn't open local file. return NULL to display proper error */
		if (!file->u.file.fp)
		{
			free(file);
			return NULL;
		}
    }
    else if (IS_HTTP_URI(url) || IS_GPFDIST_URI(url))
    {
		int sz = make_url(file->url, ev, 0);

        file->type = CFTYPE_CURL; /* marked as URL */
		if (sz < 0) 
		{
			elog(ERROR, "illegal URL: %s", file->url);
			url_fclose(file);
			return NULL;
		}

		file->u.curl.curl_url = palloc(sz);
		make_url(file->url, ev, file->u.curl.curl_url);
		if (IS_GPFDIST_URI(file->u.curl.curl_url))
		{
			/* replace gpfdist:// with http:// */
			file->u.curl.curl_url += 3;
			memcpy(file->u.curl.curl_url, "http", 4);
		}

        if (! (file->u.curl.handle = curl_easy_init()))
		{
			elog(ERROR, "internal error: curl_easy_init failed");
			url_fclose(file);
			return NULL;
		}

        if (0 != (e = curl_easy_setopt(file->u.curl.handle, CURLOPT_URL, file->u.curl.curl_url))) 
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_URL %s (%d - %s)", 
				 file->u.curl.curl_url, e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->u.curl.handle, CURLOPT_VERBOSE, FALSE))) 
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_VERBOSE error (%d - %s)", 
				 e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->u.curl.handle, CURLOPT_WRITEFUNCTION, write_callback)))
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEFUNCTION error (%d - %s)", 
				 e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->u.curl.handle, CURLOPT_WRITEDATA, file)))
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEDATA error (%d - %s)", 
				 e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
		}
	
		file->u.curl.x_httpheader = 0;
		if (set_httpheader(file, "X-GP-MASTER_HOST", ev->GP_MASTER_HOST)
			|| set_httpheader(file, "X-GP-MASTER_PORT", ev->GP_MASTER_PORT)
			|| set_httpheader(file, "X-GP_SEG_PG_CONF", ev->GP_SEG_PG_CONF)
			|| set_httpheader(file, "X-GP_SEG_DATADIR", ev->GP_SEG_DATADIR)
			|| set_httpheader(file, "X-GP-DATABASE", ev->GP_DATABASE)
			|| set_httpheader(file, "X-GP-XID", ev->GP_XID)
			|| set_httpheader(file, "X-GP-CID", ev->GP_CID)
			|| set_httpheader(file, "X-GP-SN", ev->GP_SN))  
		{
			elog(ERROR, "internal error: some variable value is too long");
			url_fclose(file);
			return NULL;
		}

		if (0 != (e = curl_easy_setopt(file->u.curl.handle, CURLOPT_HTTPHEADER, file->u.curl.x_httpheader)))
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEDATA error (%d - %s)", 
				 e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
        }

        if (!multi_handle) 
		{
            if (! (multi_handle = curl_multi_init()))
			{
				elog(ERROR, "internal error: curl_multi_init failed");
				url_fclose(file);
				return NULL;
			}
		}

        if (0 != (e = curl_multi_add_handle(multi_handle, file->u.curl.handle)))
		{
			if (CURLM_CALL_MULTI_PERFORM != e)
			{
				elog(ERROR, "internal error: curl_multi_add_handle failed (%d - %s)", 
					 e, curl_easy_strerror(e));
				url_fclose(file);
				return NULL;
			}
		}

        /* lets start the fetch */
		while (CURLM_CALL_MULTI_PERFORM == 
			   (e = curl_multi_perform(multi_handle, &file->u.curl.still_running)));

		if (e != 0) 
		{
			elog(ERROR, "internal error: curl_multi_perform failed (%d - %s)",
				 e, curl_easy_strerror(e));
			url_fclose(file);
			return NULL;
		}

		fill_buffer(file, 1);
		if (is_error(file))
		{
			url_fclose(file);
			return NULL;
		}
    }
    return file;
}

int
url_fclose(URL_FILE *file)
{
    int ret=0;/* default is good return */

    switch (file->type)
    {
    case CFTYPE_FILE:
        ret=fclose(file->u.file.fp); /* passthrough */
        break;

    case CFTYPE_EXEC:
    	pclose(file->u.exec.fp);
    	break;

    case CFTYPE_CURL:
		if (file->u.curl.x_httpheader)
			curl_slist_free_all(file->u.curl.x_httpheader);

        /* make sure the easy handle is not in the multi handle anymore */
		if (file->u.curl.handle)
		{
			curl_multi_remove_handle(multi_handle, file->u.curl.handle);
			/* cleanup */
			curl_easy_cleanup(file->u.curl.handle);
		}

		file->u.curl.buffer.bot = file->u.curl.buffer.top = 0;
        break;

    default: /* unknown or supported type - oh dear */
        ret=EOF;
        errno=EBADF;
        break;

    }

    free(file);

    return 0;
}

int
url_feof(URL_FILE *file)
{
    int ret = 0;

    switch (file->type)
    {
    case CFTYPE_FILE:
		ret = feof(file->u.file.fp);
		break;

    case CFTYPE_EXEC:
        ret = feof(file->u.exec.fp);
        break;    	

    case CFTYPE_CURL:
		if (file->u.curl.buffer.bot == file->u.curl.buffer.top
			&& ! file->u.curl.still_running)
			ret = 1;
        break;

    default: /* unknown or supported type - oh dear */
        ret = -1;
        errno = EBADF;
        break;
    }
    return ret;
}

size_t
url_fread(void *ptr, size_t size, size_t nmemb, URL_FILE *file)
{
    size_t want;
	int cnt;

    switch (file->type)
    {
    case CFTYPE_FILE:
		want = fread(ptr, size, nmemb, file->u.file.fp);
		break;

    case CFTYPE_EXEC:
		want = fread(ptr, size, nmemb, file->u.exec.fp);
        break;
    	
    case CFTYPE_CURL:
        want = nmemb * size;

		/* fill the buffer */
        fill_buffer(file,want);

		/* if no data, check error */
		cnt = file->u.curl.buffer.top - file->u.curl.buffer.bot;
		if (cnt == 0)
            return is_error(file);

        /* ensure only available data is considered */
		if (cnt < want)
			want = cnt / size * size;

        /* xfer data to caller */
        memcpy(ptr, file->u.curl.buffer.byte + file->u.curl.buffer.bot, want);
		file->u.curl.buffer.bot += want;

        want = want / size;     /* number of items - nb correct op - checked
                                 * with glibc code*/
        break;

    default: /* unknown or supported type - oh dear */
        want = 0;
        errno = EBADF;
        break;
    }

    return want;
}

void
url_rewind(URL_FILE *file)
{
    switch(file->type)
    {
    case CFTYPE_FILE:
        rewind(file->u.file.fp);
        break;
       
    case CFTYPE_EXEC:
    	/* we'll need to execute the command again */
		if (file->u.exec.fp)
			pclose(file->u.exec.fp);
		file->u.exec.fp = popen(file->u.exec.shexec, "r");
		if (file->u.exec.fp == NULL)
		{
			elog(ERROR, "External Table error executing cmd: '%s'", file->u.exec.shexec);
			return;
		}
    	break; 

    case CFTYPE_CURL:
        /* halt transaction */
        curl_multi_remove_handle(multi_handle, file->u.curl.handle);

        /* restart */
        curl_multi_add_handle(multi_handle, file->u.curl.handle);

        /* reset buffer */
		file->u.curl.buffer.bot = file->u.curl.buffer.top = 0;
        break;

    default: /* unknown or supported type - oh dear */
        break;

    }

}
