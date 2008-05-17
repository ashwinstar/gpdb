#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <chubby.h>

struct chubby_thread_struct{
    apr_thread_mutex_t*mutex;
    chubby_t*chubby;
};

void*APR_THREAD_FUNC chubby_thread_start(apr_thread_t*thread,void*voidstar);
