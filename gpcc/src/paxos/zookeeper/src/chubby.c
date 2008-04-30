/* chubby.c */

#include "chubby_private.h"

#define PAXOS_PROTOCOL_VERSION	0

#define TIMEOUT		1

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
    zhandle_t*thiz=(zhandle_t*)_thiz;
    if (thiz->addresses)
	chubby_free(thiz->addresses);
    chubby_free(thiz);
}

static int
chubby_init(zhandle_t*thiz,const char*addresses)
{
    thiz->addresses = chubby_malloc(strlen(addresses)+1);
    if (!thiz->addresses)
	return 1;
    strcpy(thiz->addresses,addresses);
    if (gethostname(thiz->id,_POSIX_HOST_NAME_MAX))
	return 1;
    sprintf(thiz->id+strlen(thiz->id),"-%d-%d-%p",(int)getpid(),(int)time(NULL),thiz);
    return 0;
}

ZOOAPI zhandle_t*
zookeeper_init(const char*host,watcher_fn fn,int recv_timeout,const clientid_t*clientid,void*context,int flags)
{
    zhandle_t*thiz=chubby_malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_delete;
	if (chubby_init(thiz,host)){
	    chubby_counted_close(thiz);
	    return NULL;
	}
    }
    return thiz;
}

chubby_connection_t*
chubby_makeConnection(zhandle_t*thiz)
{
    return chubby_connection_new(thiz);
}

void
chubby_close(zhandle_t*thiz)
{
    chubby_counted_close(thiz);
}

void
chubby_getNewId(zhandle_t*thiz,char*a)
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
chubby_sendGetLeader(zhandle_t*thiz,int fd)
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
	sprintf(a,"GetLeader version=%d",PAXOS_PROTOCOL_VERSION);
	sendto(fd,a,strlen(a),0,(struct sockaddr*)&sa,sizeof sa);
    }
    return 0;
}

static int
chubby_propose1(zhandle_t*thiz,int fd,chubby_string_t**answer,chubby_udp_buffer_t*what)
{
    struct timeval timeout;
    int e;
    time_t getLeaderTimeout=0;
    time_t proposeTimeout=0;
    if (chubby_udp_buffer_isOverflowed(what))
	return CHUBBY_E_INVAL;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    for (;;){
	chubby_udp_buffer_t b;
	int i,j;
	int version=-1;
	char*command=NULL;
	int resultSize=0;
	char*result=NULL;
	int decreeSize=0;
	char*decree=NULL;
	time_t now=time(NULL);
	if (getLeaderTimeout < now){
	    getLeaderTimeout = now+TIMEOUT;
	    e = chubby_sendGetLeader(thiz,fd);
	    if (e)
		return e;
	}
	if (thiz->leaderValid && proposeTimeout<now){
	    proposeTimeout = now+TIMEOUT;
	    sendto(fd,what->a,what->length,0,(struct sockaddr*)&thiz->leader,sizeof thiz->leader);
	}
	b.length = recvfrom(fd,b.a,65535,0,NULL,NULL);
	if (b.length < 0)
	    continue;
	for (i=0; i<b.length; i++){
	    for (j=i; i<b.length&&b.a[i]!=' '; i++);
	    if (command == NULL){
		command = b.a+j;
		b.a[i] = 0;
	    }else if (!memcmp(b.a+j,"version=",8))
		version = atoi(b.a+j+8);
	    else if (!memcmp(b.a+j,"result=",7)){
		result = b.a+j+7;
		resultSize = unmangle(result,i-j-7);
		result[resultSize] = 0;
	    }else if (!memcmp(b.a+j,"decree=",7)){
		decree = b.a+j;
		decreeSize = i-j;
		decree[decreeSize] = 0;
	    }
	}
	if (version != PAXOS_PROTOCOL_VERSION)
	    continue;
	if (!strcmp(command,"Redirect")){
	    struct sockaddr_in l;
	    if (!getSocketAddressByName(&l,result) && memcmp(&l,&thiz->leader,sizeof l)){
		thiz->leaderValid = 1;
		thiz->leader = l;
		proposeTimeout = 0;
	    }
	}else if (!strcmp(command,"Result")){
	    const char*a=strstr(what->a,"decree=");
	    assert(a);
	    if (what->a+what->length==a+decreeSize && !memcmp(a,decree,decreeSize)){
		if (!memcmp(result,"error ",6))
		    return atoi(result+6);
		if (answer){
		    *answer = chubby_malloc(sizeof**answer+resultSize);
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
chubby_propose(zhandle_t*thiz,chubby_string_t**answer,chubby_string_t*what)
{
    chubby_udp_buffer_t b;
    int e;
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd == -1)
	return CHUBBY_E_IO;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Propose version=%d decree=Tag!!",PAXOS_PROTOCOL_VERSION);
    chubby_getNewId(thiz,b.a+b.length);
    b.length += strlen(b.a+b.length);
    chubby_udp_buffer_format(&b,"!!");
    mangle(&b,what);
    e = chubby_propose1(thiz,fd,answer,&b);
    close(fd);
    return e;
}
