#define dontusethismacroname \
exec cc -DREALZOOKEEPER -o realzookeepertest zookeepertest.c -lzkmt -lpthread

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <zookeeper.h>

#ifndef REALZOOKEEPER
#include <apr.h>
#include <zookeeper_extra.h>
#else
#define APR_INT64_T_FMT "lld"
#endif

static int malloc_size;

static void*
test_malloc(size_t size)
{
    void*a=malloc(sizeof size+size);
    if (a){
	*(size_t*)a = size;
	a = (size_t*)a+1;
	malloc_size += size;
    }
    return a;
}

static void
test_free(void*p)
{
    p = (size_t*)p-1;
    malloc_size -= *(size_t*)p;
    free(p);
}


static char*
statToString(struct Stat*stat)
{
    static char s[300];
    snprintf(s,sizeof s,"czxid=%"APR_INT64_T_FMT" mzxid=%"APR_INT64_T_FMT" ctime=%"APR_INT64_T_FMT" mtime=%"APR_INT64_T_FMT" version=%d cversion=%d aversion=%d ephemeralOwner=%"APR_INT64_T_FMT,stat->czxid,stat->mzxid,stat->ctime,stat->mtime,stat->version,stat->cversion,stat->aversion,stat->ephemeralOwner);
    return s;
}

#define TEST(_a,_b,_c)   \
printf("%s\n",#_a);\
_a;\
printf("\t");\
printf _b;\
printf("\n");\
total++;\
if (!(_c))\
    printf("\tBad\n");\
else\
    passed++;\

int
main(int argc,char**argv)
{
    zhandle_t*zh;
    int rc;
    int passed=0,total=0;
    char s[100],t[350];
    struct Stat stat;
#ifndef REALZOOKEEPER
    zookeeperExtra_setMallocAndFree(test_malloc,test_free);
    if (gethostname(s,sizeof s)){
	perror("gethostname");
	exit(1);
    }
    sprintf(t,"%s:2000,%s:2001,%s:2002",s,s,s);
#else
    strcpy(t,"127.0.0.1:2000");
#endif
    TEST(zh=zookeeper_init(t,NULL,10000,0,0,0),("zh=%p",zh),zh)
    if (!zh)
	return 1;
    *s = 0;
    TEST(rc=zoo_create(zh,"/test","",0,&OPEN_ACL_UNSAFE,0,s,sizeof s),("rc=%s s=%s",zerror(rc),s),rc==ZOK||rc==ZNODEEXISTS)
    TEST(rc=zoo_create(zh,"/test/file","contents",8,&OPEN_ACL_UNSAFE,SEQUENCE,s,sizeof s),("rc=%s s=%s",zerror(rc),s),rc==ZOK)
    TEST(rc=zoo_delete(zh,s,1),("rc=%s",zerror(rc)),rc==ZBADVERSION)
    TEST(rc=zoo_delete(zh,s,0),("rc=%s",zerror(rc)),rc==ZOK)
    TEST(rc=zoo_delete(zh,"/test/file",-1),("rc=%s",zerror(rc)),rc==ZOK||rc==ZNONODE)
    TEST(rc=zoo_create(zh,"/test/file","contents",8,&OPEN_ACL_UNSAFE,0,s,sizeof s),("rc=%s s=%s",zerror(rc),s),rc==ZOK)
    TEST(rc=zoo_exists(zh,"/test/file",0,&stat),("rc=%s %s",zerror(rc),statToString(&stat)),rc==ZOK)
    TEST(rc=zoo_delete(zh,s,0),("rc=%s",zerror(rc)),rc==ZOK)
    TEST(rc=zoo_create(zh,"/test/file","contents",8,&OPEN_ACL_UNSAFE,0,s,sizeof s),("rc=%s s=%s",zerror(rc),s),rc==ZOK)
    TEST(rc=zoo_exists(zh,"/test/file",0,&stat),("rc=%s %s",zerror(rc),statToString(&stat)),rc==ZOK)
    TEST(rc=zoo_delete(zh,s,0),("rc=%s",zerror(rc)),rc==ZOK)
    memset(&stat,0,sizeof stat);
    TEST(rc=zoo_exists(zh,"/test/file",0,&stat),("rc=%s %s",zerror(rc),statToString(&stat)),rc==ZNONODE)
    zookeeper_close(zh);
    TEST(rc=0,("malloc_size=%d",malloc_size),malloc_size==0)
    printf("%d/%d passed\n",passed,total);
    return 0;
}
