/* chubbytest.c */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "chubby.h"

#define CHUNK_SIZE	30000
#define STRING_LENGTH	100

struct test_declarations{
    int ret;
    time_t time;
    chubby_string_t*answer;
    char**list;
    char catlist[STRING_LENGTH];
    struct chubby_stat stat;
};

#define TEST(aa,bb)\
    do{\
	t.answer=0;\
	t.list=0;\
	t.stat.decreeNumber = 0;\
	t.stat.size = 0;\
	t.time = 0;\
	ntest++;\
	printf("%s%s\n",prefix,#aa);\
	t.ret = aa;\
	*t.catlist = 0;\
	if (t.list){\
	    char**b;\
	    strcpy(t.catlist,",");\
	    for (b=t.list; *b; b++){\
		strcat(t.catlist,*b);\
		strcat(t.catlist,",");\
	    }\
	}\
	printf("%s\t",prefix);\
	if (bb)\
	    passed++;\
	else\
	    printf("BAD ");\
	printf("%d",t.ret);\
	if (t.stat.decreeNumber || t.stat.size)\
	    printf(" %d %d",(int)t.stat.decreeNumber,t.stat.size);\
	if (t.answer){\
	    printf(" %s",t.answer->a);\
	    counted_free(t.answer);\
	}\
	if (t.list){\
	    printf(" %s",t.catlist);\
	    chubby_connection_listFree(c,t.list);\
	}\
	if (t.time != 0)\
	    printf(" time=now%+d",(int)(t.time-time(NULL)));\
	printf("\n");\
    }while(0)

struct string{
    int length;
    char a[STRING_LENGTH];
};

static int malloc_size;
static int passed,ntest;

static void*
counted_malloc(size_t size)
{
    int*a=malloc(size+sizeof(int));
    if (a){
	*a++ = size;
	malloc_size += size;
    }
    return a;
}

static void
counted_free(void*a)
{
    malloc_size -= ((int*)a)[-1];
    free((int*)a-1);
}

static chubby_string_t*
make_string(struct string*a,const char*b)
{
    a->length = strlen(b);
    strcpy(a->a,b);
    return (chubby_string_t*)a;
}

static void
test1(int argc,char**argv,chubby_connection_t*c)
{
    const char*prefix="test1\t";
    struct test_declarations t;
    struct string s;
    chubby_connection_t*c2=NULL;
    int i;
    TEST(chubby_connection_lock(c,&c2,"test/lock2",10,5),t.ret==0);
    if (c2){
	for (i=0; i<10; i++){
	    time_t now;
	    TEST(chubby_connection_renew(c2,5),t.ret==0);
	    now = time(NULL);
	    TEST(chubby_connection_getLockExpireTime(c2,&t.time),t.ret==0&&now+3<t.time&&t.time<=now+5);
	    sleep(3);
	}
	chubby_connection_close(c2);
	c2 = NULL;
    }
    sleep(7);
    TEST(chubby_connection_lock(c,&c2,"test/lock3",10,5),1);
    if (c2){
	chubby_connection_close(c2);
	c2 = NULL;
    }
    sprintf(s.a,"%d/%d",passed,ntest);
    s.length = strlen(s.a);
    TEST(chubby_connection_put(c,"test/test1result",(chubby_string_t*)&s),t.ret==0);
}

static void
test2(int argc,char**argv,chubby_connection_t*c)
{
    const char*prefix="test2\t";
    struct test_declarations t;
    struct string s;
    chubby_connection_t*c2=NULL;
    TEST(chubby_connection_getLockStringConnection(c,&c2,argv[2]),t.ret==0);
    if (c2){
	TEST(chubby_connection_list(c2,&t.list,"/"),t.ret==0);
	sleep(10);
	TEST(chubby_connection_list(c2,&t.list,"/"),t.ret==CHUBBY_E_LOCKEXPIRED);
	chubby_connection_close(c2);
	c2 = NULL;
    }
    sprintf(s.a,"%d/%d",passed,ntest);
    s.length = strlen(s.a);
    TEST(chubby_connection_put(c,"test/test2result",(chubby_string_t*)&s),t.ret==0);
}

static void
test0(int argc,char**argv,chubby_connection_t*c)
{
    const char*prefix="\t";
    struct test_declarations t;
    struct string s;
    chubby_connection_t*c2=NULL;
    int64_t dn;
    TEST(chubby_connection_put(c,"/test/file",make_string(&s,"contents")),t.ret==0);
    TEST(chubby_connection_get(c,&t.stat,&t.answer,"/test/file"),t.ret==0&&!strcmp(t.answer->a,"contents"));
    TEST(chubby_connection_list(c,&t.list,"/"),t.ret==0&&!strcmp(t.catlist,",paxos,test,"));
    TEST(chubby_connection_list(c,&t.list,"noentfile"),t.ret==1);
    TEST(chubby_connection_stat(c,&t.stat,"test/file"),t.ret==0);
    TEST(chubby_connection_remove(c,"//test//1/1"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_put(c,"test///1/1",make_string(&s,"")),t.ret==0);
    TEST(chubby_connection_stat(c,&t.stat,"test/1//1"),t.ret==0);
    dn = t.stat.decreeNumber;
    TEST(chubby_connection_stat(c,&t.stat,"test/1"),t.ret==0&&t.stat.decreeNumber==dn);
    TEST(chubby_connection_stat(c,&t.stat,"test"),t.ret==0&&t.stat.decreeNumber==dn);
    TEST(chubby_connection_remove(c,"//test//1/1"),t.ret==0);
    TEST(chubby_connection_stat(c,&t.stat,"test"),t.ret==0&&t.stat.decreeNumber>dn);
    TEST(chubby_connection_list(c,&t.list,"/test"),t.ret==0&&!strstr(t.catlist,",1,"));
    {
	int i;
	char*a;
	chubby_string_t*big=counted_malloc(sizeof*big+CHUNK_SIZE*10);
	if (!big){
	    fprintf(stderr,"malloc failed\n");
	    exit(1);
	}
	a = big->a;
	memset(a,'!',CHUNK_SIZE);
	a += CHUNK_SIZE;
	for (i=0; i<CHUNK_SIZE; i++){
	    sprintf(a,"%d",i);
	    a += strlen(a);
	}
	big->length = a-big->a;
	TEST(chubby_connection_putBigFile(c,"///test/bi g",big),t.ret==0);
	TEST(chubby_connection_get(c,&t.stat,&t.answer,"/test//bi g"),t.ret==0&&!strcmp(t.answer->a,big->a));
	counted_free(big);
    }
    TEST(chubby_connection_list(c,&t.list,"/test"),t.ret==0&&strstr(t.catlist,",bi g,"));
    TEST(chubby_connection_remove(c,"/test/bi g"),t.ret==0);
    TEST(chubby_connection_list(c,&t.list,"/test"),t.ret==0&&!strstr(t.catlist,",bi g,"));
    TEST(chubby_connection_remove(c,"/test/lock"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_remove(c,"/test/lock-info"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_remove(c,"/test/lock2"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_remove(c,"/test/lock2-info"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_remove(c,"/test/lock3"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_remove(c,"/test/lock3-info"),t.ret==0||t.ret==CHUBBY_E_NOENT);
    TEST(chubby_connection_put(c,"/test/test1result",make_string(&s,"no")),t.ret==0);
    TEST(chubby_connection_put(c,"/test/test2result",make_string(&s,"no")),t.ret==0);
    TEST(chubby_connection_lock(c,&c2,"/test//lock",10,5),t.ret==0);
    if (c2){
	TEST(chubby_connection_getLockString(c2,&t.answer),t.ret==0);
	TEST(chubby_connection_checkLock(c2),t.ret==0);
	TEST(chubby_connection_list(c2,&t.list,"/"),t.ret==0);
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_lock(c,&c2,"test/lock",1,5),t.ret==CHUBBY_E_TIMEOUT);
    if (c2){
	chubby_connection_close(c2);
	c2 = NULL;
    }
    sprintf(s.a,"%s test1&",argv[0]);
    system(s.a);
    sleep(5);
    TEST(chubby_connection_lock(c,&c2,"test/lock2",10,5),t.ret==CHUBBY_E_TIMEOUT);
    if (c2){
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_lock(c,&c2,"test/lock2",30,5),t.ret==0);
    if (c2){
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_lock(c,&c2,"test/lock3",10,5),t.ret==0);
    if (c2){
	TEST(chubby_connection_checkLock(c2),t.ret==0);
	TEST(chubby_connection_list(c2,&t.list,"/"),t.ret==0);
	sleep(10);
	TEST(chubby_connection_checkLock(c2),t.ret==CHUBBY_E_LOCKEXPIRED);
	TEST(chubby_connection_list(c2,&t.list,"/"),t.ret==CHUBBY_E_LOCKEXPIRED);
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_lock(c,&c2,"test/lock",10,5),t.ret==0);
    if (c2){
	chubby_string_t*a=NULL;
	TEST(chubby_connection_getLockString(c2,&a),t.ret==0);
	if (a){
	    sprintf(s.a,"%s test2 %s&",argv[0],a->a);
	    counted_free(a);
	    system(s.a);
	}
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_lock(c,&c2,"test/lock",10,5),t.ret==0);
    if (c2){
	chubby_connection_close(c2);
	c2 = NULL;
    }
    TEST(chubby_connection_get(c,&t.stat,&t.answer,"/test/test1result"),t.ret==0&&!strcmp(t.answer->a,"22/22"));
    sleep(10);
    TEST(chubby_connection_get(c,&t.stat,&t.answer,"/test/test2result"),t.ret==0&&!strcmp(t.answer->a,"3/3"));
}

int
main(int argc,char**argv)
{
    char hostname[_POSIX_HOST_NAME_MAX];
    char addresses[3*_POSIX_HOST_NAME_MAX+20];
    chubby_t*chubby;
    chubby_connection_t*c;
    if (gethostname(hostname,_POSIX_HOST_NAME_MAX)){
	perror("gethostname");
	exit(1);
    }
    sprintf(addresses,"%s:2000,%s:2001,%s:2002",hostname,hostname,hostname);
    chubby = chubby_new(addresses,counted_malloc,counted_free);
    if (!chubby){
	fprintf(stderr,"chubby_new failed\n");
	exit(1);
    }
    c = chubby_makeConnection(chubby);
    if (!c){
	fprintf(stderr,"chubby_makeConnection failed\n");
	exit(1);
    }
    chubby_close(chubby);
    if (argc>1 && !strcmp(argv[1],"test1"))
	test1(argc,argv,c);
    else if (argc>1 && !strcmp(argv[1],"test2"))
	test2(argc,argv,c);
    else
	test0(argc,argv,c);
    chubby_connection_close(c);
    printf("malloc_size=%d\n",malloc_size);
    ntest++;
    if (malloc_size == 0)
	passed++;
    printf("%d/%d passed\n",passed,ntest);
    return 0;
}
