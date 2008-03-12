/* chubby.c */

#include "chubby_private.h"

#define TIMEOUT_USEC		1000000

static void
mangle(chubby_sb_t*dest,chubby_sb_t*src)
{
    int i;
    if (src->invalid)
	chubby_sb_free(dest);
    for (i=0; i<src->length; i++){
	char c=src->s[i];
	if (c==' ' || c=='!'){
	    chubby_sb_append_c(dest,'!');
	    c++;
	}
	chubby_sb_append_c(dest,c);
    }
}

static void
unmangle(chubby_sb_t*dest,chubby_sb_t*src)
{
    int i;
    if (src->invalid)
	chubby_sb_free(dest);
    for (i=0; i<src->length; i++){
	char c=src->s[i];
	if (c=='!' && i+1<src->length)
	    c = src->s[++i]-1;
	chubby_sb_append_c(dest,c);
    }
}

static void
chubby_delete(struct chubby_counted*_thiz)
{
    chubby_t*thiz=(chubby_t*)_thiz;
    chubby_sb_free(&thiz->addresses);
    chubby_sb_free(&thiz->id);
    free(thiz);
}

static int
chubby_init(chubby_t*thiz,const char*addresses)
{
    int i;
    char hostname[_POSIX_HOST_NAME_MAX];
    chubby_sb_init(&thiz->addresses);
    chubby_sb_init(&thiz->id);
    chubby_sb_append_format(&thiz->addresses,"%s",addresses);
    if (thiz->addresses.invalid)
	return 1;
    if (gethostname(hostname,_POSIX_HOST_NAME_MAX))
	return 1;
    chubby_sb_append_format(&thiz->id,"%s-%d-%d-%p",hostname,getpid(),(int)time(0),thiz);
    if (thiz->id.invalid)
	return 1;
    return 0;
}

chubby_t*
chubby_new(const char*addresses)
{
    chubby_t*thiz=malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_delete;
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
chubby_getId(chubby_t*thiz,chubby_sb_t*dest)
{
    chubby_sb_append_sb(dest,&thiz->id);
    chubby_sb_append_format(dest,"-%lld",++thiz->count);
}

static int
getSocketAddressByName(struct sockaddr_in*sa,char*a)
{
    char*b=strchr(a,':');
    struct hostend*h;
    memset(sa,0,sizeof*sa);
    sa->sin_len = sizeof*sa;
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
    char a[_POSIX_HOST_NAME_MAX+10];
    int i,j;
    struct sockaddr_in sa;
    for (i=0; i<thiz->addresses.length; i++){
	for (j=i; i<thiz->addresses.length&&thiz->addresses.s[i]!=','; i++);
	memcpy(a,thiz->addresses.s+j,i-j);
	a[i-j] = 0;
	if (getSocketAddressByName(&sa,a))
	    return CHUBBY_E_IO;
	sendto(fd,"GetLeader",9,0,&sa,sizeof sa);
    }
    return 0;
}

static int
chubby_propose1(chubby_t*thiz,chubby_sb_t*result,chubby_sb_t*what)
{
    timeval timeout;
    int e;
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd == -1)
	return CHUBBY_E_IO;
    timeout.tv_sec = TIMEOUT_USEC/1000000;
    timeout.tv_usec = TIMEOUT_USEC%1000000;
e=
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
assert(!e);
    for (;;){
	char buffer[65536];
	int length,i,j;
	if (!thiz->leaderValid){
	    e = chubby_sendGetLeader(thiz,fd);
	    if (e){
		close(fd);
		return e;
	    }
	}else
	    sendto(fd,what->s,what->length,0,&thiz->leader,sizeof thiz->leader);
	length = recvfrom(fd,buffer,sizeof buffer,0,NULL,NULL);
	if (length < 0){
	    close(fd);
	    return CHUBBY_E_IO;
	}

	for (i=0; i<length; i++){
	    for (j=i; i<length&&buffer[i]!=' '; i++);

	memcpy(a,thiz->addresses.s+j,i-j);
	}

    }

		p = p.split(' ')
		presult = None
		pdecree = None
		pcommand = p[0]
		for i in p[1:]:
		    j = i.index('=')
		    key = i[:j]
		    value = i[j+1:]
		    if key=="result":
			presult = self.__unmangle(value)
		    elif key=="decree":
			pdecree = self.__unmangle(value)
		if p[0]=="Redirect":
		    self.__leader = presult
		elif p[0]=="Result":
		    if pdecree==decree:
			if presult.startswith("error "):
			    return (int(presult[6:]),)
			return (0,presult)
		else:
		    assert 0
	finally:
	    fd.close()
}

int
chubby_propose(chubby_t*thiz,chubby_sb_t*result,chubby_sb_t*what)
{
    int e;
    chubby_sb_t sb1,sb2;
    chubby_sb_init(&sb1);
    chubby_sb_init(&sb2);
    chubby_sb_append_format(&sb1,"Tag ");
    chubby_getId(thiz,&sb1);
    chubby_sb_append_c(&sb1,' ');
    chubby_sb_append_sb(&sb1,what);
    chubby_sb_append_format(&sb2,"Propose decree=");
    mangle(&sb2,&sb1);
    chubby_sb_free(&sb1);
    if (sb2.invalid)
	return CHUBBY_E_OOM;
    e = chubby_propose1(thiz,result,sb2);
    chubby_sb_free(&sb2);
    return e;
}
/*

    def propose(self,decree):
	decree = "Tag %s "%self.getId()+decree
	fd = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
	try:
	    fd.settimeout(TIMEOUT)
	    while 1:
		if not self.__leader:
		    for a in self.__addresses.split(','):
			if a:
			    self.__sendto(fd,"GetLeader",a)
		else:
		    self.__sendto(fd,"Propose decree="+self.__mangle(decree),self.__leader)
		try:
		    p = fd.recv(65536)
		except socket.error,e:
		    if e.args[0]==11 or e.args[0]=="timed out":
			self.__leader = None
			continue
		    raise
		p = p.split(' ')
		presult = None
		pdecree = None
		pcommand = p[0]
		for i in p[1:]:
		    j = i.index('=')
		    key = i[:j]
		    value = i[j+1:]
		    if key=="result":
			presult = self.__unmangle(value)
		    elif key=="decree":
			pdecree = self.__unmangle(value)
		if p[0]=="Redirect":
		    self.__leader = presult
		elif p[0]=="Result":
		    if pdecree==decree:
			if presult.startswith("error "):
			    return (int(presult[6:]),)
			return (0,presult)
		else:
		    assert 0
	finally:
	    fd.close()

    @staticmethod
    def __sendto(fd,what,address):
	i = address.index(':')
	port = int(address[i+1:])
	host = address[0:i]
	fd.sendto(what,(socket.gethostbyname(host),port))
*/
