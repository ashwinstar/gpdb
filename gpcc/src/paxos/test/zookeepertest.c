#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <zookeeper_extra.h>

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
    zookeeperExtra_setMallocAndFree(test_malloc,test_free);
    if (gethostname(s,sizeof s)){
	perror("gethostname");
	exit(1);
    }
    sprintf(t,"%s:2000,%s:2001,%s:2002",s,s,s);
    TEST(zh=zookeeper_init(t,NULL,0,0,0,0),("zh=%p",zh),zh)
    if (!zh)
	return 1;
    TEST(rc=zoo_create(zh,"test///file","contents",8,NULL,SEQUENCE,s,sizeof s),("s=%s rc=%s",s,zerror(rc)),rc==ZOK)
    TEST(rc=zoo_delete(zh,s,0),("rc=%s",zerror(rc)),rc==ZBADVERSION)
    TEST(rc=zoo_delete(zh,s,1),("rc=%s",zerror(rc)),rc==ZOK)
    zookeeper_close(zh);
    TEST(rc=0,("malloc_size=%d",malloc_size),malloc_size==0)
    printf("%d/%d passed\n",passed,total);
    return 0;
}
