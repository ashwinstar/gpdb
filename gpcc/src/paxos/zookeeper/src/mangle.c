/* mangle.c */

#include "header.h"

#define PREFIX	"zoo/"

static void
format(char**s,int*len,const char*fmt,...)
{
    int i;
    va_list va;
    va_start(va,fmt);
    i = vsnprintf(*s,*len,fmt,va);
    if (i >= *len)
        *len = 0;
    else{
	*s += i;
	*len -= i;
    }
    va_end(va);
}

int
mangle_path(char*realpath,int max_realpath_len,const char*path,int isdir)
{
    *realpath = 0;
    format(&realpath,&max_realpath_len,"%s",PREFIX);
    for (;;){
        const char*a;
	while (*path == '/')
	    path++;
	if (!*path){
	    if (!isdir)
	        return 1;
	    break;
	}
	a = strchr(path,'/');
	if (isdir && !a)
	    a = path+strlen(path);
	if (a){
	    format(&realpath,&max_realpath_len,"d-");
	    while (path < a)
		format(&realpath,&max_realpath_len,"%c",*path++);
	    format(&realpath,&max_realpath_len,"/");
	}else{
	    format(&realpath,&max_realpath_len,"f-");
	    while (*path)
		format(&realpath,&max_realpath_len,"%c",*path++);
	    break;
	}
    }
    return !max_realpath_len;
}

int
unmangle_path(char*path)
{
    char*a=path;
    if (strncmp(a,PREFIX,strlen(PREFIX)))
	return 1;
    a += strlen(PREFIX);
    while (*a){
	if ((a[0]!='d' && a[0]!='f') || a[1]!='-')
	   return 1;
	a += 2;
	while (*a)
	    if ((*path++=*a++) == '/')
		break;
    }
    *path = 0;
    return 0;
}

char
mangle_setChar(char*path,char what)
{
    char*a=strrchr(path,'/');
    char old;
    if (!a || !a[1]){
        assert(0);
	return 0;
    }
    old = a[1];
    a[1] = what;
    return old;
}
