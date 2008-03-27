/* chubbytest.c */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chubby.h"

#define STRING_LENGTH	1000

#define TEST(aa,bb)\
    do{\
	answer=0;\
	list=0;\
	stat.decreeNumber = 0;\
	stat.size = 0;\
	ntest++;\
	printf("%s\n",#aa);\
	error = aa;\
	*catlist = 0;\
	if (list){\
	    char**b;\
	    strcpy(catlist,",");\
	    for (b=list; *b; b++){\
		strcat(catlist,*b);\
		strcat(catlist,",");\
	    }\
	}\
	printf("\t");\
	if (bb)\
	    passed++;\
	else\
	    printf("BAD ");\
	printf("%d",error);\
	if (stat.decreeNumber || stat.size)\
	    printf(" %d %d",(int)stat.decreeNumber,stat.size);\
	if (answer){\
	    printf(" %s",answer->a);\
	    counted_free(answer);\
	}\
	if (list){\
	    printf(" %s",catlist);\
	    chubby_connection_listFree(c,list);\
	}\
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
test1(chubby_connection_t*c)
{
}

static void
test2(chubby_connection_t*c)
{
}

static void
test0(chubby_connection_t*c)
{
    struct string s;
    int error;
    chubby_string_t*answer;
    char**list;
    char catlist[STRING_LENGTH];
    struct chubby_stat stat;
    int64_t dn;
    TEST(chubby_connection_put(c,"/test/file",make_string(&s,"contents")),error==0);
    TEST(chubby_connection_get(c,&stat,&answer,"/test/file"),error==0&&!strcmp(answer->a,"contents"));
    TEST(chubby_connection_list(c,&list,"/"),error==0&&!strcmp(catlist,",paxos,test,"));
    TEST(chubby_connection_list(c,&list,"noentfile"),error==1);
    TEST(chubby_connection_stat(c,&stat,"test/file"),error==0);
    TEST(chubby_connection_remove(c,"//test//1/1"),error==0||error==CHUBBY_E_NOENT);
    TEST(chubby_connection_put(c,"test///1/1",make_string(&s,"")),error==0);
    TEST(chubby_connection_stat(c,&stat,"test/1//1"),error==0);
    dn = stat.decreeNumber;
    TEST(chubby_connection_stat(c,&stat,"test/1"),error==0&&stat.decreeNumber==dn);
    TEST(chubby_connection_stat(c,&stat,"test"),error==0&&stat.decreeNumber==dn);
    TEST(chubby_connection_remove(c,"//test//1/1"),error==0);
    TEST(chubby_connection_stat(c,&stat,"test"),error==0&&stat.decreeNumber>dn);
    TEST(chubby_connection_list(c,&list,"/test"),error==0&&!strstr(catlist,",1,"));
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
	test1(c);
    else if (argc>1 && !strcmp(argv[1],"test2"))
	test2(c);
    else
	test0(c);
    chubby_connection_close(c);
    printf("malloc_size=%d\n",malloc_size);
    ntest++;
    if (malloc_size == 0)
	passed++;
    printf("%d/%d passed\n",passed,ntest);
    return 0;
}
