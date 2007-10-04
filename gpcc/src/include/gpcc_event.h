#ifndef GPCC_EVENT_H
#define GPCC_EVENT_H

#include <gpcc.h>


enum gpcc_event_type_t {
    GPCC_EVENT_NULL = 0,
    GPCC_EVENT_MSG = 1,
};
typedef enum gpcc_event_type_t gpcc_event_type_t;


typedef struct gpcc_event_t gpcc_event_t;
struct gpcc_event_t {
    gpcc_event_type_t etype;	/* event type */
    int               edatasz;	/* size of content data (pdata[]) */
    apr_uint32_t      cmdidx;	/* command index */
    const char*       pdata;	/* point to the data */
    void              (*pfree)(void*); /* internal use */
};

/**
 *  Allocate an event. To deallocate an event returned by this function, 
 *  use gpcc_event_free().
 *  @param type the event type
 *  @param cmdidx the command index
 *  @param cdatasz payload size 
 *  @return a newly allocated event. Caller is responsible in filling cdata.
 */
APR_DECLARE(gpcc_event_t*) gpcc_event_alloc(int type, apr_uint32_t cmdidx, int cdatasz);

/**
 *  Alternate allocate an event. To deallocate an event returned by this 
 *  function, use gpcc_event_free().
 *  @param type the event type
 *  @param cmdidx the command index
 *  @param pdata points to something the user allocated 
 *  @param datasz #bytes in pdata
 *  @param pfree function to free pdata when event is destroyed
 *  @return a newly allocated event.
 */
APR_DECLARE(gpcc_event_t*) gpcc_event_alloc_ex(int type, apr_uint32_t cmdidx, 
					       void* pdata, int datasz, void (*pfree)(void*));

/**
 *  Free an event allocated with gpcc_event_alloc() and gpcc_event_alloc_ex()
 *  @param ev the event to free
 */
APR_DECLARE(void) gpcc_event_free(const gpcc_event_t* ev);


/** Event queue */
typedef struct gpcc_evqueue_t gpcc_evqueue_t;

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

/**
 *  Try to dequeue an event. 
 *  @param q the event queue
 *  @return an event in the queue or NULL if queue is empty.
 */
APR_DECLARE(gpcc_event_t*) gpcc_evqueue_tryget(gpcc_evqueue_t* q);


#endif /* GPCC_EVENT_H */
