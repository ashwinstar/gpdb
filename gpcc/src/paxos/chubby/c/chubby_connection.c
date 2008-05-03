/* chubby_connection.c */

#include "chubby_private.h"

struct chubby_connection{
    struct chubby_counted super;
    chubby_t*chubby;
    chubby_connection_t*parent;
    char lockFilename[PATH_MAX];
    char lockString[PATH_MAX+NUMBER_STRING_LENGTH+1];
    int64_t lockDn;
    char lockId[CHUBBY_ID_SIZE];
    time_t lockExpireTime;
};

struct chubby_string_linked_list{
    struct chubby_string_linked_list*next;
    chubby_string_t*s;
};

static void
fixFilename(chubby_udp_buffer_t*b,const char*name)
{
    for (; *name; name++)
	if (b->length < sizeof b->a)
	    b->a[b->length++] = *name==' '?0:*name;
}

static void
chubby_connection_delete(struct chubby_counted*_thiz)
{
    chubby_connection_t*thiz=(chubby_connection_t*)_thiz;
    void(*free)(void*)=thiz->chubby->free;
    chubby_counted_set(&thiz->chubby,NULL);
    chubby_counted_set(&thiz->parent,NULL);
    free(thiz);
}

chubby_connection_t*
chubby_connection_new(chubby_t*chubby)
{
    chubby_connection_t*thiz=chubby->malloc(sizeof*thiz);
    if (thiz){
	memset(thiz,0,sizeof*thiz);
	thiz->super.delet = chubby_connection_delete;
	chubby_counted_set(&thiz->chubby,chubby);
    }
    return thiz;
}

void
chubby_connection_close(chubby_connection_t*thiz)
{
    chubby_counted_close(thiz);
}

static void
reverse(char*a,int length)
{
    char*b=a+length-1;
    while (a < b){
	char tmp=*a;
	*a++ = *b;
	*b-- = tmp;
    }
}

static int
chubby_connection_propose(chubby_connection_t*thiz,chubby_string_t**answer,chubby_udp_buffer_t*b)
{
    int length=b->length;
    if (!thiz->parent)
	return chubby_propose(thiz->chubby,answer,(chubby_string_t*)b);
    chubby_udp_buffer_format(b,"If ");
    fixFilename(b,thiz->lockFilename);
    chubby_udp_buffer_format(b," %"APR_INT64_T_FMT" ",thiz->lockDn);
    reverse(b->a,length);
    reverse(b->a+length,b->length-length);
    reverse(b->a,b->length);
    return chubby_connection_propose(thiz->parent,answer,b);
}

static void
chubby_connection_freeChubbyStringLinkedList(chubby_connection_t*thiz,struct chubby_string_linked_list*ll)
{
    while (ll){
	struct chubby_string_linked_list*a=ll;
	ll = a->next;
	if (a->s)
	    thiz->chubby->free(a->s);
	thiz->chubby->free(a);
    }
}

static void
checkRenew(chubby_connection_t*thiz,int renewLockTime)
{
    if (renewLockTime && thiz->lockExpireTime<=time(NULL)+renewLockTime*3/4)
	chubby_connection_renew(thiz,renewLockTime);
}

int
chubby_connection_get(chubby_connection_t*thiz,struct chubby_stat*stat,chubby_string_t**answer,const char*name,int renewLockTime)
{
    int64_t dn=0;
    struct chubby_string_linked_list*ll=NULL,*m;
    int size=0;
    int e=0;
    for (;;){
	chubby_udp_buffer_t b;
	int64_t newdn=0;
	int n=0,s=0;
	b.length = 0;
	chubby_udp_buffer_format(&b,"Get ");
	fixFilename(&b,name);
	chubby_udp_buffer_format(&b," %d %d",size,CHUBBY_CHUNK_SIZE);
	m = thiz->chubby->malloc(sizeof*m);
	if (!m){
	    e = CHUBBY_E_OOM;
	    break;
	}
	m->next = ll;
	m->s = NULL;
	ll = m;
	e = chubby_connection_propose(thiz,&ll->s,&b);
	if (e)
	    break;
	sscanf(ll->s->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
	assert(n);
	if (dn != newdn){
	    dn = newdn;
	    if (size){
		chubby_connection_freeChubbyStringLinkedList(thiz,ll);
		ll = NULL;
		size = 0;
	    }else
		size = ll->s->length-n-1;
	}else
	    size += ll->s->length-n-1;
	if (size == s){
	    *answer = thiz->chubby->malloc(sizeof**answer+size);
	    if (!*answer){
		e = CHUBBY_E_OOM;
		break;
	    }
	    (*answer)->length = size;
	    (*answer)->a[size] = 0;
	    if (stat){
		stat->decreeNumber = dn;
		stat->size = size;
	    }
	    for (m=ll; m; m=m->next){
		sscanf(m->s->a,"%"APR_INT64_T_FMT" %d%n",&newdn,&s,&n);
		s = m->s->length-n-1;
		size -= s;
		assert(size >= 0);
		memcpy((*answer)->a+size,m->s->a+n+1,s);
	    }
	    assert(size == 0);
	    break;
	}
	checkRenew(thiz,renewLockTime);
    }
    chubby_connection_freeChubbyStringLinkedList(thiz,ll);
    return e;
}

int
chubby_connection_stat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name)
{
    chubby_udp_buffer_t b;
    chubby_string_t*answer;
    int e;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Stat ");
    fixFilename(&b,name);
    e = chubby_connection_propose(thiz,&answer,&b);
    if (e == CHUBBY_E_NOENT){
	stat->decreeNumber = 0;
	stat->size = 0;
	e = 0;
    }else if (!e){
	sscanf(answer->a,"%"APR_INT64_T_FMT" %d",&stat->decreeNumber,&stat->size);
	thiz->chubby->free(answer);
    }
    return e;
}

static int
chubby_connection_chubbyStat(chubby_connection_t*thiz,struct chubby_stat*stat,const char*name)
{
    chubby_udp_buffer_t b;
    chubby_string_t*answer;
    int e;
    b.length = 0;
    chubby_udp_buffer_format(&b,"Stat ");
    fixFilename(&b,name);
    e = chubby_propose(thiz->chubby,&answer,(chubby_string_t*)&b);
    if (!e){
	sscanf(answer->a,"%"APR_INT64_T_FMT" %d",&stat->decreeNumber,&stat->size);
	thiz->chubby->free(answer);
    }
    return e;
}

int
chubby_connection_put(chubby_connection_t*thiz,const char*name,chubby_string_t*contents)
{
    chubby_udp_buffer_t b;
    struct chubby_stat stat;
    int e=chubby_connection_stat(thiz,&stat,name);
    if (e)
	return e;
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    fixFilename(&b,name);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Put ",stat.decreeNumber);
    fixFilename(&b,name);
    chubby_udp_buffer_format(&b," 0 ");
    chubby_udp_buffer_cat(&b,contents);
    e = chubby_connection_propose(thiz,NULL,&b);
    if (e == CHUBBY_E_IFFAILED){
	int64_t dn=stat.decreeNumber;
	e = chubby_connection_chubbyStat(thiz,&stat,name);
	if (e==0 && dn==stat.decreeNumber)
	    return CHUBBY_E_LOCKEXPIRED;
    }
    return e;
}

int
chubby_connection_putBigFile(chubby_connection_t*thiz,const char*name,chubby_string_t*contents,int renewLockTime)
{
    int offset=0,len,e;
    chubby_udp_buffer_t b;
    do{
	b.length = 0;
	chubby_udp_buffer_format(&b,"Put ");
	fixFilename(&b,name);
	chubby_udp_buffer_format(&b," %d ",offset);
	len = contents->length-offset;
	if (len > CHUBBY_CHUNK_SIZE)
	    len = CHUBBY_CHUNK_SIZE;
	for (; len&&b.length<sizeof b.a; --len)
	    b.a[b.length++] = contents->a[offset++];
	e = chubby_connection_propose(thiz,NULL,&b);
	checkRenew(thiz,renewLockTime);
    }while (!e && offset<contents->length);
    return e;
}

int
chubby_connection_remove(chubby_connection_t*thiz,const char*name)
{
    struct chubby_stat stat;
    chubby_udp_buffer_t b;
    int e=chubby_connection_stat(thiz,&stat,name);
    if (!e){
	if (stat.decreeNumber == 0)
	    return CHUBBY_E_NOENT;
	b.length = 0;
	chubby_udp_buffer_format(&b,"If ");
	fixFilename(&b,name);
	chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Remove ",stat.decreeNumber);
	fixFilename(&b,name);
	e = chubby_connection_propose(thiz,NULL,&b);
	if (e == CHUBBY_E_NOENT)
	    return 0;
	if (e == CHUBBY_E_IFFAILED){
	    int64_t dn=stat.decreeNumber;
	    e = chubby_connection_chubbyStat(thiz,&stat,name);
	    if (e==0 && dn==stat.decreeNumber)
		return CHUBBY_E_LOCKEXPIRED;
	}
    }
    return e;
}

static int
chubby_connection_list1(chubby_connection_t*thiz,chubby_string_t**a,char***answer,const char*name,int renewLockTime)
{
    int e=chubby_connection_get(thiz,NULL,a,name,renewLockTime);
    if (!e){
	int count=0,i;
	if ((*a)->length != 0){
	    count++;
	    for (i=0; i<(*a)->length; i++)
		if ((*a)->a[i] == ' ')
		    count++;
	}
	*answer = thiz->chubby->malloc(sizeof*answer*(count+1));
	if (!*answer)
	    return CHUBBY_E_OOM;
	**answer = NULL;
	if (count){
	    char*s=thiz->chubby->malloc((*a)->length+1);
	    if (!s)
		return CHUBBY_E_OOM;
	    count = 0;
	    for (i=0; i<(*a)->length; i++){
		(*answer)[count++] = s;
		for (; i<(*a)->length&&(*a)->a[i]!=' '; i++)
		    *s++ = (*a)->a[i]==0?' ':(*a)->a[i];
		*s++ = 0;
	    }
	    (*answer)[count] = NULL;
	}
    }
    return e;
}

int
chubby_connection_list(chubby_connection_t*thiz,char***answer,const char*name,int renewLockTime)
{
    chubby_string_t*a=NULL;
    char**aa=NULL;
    int e=chubby_connection_list1(thiz,&a,&aa,name,renewLockTime);
    if (a)
	thiz->chubby->free(a);
    if (e)
	chubby_connection_listFree(thiz,aa);
    else
	*answer = aa;
    return e;
}

void
chubby_connection_listFree(chubby_connection_t*thiz,char**answer)
{
    if (answer){
	if (*answer)
	    thiz->chubby->free(*answer);
	thiz->chubby->free(answer);
    }
}

chubby_connection_t*
chubby_connection_getLockFilenameDecreeNumberConnection(chubby_connection_t*thiz,const char*name,int64_t decreeNumber)
{
    chubby_connection_t*c=chubby_connection_new(thiz->chubby);
    if (c){
	chubby_counted_set(&c->parent,thiz);
	c->lockDn = decreeNumber;
	strcpy(c->lockFilename,name);
    }
    return c;
}

int
chubby_connection_getLockStringConnection(chubby_connection_t*thiz,chubby_connection_t**answer,const char*lockString)
{
    int n=0;
    int64_t dn=0;
    sscanf(lockString,"%"APR_INT64_T_FMT"-%n",&dn,&n);
    *answer = chubby_connection_getLockFilenameDecreeNumberConnection(thiz,lockString+n,dn);
    return 0;
}

int
chubby_connection_getLockString(chubby_connection_t*thiz,chubby_string_t**answer)
{
    char ls[PATH_MAX+NUMBER_STRING_LENGTH+1];
    sprintf(ls,"%"APR_INT64_T_FMT"-%s",thiz->lockDn,thiz->lockFilename);
    *answer = thiz->chubby->malloc(sizeof(chubby_string_t)+strlen(ls));
    if (!*answer)
	return CHUBBY_E_OOM;
    (*answer)->length = strlen(ls);
    strcpy((*answer)->a,ls);
    return 0;
}

int
chubby_connection_checkLock(chubby_connection_t*thiz)
{
    struct chubby_stat stat;
    int e=chubby_connection_stat(thiz,&stat,thiz->lockFilename);
    return e==0&&stat.decreeNumber==thiz->lockDn?0:CHUBBY_E_LOCKEXPIRED;
}

int
chubby_connection_lock(chubby_connection_t*thiz,chubby_connection_t**answer,const char*name,int timeout,int lockTime)
{
    int e;
    time_t timedout=time(NULL)+timeout;
    time_t expire=0,now;
    int waitTime;
    int64_t dn=0;
    struct chubby_stat stat;
    chubby_string_t*a;
    char id[CHUBBY_ID_SIZE];
    char info[PATH_MAX];
    sprintf(info,"%s-info",name);
    chubby_getNewId(thiz->chubby,id);
    while (time(NULL) < timedout){
	e = chubby_connection_get(thiz,&stat,&a,info,0);
	if (!e){
	    sscanf(a->a,"%d",&waitTime);
	    thiz->chubby->free(a);
	}else if (e == CHUBBY_E_NOENT)
	    stat.decreeNumber = 0;
	else
	    return e;
	now = time(NULL);
	if (stat.decreeNumber > dn){
	    expire = now+waitTime+1;
	    dn = stat.decreeNumber;
	}
	if (stat.decreeNumber==0 || expire<now){
	    a = chubby_string_format(thiz->chubby,"%s",id);
	    if (!a)
		return CHUBBY_E_OOM;
	    e = chubby_connection_put(thiz,name,a);
	    thiz->chubby->free(a);
	    if (e)
		return e;
	    e = chubby_connection_get(thiz,&stat,&a,name,0);
	    if (e)
		return e;
	    if (a->length==strlen(id) && !memcmp(a->a,id,a->length)){
		chubby_connection_t*c=chubby_connection_new(thiz->chubby);
		thiz->chubby->free(a);
		if (!c)
		    return CHUBBY_E_OOM;
		chubby_counted_set(&c->parent,thiz);
		c->lockDn = stat.decreeNumber;
		strcpy(c->lockFilename,name);
		strcpy(c->lockId,id);
		e = chubby_connection_renew(c,lockTime);
		if (e){
		    chubby_connection_close(c);
		    return CHUBBY_E_LOCKEXPIRED;
		}
		*answer = c;
		return 0;
	    }
	    thiz->chubby->free(a);
	}
	sleep(1);
    }
    return CHUBBY_E_TIMEOUT;
}

int
chubby_connection_renew(chubby_connection_t*thiz,int lockTime)
{
    chubby_string_t*a,*b;
    char info[PATH_MAX];
    time_t now=time(NULL);
    int e;
    if (!*thiz->lockId)
	return CHUBBY_E_INVAL;
    a = chubby_string_format(thiz->chubby,"%d %s",lockTime,thiz->lockId);
    if (!a)
	return CHUBBY_E_OOM;
    sprintf(info,"%s-info",thiz->lockFilename);
    e = chubby_connection_put(thiz,info,a);
    if (!e){
	e = chubby_connection_get(thiz,NULL,&b,info,0);
	if (!e){
	    if (a->length==b->length && !memcmp(a->a,b->a,a->length))
		thiz->lockExpireTime = now+lockTime;
	    else
		e = CHUBBY_E_LOCKEXPIRED;
	    thiz->chubby->free(b);
	}
    }
    thiz->chubby->free(a);
    return e;
}

int
chubby_connection_getLockExpireTime(chubby_connection_t*thiz,time_t*t)
{
    *t = thiz->lockExpireTime;
    return 0;
}

void
chubby_connection_giveUpLock(chubby_connection_t*thiz)
{
    chubby_connection_renew(thiz,0);
}

int
chubby_connection_move(chubby_connection_t*thiz,const char*fromname,const char*toname)
{
    struct chubby_stat fstat,tstat;
    int e=chubby_connection_stat(thiz,&fstat,fromname);
    chubby_udp_buffer_t b;
    if (e)
        return e;
    if (!fstat.decreeNumber)
    	return CHUBBY_E_NOENT;
    e = chubby_connection_stat(thiz,&tstat,toname);
    if (e)
        return e;
    b.length = 0;
    chubby_udp_buffer_format(&b,"If ");
    fixFilename(&b,fromname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" If ",fstat.decreeNumber);
    fixFilename(&b,toname);
    chubby_udp_buffer_format(&b," %"APR_INT64_T_FMT" Move ",tstat.decreeNumber);
    fixFilename(&b,fromname);
    chubby_udp_buffer_format(&b," ");
    fixFilename(&b,toname);
    e = chubby_connection_propose(thiz,NULL,&b);
    if (e == CHUBBY_E_IFFAILED){
	e = chubby_connection_chubbyStat(thiz,&tstat,fromname);
	if (e==0 && fstat.decreeNumber==tstat.decreeNumber)
	    return CHUBBY_E_LOCKEXPIRED;
    }
    return e;
}
