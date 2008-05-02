/* sign.c */

#ifdef sun
#include <md5.h>
#define MD5_Init	MD5Init
#define MD5_Update	MD5Update
#define MD5_Final	MD5Final
#else
#include <openssl/md5.h>
#endif

#include "chubby_private.h"

int
chubby_readPaxosKey(chubby_t*thiz)
{
    char p[PATH_MAX],*q;
    FILE*f;
    int i;
    long filesize;
    sprintf(p,"%s/.paxos_key",getenv("HOME"));
    f = fopen(p,"r");
    if (!f){
        perror(p);
	return 1;
    }
    fseek(f,0,2);
    filesize = ftell(f);
    rewind(f);
    q = thiz->malloc(filesize+1);
    if (!q)
        return 1;
    thiz->paxosKey = q;
    while ((i=getc(f))!=-1)
        if (i>' ' && i<='~' && q<thiz->paxosKey+filesize)
	    *q++ = i;
    *q = 0;
    return 0;
}

void
sockaddr_toString(char*a,const struct sockaddr*sa)
{
    assert(sa->sa_family==AF_INET);
    sprintf(a,"%u.%u.%u.%u:%u ",sa->sa_data[2]&255,sa->sa_data[3]&255,sa->sa_data[4]&255,sa->sa_data[5]&255,(sa->sa_data[0]&255)<<8|(sa->sa_data[1]&255));
}

void
chubby_sign(chubby_t*thiz,char*a,const char*b,int len,const struct sockaddr*f,const struct sockaddr*t)
{
    char s[23]; /* "xxx.xxx.xxx.xxx:xxxxx " */
    unsigned char md[MD5_DIGEST_LENGTH];
    int i;
    MD5_CTX m;
    MD5_Init(&m);
    MD5_Update(&m,thiz->paxosKey,strlen(thiz->paxosKey));
    sockaddr_toString(s,f);
    MD5_Update(&m,s,strlen(s));
    sockaddr_toString(s,t);
    MD5_Update(&m,s,strlen(s));
    MD5_Update(&m,b+SIGNSTRING_LENGTH,len-SIGNSTRING_LENGTH);
    MD5_Final(md,&m);
    for (i=0; i<16; i++)
        sprintf(a+i*2,"%02x",md[i]);
    a[32] = ' ';
}
