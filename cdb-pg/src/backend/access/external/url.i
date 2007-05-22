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

struct fcurl_data
{
    enum fcurl_type_e type;     /* type of handle */
    union {
        CURL *curl;
        FILE *file;
    } handle;                   /* handle */

	char* curl_url;
    struct curl_slist *x_httpheader;

    char *buffer;               /* buffer to store cached data*/
    int buffer_len;             /* currently allocated buffers length */
    int buffer_pos;             /* end of data in buffer*/
    int still_running;          /* Is background url fetch still in progress */
};

typedef struct fcurl_data URL_FILE;

/* exported functions */
URL_FILE *url_fopen(char *url, char *filename, const char *operation, 
					const char* XID, 
					const char* CID,
					const char* SN);
int url_fclose(URL_FILE *file);
int url_feof(URL_FILE *file);
size_t url_fread(void *ptr, size_t size, size_t nmemb, URL_FILE *file);
char * url_fgets(char *ptr, int size, URL_FILE *file);
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
    char *newbuff;
    int rembuff;

    URL_FILE *url = (URL_FILE *)userp;
    size *= nitems;

    rembuff=url->buffer_len - url->buffer_pos;//remaining space in buffer

    if(size > rembuff)
    {
        //not enuf space in buffer
        newbuff=realloc(url->buffer,url->buffer_len + (size - rembuff));
        if(newbuff==NULL)
        {
            fprintf(stderr,"callback buffer grow failed\n");
            size=rembuff;
        }
        else
        {
            /* realloc suceeded increase buffer size*/
            url->buffer_len+=size - rembuff;
            url->buffer=newbuff;

            /*printf("Callback buffer grown to %d bytes\n",url->buffer_len);*/
        }
    }

    memcpy(&url->buffer[url->buffer_pos], buffer, size);
    url->buffer_pos += size;

    /*fprintf(stderr, "callback %d size bytes\n", size);*/

    return size;
}


static int
is_error(CURL* curl)
{
	long response_code;
	char* effective_url;
	int e;

	if (0 != (e = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code))) 
		return -1;

	if (0 != (e = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url)))
		return -1;

	if (! (200 <= response_code && response_code < 300)) {
		if (response_code == 0) 
			elog(ERROR, "connection failed while %s", effective_url);
		else
			elog(ERROR, "http response code: %ld", response_code);
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
	if (! file->still_running)
		return 0;
	if (file->buffer_pos >= want)
		return 0;

    /* attempt to fill buffer */
	while (file->still_running && file->buffer_pos < want)
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
			elog(ERROR, "internal error: curl_multi_fdset failed (%d)", e);
			return -1;
		}

		if (maxfd <= 0)
		{
			elog(ERROR, "internal error: curl_multi_fdset reports no more fd");
			return -1;
		}

        if (-1 == (nfds = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout))) 
		{
			elog(ERROR, "internal error: select failed on curl_multi_fdset");
			return -1;
		}

		if (nfds > 0) 
		{
            /* timeout or readable/writable sockets */
            /* note we *could* be more efficient and not wait for
             * CURLM_CALL_MULTI_PERFORM to clear here and check it on re-entry
             * but that gets messy */
			while (CURLM_CALL_MULTI_PERFORM == 
				   (e = curl_multi_perform(multi_handle, &file->still_running)));
			if (e != 0) {
				elog(ERROR, "internal error: curl_multi_perform failed (%d)", e);
				return 1;
			}
        }
    } 
    return 1;
}

/* use to remove want bytes from the front of a files buffer */
static int
use_buffer(URL_FILE *file,int want)
{
    /* sort out buffer */
    if((file->buffer_pos - want) <=0)
    {
        /* ditch buffer - write will recreate */
        if(file->buffer)
            free(file->buffer);

        file->buffer=NULL;
        file->buffer_pos=0;
        file->buffer_len=0;
    }
    else
    {
        /* move rest down make it available for later */
        memmove(file->buffer,
                &file->buffer[want],
                (file->buffer_pos - want));

        file->buffer_pos -= want;
    }
    return 0;
}



URL_FILE *
url_fopen(char *url, char *localfilename, const char *operation, 
		  const char* XID, const char* CID, const char* SN)
{
    /* this code checks for URLs or types in the 'url' and
       basicly use the real fopen() for standard files */
    bool  isGpfdist = false;
	/*char  sessionIDBuf[80];*/
    URL_FILE *file;
	int e;
    (void)operation;

    file = (URL_FILE *)malloc(sizeof(URL_FILE) + strlen(url) + 1);
    if(!file)
        return NULL;
    memset(file, 0, sizeof(URL_FILE));

	file->curl_url = ((char*) file) + sizeof(URL_FILE);
	strcpy(file->curl_url, url);


	if (strncmp(url, PROTOCOL_EXEC, strlen(PROTOCOL_EXEC)) == 0)
	{
		int len = strlen(url) - strlen(PROTOCOL_EXEC);
 		char *cmd = (char *) palloc(len + 1);
 		strncpy(cmd, url+strlen(PROTOCOL_EXEC), len); 		
 		cmd[len] = '\0';
 		
		file->handle.file= popen(cmd,"r");
		file->buffer = cmd;
		if( file->handle.file == NULL)
			elog( ERROR, "External Table error executing cmd: '%s'", cmd );
		
		file->type = CFTYPE_EXEC; /* marked as a EXEC */
	}
    else if(localfilename)
    {
		file->type = CFTYPE_FILE; /* marked as local FILE */
		file->handle.file=fopen(localfilename,operation);
		
		/* couldn't open local file. return NULL to display proper error */
		if(!file->handle.file)
			return NULL;
    }
    else
    {
        file->type = CFTYPE_CURL; /* marked as URL */
		
        if(strncmp(file->curl_url, "gpfdist", 7) == 0)
        {
			isGpfdist = true;
			file->curl_url += 3;
			memcpy(file->curl_url, "http", 4);
        }

        if (! (file->handle.curl = curl_easy_init()))
		{
			elog(ERROR, "internal error: curl_easy_init failed");
			return NULL;
		}

        if (0 != (e = curl_easy_setopt(file->handle.curl, CURLOPT_URL, file->curl_url))) 
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_URL %s error %d", file->curl_url, e);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->handle.curl, CURLOPT_VERBOSE, FALSE))) 
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_VERBOSE error %d", e);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->handle.curl, CURLOPT_WRITEFUNCTION, write_callback)))
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEFUNCTION error %d", e);
			return NULL;
		}
        if (0 != (e = curl_easy_setopt(file->handle.curl, CURLOPT_WRITEDATA, file)))
		{
			elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEDATA error %d", e);
			return NULL;
		}

		if (isGpfdist) 
		{
			char tmp[100];
            file->x_httpheader = NULL;
			sprintf(tmp, "X-GP-XID: %s", XID);
    		file->x_httpheader = curl_slist_append(file->x_httpheader, tmp);
			sprintf(tmp, "X-GP-CID: %s", CID);
    		file->x_httpheader = curl_slist_append(file->x_httpheader, tmp);
			sprintf(tmp, "X-GP-SN: %s", SN);
    		file->x_httpheader = curl_slist_append(file->x_httpheader, tmp);
            if (0 != (e = curl_easy_setopt(file->handle.curl, CURLOPT_HTTPHEADER, file->x_httpheader)))
			{
				elog(ERROR, "internal error: curl_easy_setopt CURLOPT_WRITEDATA error %d", e);
				return NULL;
			}
        }

        if (!multi_handle) 
		{
            if (! (multi_handle = curl_multi_init()))
			{
				elog(ERROR, "internal error: curl_multi_init failed");
				return NULL;
			}
		}

        if (0 != (e = curl_multi_add_handle(multi_handle, file->handle.curl)))
		{
			if (CURLM_CALL_MULTI_PERFORM != e)
			{
				elog(ERROR, "internal error: curl_multi_add_handle failed (%d)", e);
				return NULL;
			}
		}

        /* lets start the fetch */
		while (CURLM_CALL_MULTI_PERFORM == 
			   (e = curl_multi_perform(multi_handle, &file->still_running)));

		if (e != 0) 
		{
			elog(ERROR, "internal error: curl_multi_perform failed (%d)", e);
			return NULL;
		}

		fill_buffer(file, 1);
		if (!file->buffer_pos && is_error(file->handle.curl)) 
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

    switch(file->type)
    {
    case CFTYPE_FILE:
        ret=fclose(file->handle.file); /* passthrough */
        break;
    case CFTYPE_EXEC:
    	pclose(file->handle.file);
    	break;

    case CFTYPE_CURL:
		curl_slist_free_all(file->x_httpheader);

        /* make sure the easy handle is not in the multi handle anymore */
        curl_multi_remove_handle(multi_handle, file->handle.curl);

        /* cleanup */
        curl_easy_cleanup(file->handle.curl);
        break;

    default: /* unknown or supported type - oh dear */
        ret=EOF;
        errno=EBADF;
        break;

    }

    if(file->buffer)
        free(file->buffer);/* free any allocated buffer space */

    free(file);

    return ret;
}

int
url_feof(URL_FILE *file)
{
    int ret=0;

    switch(file->type)
    {
    case CFTYPE_FILE:
    case CFTYPE_EXEC:
        ret=feof(file->handle.file);
        break;    	
    case CFTYPE_CURL:
        if((file->buffer_pos == 0) && (!file->still_running))
            ret = 1;
        break;
    default: /* unknown or supported type - oh dear */
        ret=-1;
        errno=EBADF;
        break;
    }
    return ret;
}

size_t
url_fread(void *ptr, size_t size, size_t nmemb, URL_FILE *file)
{
    size_t want;

    switch(file->type)
    {
    case CFTYPE_FILE:
    case CFTYPE_EXEC:
        want=fread(ptr,size,nmemb,file->handle.file);
        break;
    	
    case CFTYPE_CURL:
        want = nmemb * size;

        fill_buffer(file,want);

        /* check if theres data in the buffer - if not fill_buffer()
         * either errored or EOF */
        if (!file->buffer_pos)
            return is_error(file->handle.curl);

        /* ensure only available data is considered */
        if(file->buffer_pos < want)
            want = file->buffer_pos;

        /* xfer data to caller */
        memcpy(ptr, file->buffer, want);

        use_buffer(file,want);

        want = want / size;     /* number of items - nb correct op - checked
                                 * with glibc code*/

        /*printf("(fread) return %d bytes %d left\n", want,file->buffer_pos);*/
        break;

    default: /* unknown or supported type - oh dear */
        want=0;
        errno=EBADF;
        break;

    }
    return want;
}

char *
url_fgets(char *ptr, int size, URL_FILE *file)
{
    int want = size - 1;/* always need to leave room for zero termination */
    int loop;

    switch(file->type)
    {
    case CFTYPE_FILE:
    case CFTYPE_EXEC:
        ptr = fgets(ptr,size,file->handle.file);
        break;
    	
    case CFTYPE_CURL:
        fill_buffer(file,want);

        /* check if theres data in the buffer - if not fill either errored or
         * EOF */
        if(!file->buffer_pos)
            return NULL;

        /* ensure only available data is considered */
        if(file->buffer_pos < want)
            want = file->buffer_pos;

        /*buffer contains data */
        /* look for newline or eof */
        for(loop=0;loop < want;loop++)
        {
            if(file->buffer[loop] == '\n')
            {
                want=loop+1;/* include newline */
                break;
            }
        }

        /* xfer data to caller */
        memcpy(ptr, file->buffer, want);
        ptr[want]=0;/* allways null terminate */

        use_buffer(file,want);

        /*printf("(fgets) return %d bytes %d left\n", want,file->buffer_pos);*/
        break;

    default: /* unknown or supported type - oh dear */
        ptr=NULL;
        errno=EBADF;
        break;
    }

    return ptr;/*success */
}

void
url_rewind(URL_FILE *file)
{
    switch(file->type)
    {
    case CFTYPE_FILE:
        rewind(file->handle.file); /* passthrough */
        break;
       
    case CFTYPE_EXEC:
    	/* we'll need to execute the command again */
    	url_fclose(file);
    	/* most of these are null fo us. */
    	url_fopen( file->buffer, NULL, NULL, NULL, NULL, NULL ); 
    	break;   

    case CFTYPE_CURL:
        /* halt transaction */
        curl_multi_remove_handle(multi_handle, file->handle.curl);

        /* restart */
        curl_multi_add_handle(multi_handle, file->handle.curl);

        /* ditch buffer - write will recreate - resets stream pos*/
        if(file->buffer)
            free(file->buffer);

        file->buffer=NULL;
        file->buffer_pos=0;
        file->buffer_len=0;

        break;

    default: /* unknown or supported type - oh dear */
        break;

    }

}


/* Small main program to retrive from a url using fgets and fread saving the
 * output to two test files (note the fgets method will corrupt binary files if
 * they contain 0 chars */
#ifdef UNDEF
int
main(int argc, char *argv[])
{
    URL_FILE *handle;
    FILE *outf;

    int nread;
    char buffer[256];
    char *url;

    if(argc < 2)
    {
        url="http://192.168.7.3/testfile";/* default to testurl */
    }
    else
    {
        url=argv[1];/* use passed url */
    }

    /* copy from url line by line with fgets */
    outf=fopen("fgets.test","w+");
    if(!outf)
    {
        perror("couldnt open fgets output file\n");
        return 1;
    }

    handle = url_fopen(url, "r");
    if(!handle)
    {
        printf("couldn't url_fopen()\n");
        fclose(outf);
        return 2;
    }

    while(!url_feof(handle))
    {
        url_fgets(buffer,sizeof(buffer),handle);
        fwrite(buffer,1,strlen(buffer),outf);
    }

    url_fclose(handle);

    fclose(outf);


    /* Copy from url with fread */
    outf=fopen("fread.test","w+");
    if(!outf)
    {
        perror("couldnt open fread output file\n");
        return 1;
    }

    handle = url_fopen("testfile", "r");
    if(!handle) {
        printf("couldn't url_fopen()\n");
        fclose(outf);
        return 2;
    }

    do {
        nread = url_fread(buffer, 1,sizeof(buffer), handle);
        fwrite(buffer,1,nread,outf);
    } while(nread);

    url_fclose(handle);

    fclose(outf);


    /* Test rewind */
    outf=fopen("rewind.test","w+");
    if(!outf)
    {
        perror("couldnt open fread output file\n");
        return 1;
    }

    handle = url_fopen("testfile", "r");
    if(!handle) {
        printf("couldn't url_fopen()\n");
        fclose(outf);
        return 2;
    }

        nread = url_fread(buffer, 1,sizeof(buffer), handle);
        fwrite(buffer,1,nread,outf);
        url_rewind(handle);

        buffer[0]='\n';
        fwrite(buffer,1,1,outf);

        nread = url_fread(buffer, 1,sizeof(buffer), handle);
        fwrite(buffer,1,nread,outf);


    url_fclose(handle);

    fclose(outf);


    return 0;/* all done */
}
#endif /*UNDEF */
