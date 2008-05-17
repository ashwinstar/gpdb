#include "chubby_thread.h"

void*APR_THREAD_FUNC
chubby_thread_start(apr_thread_t*thread,void*voidstar)
{
    struct chubby_thread_struct*cts=(struct chubby_thread_struct*)voidstar;
    for (;;){
	fd_set rset;
	int fd;
	struct timeval tv;
	apr_thread_mutex_lock(cts->mutex);
	chubby_asynchronous_think(cts->chubby,&fd,&tv);
	apr_thread_mutex_unlock(cts->mutex);
	FD_ZERO(&rset);
	FD_SET(fd,&rset);
	select(fd+1,&rset,NULL,NULL,&tv);
    }
}
