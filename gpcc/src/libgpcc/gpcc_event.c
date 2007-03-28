#include <stdlib.h>
#include <gpcc_event.h>
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

typedef struct gpcc_evqueue_t gpcc_evqueue_t;
struct gpcc_evqueue_t {
    int cnt, max, bot, top;
    const gpcc_event_t** arr;
    apr_thread_mutex_t* mutex;
    apr_thread_cond_t* not_empty;
    apr_thread_cond_t* not_full;
    apr_pool_t* pool;
};


/**
 *  Create an event queue.
 *  @param qsize capacity of queue to hold events
 *  @param pool the pool container
 *  @return a newly minted event queue
 */
APR_DECLARE(gpcc_evqueue_t*) gpcc_evqueue_create(int qsize, 
						 apr_pool_t* pool);

/**
 *  Destroy an event queue. Any residue contents of the queue is 
 *  simply discarded untouched.
 *  @param q the event queue
 */
APR_DECLARE(void) gpcc_evqueue_destroy(gpcc_evqueue_t* q);

/**
 *  Retrieve the current number of elements in the queue.
 *  @param q the event queue.
 */
APR_DECLARE(int) gpcc_evqueue_size(gpcc_evqueue_t* q);

/**
 *  Insert an event.
 *  @param q the event queue
 *  @param ev insert this event into the queue
 */
APR_DECLARE(void) gpcc_evqueue_put(gpcc_evqueue_t* q, const gpcc_event_t* ev);


/**
 *  Dequeue an event. This function is blocking when queue is empty.
 *  @param q the event queue
 *  @return an event in the queue
 */
APR_DECLARE(gpcc_event_t*) gpcc_evqueue_get(gpcc_evqueue_t* q);
APR_DECLARE(gpcc_event_t*) gpcc_evqueue_tryget(gpcc_evqueue_t* q);




APR_DECLARE(gpcc_event_t*) gpcc_event_alloc(int reqid, int type, int datasz)
{
    int len = sizeof(gpcc_event_t) + datasz - 1;
    gpcc_event_t* ret;

    GPCC_DO(datasz >= 0);
    GPCC_DO(0 != (ret = malloc(len)));
    memset(ret, 0, len);
    ret->reqid = reqid;
    ret->type = type;
    ret->cdatasz = datasz;
    return ret;
}

APR_DECLARE(gpcc_event_t*) gpcc_event_alloc_ex(int reqid, int type, 
					       void* pdata, void (*pfree)(void*))
{
    gpcc_event_t* ret = gpcc_event_alloc(reqid, type, 0);
    ret->pdata = pdata;
    ret->pfree = pfree;
    return ret;
}

APR_DECLARE(void) gpcc_event_free(const gpcc_event_t* ev)
{
    if (ev->pdata)
	ev->pfree(ev->pdata);
    free((void*)ev);
}


static struct {
    apr_pool_t* pool;
    gpcc_evqueue_t* evq;
    apr_thread_t* thread;
    gpcc_event_handler_t* handler;
} cb = { 0 };


static void* APR_THREAD_FUNC gpcc_event_thread_run(apr_thread_t* thread, void* arg)
{
    const gpcc_event_t* ev = 0;
    while (thread == cb.thread) {
	if (ev) gpcc_event_free(ev);
	ev = gpcc_evqueue_get(cb.evq);
	cb.handler(ev);
    }
    if (ev) gpcc_event_free(ev);
    return 0;
}



APR_DECLARE(void) gpcc_event_thread_start(gpcc_event_handler_t* handler)
{
    apr_threadattr_t* ta;
    if (cb.pool)
	return;

    GPCC_DO(0 == apr_pool_create(&cb.pool, 0));
    cb.handler = handler;
    GPCC_DO(0 != (cb.evq = gpcc_evqueue_create(100, cb.pool)));
    GPCC_DO(0 == apr_threadattr_create(&ta, cb.pool));
    GPCC_DO(0 == apr_threadattr_detach_set(ta, 0));
    GPCC_DO(0 == apr_thread_create(&cb.thread, ta, 
				   gpcc_event_thread_run, 0, cb.pool));
}


APR_DECLARE(void) gpcc_event_thread_stop(void)
{
    apr_thread_t* thread = cb.thread;
    apr_status_t status;

    if (thread) {
	gpcc_event_t* ev = gpcc_event_alloc(-1, GPCC_EVENT_NULL, 0);
	cb.thread = 0;
	gpcc_evqueue_put(cb.evq, ev); /* prime the pipe, so to speak */
	GPCC_DO(0 == apr_thread_join(&status, thread));
	while (0 != (ev = gpcc_evqueue_tryget(cb.evq))) 
	    gpcc_event_free(ev);
	gpcc_evqueue_destroy(cb.evq);
	cb.evq = 0;
	apr_pool_destroy(cb.pool);
	cb.pool = 0;
    }
}


APR_DECLARE(gpcc_evqueue_t*) gpcc_evqueue_create(int qsize, 
						 apr_pool_t* pool)
{
    gpcc_evqueue_t* qq = 0;
    qq = apr_pcalloc(pool, sizeof(gpcc_evqueue_t));
    qq->pool = pool;
    qq->max = qsize;
    qq->arr = apr_palloc(pool, sizeof(qq->arr[0]) * qsize);
    GPCC_DO(0 == apr_thread_mutex_create(&qq->mutex, 0, pool));
    GPCC_DO(0 == apr_thread_cond_create(&qq->not_empty, pool));
    GPCC_DO(0 == apr_thread_cond_create(&qq->not_full, pool));
    return qq;
}


APR_DECLARE(void) gpcc_evqueue_destroy(gpcc_evqueue_t* q)
{
    if (q->not_full) 
	apr_thread_cond_destroy(q->not_full);
    if (q->not_empty)
	apr_thread_cond_destroy(q->not_empty);
    q->not_full = q->not_empty = 0;

    if (q->mutex)
	apr_thread_mutex_destroy(q->mutex);
    q->mutex = 0;
}


APR_DECLARE(int) gpcc_evqueue_size(gpcc_evqueue_t* q)
{
    return q->cnt;
}


APR_DECLARE(void) gpcc_evqueue_put(gpcc_evqueue_t* q, const gpcc_event_t* ev)
{
    GPCC_DO(0 == apr_thread_mutex_lock(q->mutex));
    while (q->cnt == q->max) 
	GPCC_DO(0 == apr_thread_cond_wait(q->not_full, q->mutex));
    GPCC_DO(q->cnt < q->max);
    q->arr[q->top++] = ev;
    if (q->top == q->max) q->top = 0;
    q->cnt++;
    GPCC_DO(0 == apr_thread_mutex_unlock(q->mutex));
    GPCC_DO(0 == apr_thread_cond_broadcast(q->not_empty));
}

APR_DECLARE(gpcc_event_t*) gpcc_evqueue_get(gpcc_evqueue_t* q)
{
    gpcc_event_t* ret = 0;
    GPCC_DO(0 == apr_thread_mutex_lock(q->mutex));
    while (q->cnt == 0)
	GPCC_DO(0 == apr_thread_cond_wait(q->not_empty, q->mutex));
    ret = (gpcc_event_t*) q->arr[q->bot];
    if (++q->bot == q->max) 
	q->bot = 0;
    q->cnt--;
    GPCC_DO(0 == apr_thread_mutex_unlock(q->mutex));
    GPCC_DO(0 == apr_thread_cond_broadcast(q->not_full));

    return ret;
}


APR_DECLARE(gpcc_event_t*) gpcc_evqueue_tryget(gpcc_evqueue_t* q)
{
    gpcc_event_t* ret = 0;
    GPCC_DO(0 == apr_thread_mutex_lock(q->mutex));
    if (q->cnt) {
	ret = (gpcc_event_t*) q->arr[q->bot];
	if (++q->bot == q->max) 
	    q->bot = 0;
	q->cnt--;
    }
    GPCC_DO(0 == apr_thread_mutex_unlock(q->mutex));
    if (ret)
	GPCC_DO(0 == apr_thread_cond_broadcast(q->not_full));
    return ret;
}
