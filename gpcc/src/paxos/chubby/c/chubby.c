/* chubby.c */

#include "chubby_private.h"

#define PAXOS_PROTOCOL_VERSION	0

#define TIMEOUT		1000000

struct chubby_proposal{
    struct chubby_proposal*next;
    chubby_asynchronous_callback_t callback;
    void*data;
    apr_time_t bigTimeout;
    apr_time_t proposeTimeout;
    chubby_string_t**answer;
    chubby_udp_buffer_t b;
};

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

static int
getSocketAddressByName(struct sockaddr_in*sa,char*a)
{
    char*b=strchr(a,':');
    struct hostent*h;
    memset(sa,0,sizeof*sa);
    sa->sin_family = AF_INET;
    if (b){
        *b++ = 0;
	sa->sin_port = htons(atoi(b));
    }
    h = gethostbyname(a);
    if (!h)
	return 1;
    if (h->h_length!=sizeof(struct in_addr) || !h->h_addr)
	return 1;
    sa->sin_addr = *(struct in_addr*)h->h_addr;
    return 0;
}

static void
chubby_proposal_done(chubby_t*thiz,struct chubby_proposal*p,int e)
{
    p->callback(e,p->data);
    thiz->free(p);
}

static void
chubby_delete(struct chubby_counted*_thiz)
{
    chubby_t*thiz=(chubby_t*)_thiz;
    while (thiz->proposal){
	struct chubby_proposal*p=thiz->proposal;
	thiz->proposal = p->next;
	chubby_proposal_done(thiz,p,CHUBBY_E_CLOSED);
    }
    if (thiz->addresses)
	thiz->free(thiz->addresses);
    if (thiz->paxosKey)
	thiz->free(thiz->paxosKey);
    if (thiz->fd != -1)
	close(thiz->fd);
    thiz->free(thiz);
}

static int
chubby_init(chubby_t*thiz,const char*addresses)
{
    char hostname[_POSIX_HOST_NAME_MAX];
    socklen_t namelen=sizeof(struct sockaddr);
    thiz->fd = socket(AF_INET,SOCK_DGRAM,0);
    if (thiz->fd == -1)
	return 1;
    thiz->addresses = thiz->malloc(strlen(addresses)+1);
    if (!thiz->addresses)
	return 1;
    strcpy(thiz->addresses,addresses);
    if (gethostname(thiz->id,_POSIX_HOST_NAME_MAX))
	return 1;
    if (chubby_readPaxosKey(thiz))
	return 1;
    sprintf(thiz->id+strlen(thiz->id),"-%d-%"APR_TIME_T_FMT"-%p",(int)getpid(),apr_time_now(),thiz);
    if (gethostname(hostname,_POSIX_HOST_NAME_MAX))
	return 1;
    if (getSocketAddressByName((struct sockaddr_in*)&thiz->socketName,hostname))
	return 1;
    bind(thiz->fd,&thiz->socketName,sizeof(struct sockaddr));
    getsockname(thiz->fd,&thiz->socketName,&namelen);
    return 0;
}

chubby_t*
chubby_new(const char*addresses,int timeout,void*(*malloc)(size_t),void(*free)(void*))
{
    chubby_t*thiz=malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_delete;
	thiz->malloc = malloc;
	thiz->free = free;
	thiz->timeout = timeout;
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

void
chubby_two_int_callback(int e,void*data)
{
    (0[(int*)data]=1)[(int*)data] = e;
}

int
chubby_two_int_finish(chubby_t*thiz,int a[2],int e)
{
    if (e)
	return e;
    for (;;){
	fd_set rset;
	int fd;
	struct timeval tv;
	chubby_asynchronous_think(thiz,&fd,&tv);
	if (a[0])
	    return a[1];
	FD_ZERO(&rset);
	FD_SET(fd,&rset);
	select(fd+1,&rset,NULL,NULL,&tv);
    }
}

int
chubby_asynchronous_propose(chubby_t*thiz,chubby_asynchronous_callback_t callback,void*data,chubby_string_t**answer,chubby_string_t*what)
{
    struct chubby_proposal*p=thiz->malloc(sizeof*p);
    if (!p)
	return CHUBBY_E_OOM;
    p->b.length = 0;
    chubby_udp_buffer_format(&p->b,SIGNSTRING"Propose version=%d decree=Tag!!",PAXOS_PROTOCOL_VERSION);
    chubby_getNewId(thiz,p->b.a+p->b.length);
    p->b.length += strlen(p->b.a+p->b.length);
    chubby_udp_buffer_format(&p->b,"!!");
    mangle(&p->b,what);
    if (chubby_udp_buffer_isOverflowed(&p->b)){
	thiz->free(p);
	return CHUBBY_E_INVAL;
    }
    p->callback = callback;
    p->data = data;
    p->bigTimeout = apr_time_now()+apr_time_from_sec(thiz->timeout);
    p->proposeTimeout = 0;
    p->answer = answer;
    p->next = thiz->proposal;
    thiz->proposal = p;
    return 0;
}

static void
chubby_asynchronous_checkTimeouts(chubby_t*thiz)
{
    apr_time_t now=apr_time_now();
    struct chubby_proposal**p=&thiz->proposal;
    while (*p){
	struct chubby_proposal*q=*p;
	if (now > q->bigTimeout){
	    *p = q->next;
	    chubby_proposal_done(thiz,q,CHUBBY_E_TIMEOUT);
	}else
	    p = &q->next;
    }
}

static void
chubby_asynchronous_sendGetLeader(chubby_t*thiz)
{
    apr_time_t now=apr_time_now();
    if (now >= thiz->getLeaderTimeout){
	char a[_POSIX_HOST_NAME_MAX+NUMBER_STRING_LENGTH+1],*s;
	struct sockaddr sa;
	thiz->getLeaderTimeout = now+TIMEOUT;
	for (s=thiz->addresses; *s; s++){
	    char*t=s;
	    s = strchr(t,',');
	    if (!s)
		s = t+strlen(t);
	    memcpy(a,t,s-t);
	    a[s-t] = 0;
	    if (!getSocketAddressByName((struct sockaddr_in*)&sa,a)){
		sprintf(a,SIGNSTRING"GetLeader version=%d",PAXOS_PROTOCOL_VERSION);
		chubby_sign(thiz,a,a,strlen(a),&thiz->socketName,&sa);
		sendto(thiz->fd,a,strlen(a),0,&sa,sizeof sa);
	    }
	}
    }
}

static void
chubby_asynchronous_sendProposal(chubby_t*thiz)
{
    struct chubby_proposal*p;
    apr_time_t now=apr_time_now();
    for (p=thiz->proposal; p; p=p->next)
	if (now >= p->proposeTimeout){
	    p->proposeTimeout = now+TIMEOUT;
	    chubby_sign(thiz,p->b.a,p->b.a,p->b.length,&thiz->socketName,&thiz->leader);
	    sendto(thiz->fd,p->b.a,p->b.length,0,&thiz->leader,sizeof thiz->leader);
	}
}

static void
chubby_asynchronous_recv(chubby_t*thiz,chubby_udp_buffer_t*b)
{
    int i,j;
    int version=-1;
    char*command=NULL;
    int resultSize=0;
    char*result=NULL;
    int decreeSize=0;
    char*decree=NULL;
    for (i=SIGNSTRING_LENGTH; i<b->length; i++){
	for (j=i; i<b->length&&b->a[i]!=' '; i++);
	if (command == NULL){
	    command = b->a+j;
	    b->a[i] = 0;
	}else if (!memcmp(b->a+j,"version=",8))
	    version = atoi(b->a+j+8);
	else if (!memcmp(b->a+j,"result=",7)){
	    result = b->a+j+7;
	    resultSize = unmangle(result,i-j-7);
	    result[resultSize] = 0;
	}else if (!memcmp(b->a+j,"decree=",7)){
	    decree = b->a+j;
	    decreeSize = i-j;
	    decree[decreeSize] = 0;
	}
    }
    if (version != PAXOS_PROTOCOL_VERSION)
	return;
    if (!strcmp(command,"Redirect")){
	struct sockaddr l;
	if (!getSocketAddressByName((struct sockaddr_in*)&l,result) && memcmp(&l,&thiz->leader,sizeof l)){
	    struct chubby_proposal*p;
	    thiz->leaderValid = 1;
	    thiz->leader = l;
	    for (p=thiz->proposal; p; p=p->next)
		p->proposeTimeout = 0;
	}
    }else if (!strcmp(command,"Result")){
	struct chubby_proposal**p=&thiz->proposal;
	while (*p){
	    struct chubby_proposal*q=*p;
	    const char*a=strstr(q->b.a,"decree=");
	    assert(a);
	    if (q->b.a+q->b.length==a+decreeSize && !memcmp(a,decree,decreeSize)){
		if (!memcmp(result,"error ",6)){
		    *p = q->next;
		    chubby_proposal_done(thiz,q,atoi(result+6));
		    continue;
		}
		if (q->answer){
		    *q->answer = thiz->malloc(sizeof**q->answer+resultSize);
		    if (!*q->answer){
			*p = q->next;
			chubby_proposal_done(thiz,q,CHUBBY_E_OOM);
			continue;
		    }
		    (*q->answer)->length = resultSize;
		    memcpy((*q->answer)->a,result,resultSize);
		    (*q->answer)->a[resultSize] = 0;
		}
		*p = q->next;
		chubby_proposal_done(thiz,q,0);
		continue;
	    }
	    p = &q->next;
	}
    }else
	assert(0);
}

void
chubby_asynchronous_think(chubby_t*thiz,int*fd,struct timeval*tv)
{
    fd_set rset;
    if (thiz->timeout > 0)
	chubby_asynchronous_checkTimeouts(thiz);
    for (;;){
	int i;
	struct timeval zero={0,0};
	struct sockaddr from;
	socklen_t fromlen=sizeof from;
	char signature[SIGNSTRING_LENGTH];
	chubby_udp_buffer_t b;
	FD_ZERO(&rset);
	FD_SET(thiz->fd,&rset);
	i = select(thiz->fd+1,&rset,NULL,NULL,&zero);
	if (i <= 0)
	    break;
	b.length = recvfrom(thiz->fd,b.a,65535,0,&from,&fromlen);
	if (b.length < SIGNSTRING_LENGTH)
	    continue;
	chubby_sign(thiz,signature,b.a,b.length,&from,&thiz->socketName);
	if (memcmp(signature,b.a,SIGNSTRING_LENGTH))
	    continue;
	chubby_asynchronous_recv(thiz,&b);
    }
    chubby_asynchronous_sendGetLeader(thiz);
    if (thiz->leaderValid)
	chubby_asynchronous_sendProposal(thiz);
    if (fd)
	*fd = thiz->fd;
    if (tv){
	tv->tv_sec = apr_time_sec(TIMEOUT);
	tv->tv_usec = apr_time_usec(TIMEOUT);
    }
}

int
chubby_propose(chubby_t*thiz,chubby_string_t**answer,chubby_string_t*what)
{
    int a[2]={0};
    return chubby_two_int_finish(thiz,a,chubby_asynchronous_propose(thiz,chubby_two_int_callback,a,answer,what));
}
