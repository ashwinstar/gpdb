/* chubby.c */

#include "chubby_private.h"

#define TIMEOUT_USEC		1000000

static void
mangle(chubby_udp_buffer_t*b,chubby_string_t*s)
{
    int i;
    for (i=0; i<s->length; i++){
	char c=s->a[i];
	if (c==' ' || c=='!'){
	    if (b->length < sizeof b->a)
		b->a[b->length++] = '!';
	    c++;
	}
	if (b->length < sizeof b->a)
	    b->a[b->length++] = c;
    }
}

static int
unmangle(char*s,int size)
{
    int i,j=0;
    for (i=0; i<size; i++){
	char c=s[i];
	if (c=='!' && i+1<size)
	    c = s[++i]-1;
	s[j++] = c;
    }
    return j;
}

static void
chubby_delete(struct chubby_counted*_thiz)
{
    chubby_t*thiz=(chubby_t*)_thiz;
    if (thiz->addresses)
	thiz->free(thiz->addresses);
    thiz->free(thiz);
}

static int
chubby_init(chubby_t*thiz,const char*addresses)
{
    thiz->addresses = thiz->malloc(strlen(addresses)+1);
    if (!thiz->addresses)
	return 1;
    strcpy(thiz->addresses,addresses);
    if (gethostname(thiz->id,_POSIX_HOST_NAME_MAX))
	return 1;
    sprintf(thiz->id+strlen(thiz->id),"-%d-%d-%p",(int)getpid(),(int)time(NULL),thiz);
    return 0;
}

chubby_t*
chubby_new(const char*addresses,void*(*malloc)(size_t),void(*free)(void*))
{
    chubby_t*thiz=malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_delete;
	thiz->malloc = malloc;
	thiz->free = free;
	if (chubby_init(thiz,addresses)){
	    chubby_counted_close(thiz);
	    return NULL;
	}
    }
    return thiz;
}

chubby_connection_t*
chubby_makeConnection(chubby_t*thiz)
{
    return chubby_connection_new(thiz);
}

void
chubby_close(chubby_t*thiz)
{
    chubby_counted_close(thiz);
}

void
chubby_getNewId(chubby_t*thiz,char*a)
{
    sprintf(a,"%s-%"APR_INT64_T_FMT,thiz->id,++thiz->count);
}

static int
getSocketAddressByName(struct sockaddr_in*sa,char*a)
{
    char*b=strchr(a,':');
    struct hostent*h;
    memset(sa,0,sizeof*sa);
    sa->sin_family = AF_INET;
    if (!b)
	return 1;
    *b++ = 0;
    sa->sin_port = htons(atoi(b));
    h = gethostbyname(a);
    if (!h)
	return 1;
    if (h->h_length!=sizeof(struct in_addr) || !h->h_addr)
	return 1;
    sa->sin_addr = *(struct in_addr*)h->h_addr;
    return 0;
}

static int
chubby_sendGetLeader(chubby_t*thiz,int fd)
{
    char a[_POSIX_HOST_NAME_MAX+NUMBER_STRING_LENGTH+1],*s;
    struct sockaddr_in sa;
    for (s=thiz->addresses; *s; s++){
	char*t=s;
	s = strchr(t,',');
	if (!s)
	    s = t+strlen(t);
	memcpy(a,t,s-t);
	a[s-t] = 0;
	if (getSocketAddressByName(&sa,a))
	    return CHUBBY_E_IO;
	sendto(fd,"GetLeader",9,0,(struct sockaddr*) &sa,sizeof sa);
    }
    return 0;
}

static int
chubby_propose1(chubby_t*thiz,int fd,chubby_string_t**answer,chubby_udp_buffer_t*what)
{
    struct timeval timeout;
    int e;
    if (chubby_udp_buffer_isOverflowed(what))
	return CHUBBY_E_INVAL;
    timeout.tv_sec = TIMEOUT_USEC/1000000;
    timeout.tv_usec = TIMEOUT_USEC%1000000;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    for (;;){
	chubby_udp_buffer_t b;
	int i,j;
	char*command=NULL;
	int resultSize=0;
	char*result=NULL;
	int decreeSize=0;
	char*decree=NULL;
	if (!thiz->leaderValid){
	    e = chubby_sendGetLeader(thiz,fd);
	    if (e)
		return e;
	}else
	    sendto(fd,what->a,what->length,0,
		   (struct sockaddr*) &thiz->leader,sizeof thiz->leader);
	b.length = recvfrom(fd,b.a,65535,0,NULL,NULL);
	if (b.length < 0){
	    thiz->leaderValid = 0;
	    continue;
	}
	for (i=0; i<b.length; i++){
	    for (j=i; i<b.length&&b.a[i]!=' '; i++);
	    if (command == NULL){
		command = b.a+j;
		b.a[i] = 0;
	    }else if (!memcmp(b.a+j,"result=",7)){
		result = b.a+j+7;
		resultSize = unmangle(result,i-j-7);
		result[resultSize] = 0;
	    }else if (!memcmp(b.a+j,"decree=",7)){
		decree = b.a+j;
		decreeSize = i-j;
		decree[decreeSize] = 0;
	    }
	}
	if (!strcmp(command,"Redirect"))
	    thiz->leaderValid = !getSocketAddressByName(&thiz->leader,result);
	else if (!strcmp(command,"Result")){
	    if (what->length==decreeSize+8 && !memcmp(what->a+8,decree,decreeSize)){
		if (!memcmp(result,"error ",6))
		    return atoi(result+6);
		if (answer){
		    *answer = thiz->malloc(sizeof**answer+resultSize);
		    if (!*answer)
			return CHUBBY_E_OOM;
		    (*answer)->length = resultSize;
		    memcpy((*answer)->a,result,resultSize);
		    (*answer)->a[resultSize] = 0;
		}
		return 0;
	    }
	}else
	    assert(0);
    }
}

int
chubby_propose(chubby_t*thiz,chubby_string_t**answer,chubby_string_t*what)
{
    chubby_udp_buffer_t b;
    int e;
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd == -1)
	return CHUBBY_E_IO;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Propose decree=Tag!!");
    chubby_getNewId(thiz,b.a+b.length);
    b.length += strlen(b.a+b.length);
    chubby_udp_buffer_format(&b,"!!");
    mangle(&b,what);
    e = chubby_propose1(thiz,fd,answer,&b);
    close(fd);
    return e;
}
