#include <stdlib.h>
#include <sys/types.h>
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

int
main(int argc,char**argv)
{
    zhandle_t*zh;
    zookeeperExtra_setMallocAndFree(test_malloc,test_free);
    zh = zookeeper_init("127.0.0.1:2000",NULL,0,0,0,0);
    printf("zh=%p\n",zh);
    zookeeper_close(zh);
    printf("malloc_size=%d\n",malloc_size);
    return 0;
}
