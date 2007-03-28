#ifndef GPCC_EVENT_H
#define GPCC_EVENT_H

#include <gpcc.h>

#define GPCC_EVENT_NULL		0
#define GPCC_EVENT_MSG		1


typedef struct gpcc_event_t gpcc_event_t;
struct gpcc_event_t {
    short reqid;		/* request id */
    short type;			/* event type */
    short cdatasz;		/* size of cdata[] */
    short reserved;		/* unused */
    void* pdata;		/* if not null, points to something the user allocated */
    void (*pfree)(void*);	/* howto free(pdata) */
    char  cdata[1];
};

/**
 *  Allocate an event. To deallocate an event returned by this function, 
 *  use gpcc_event_free().
 *  @param reqid the request id
 *  @param type the event type
 *  @param cdatasz payload size 
 *  @return a newly allocated event. Caller is responsible in filling cdata.
 */
APR_DECLARE(gpcc_event_t*) gpcc_event_alloc(int reqid, int type, int cdatasz);

/**
 *  Alternate allocate an event. To deallocate an event returned by this 
 *  function, use gpcc_event_free().
 *  @param reqid the request id
 *  @param type the event type
 *  @param pdata points to something the user allocated 
 *  @param pfree function to free pdata when event is destroyed
 *  @return a newly allocated event.
 */
APR_DECLARE(gpcc_event_t*) gpcc_event_alloc_ex(int reqid, int type, 
					       void* pdata, void (*pfree)(void*));

/**
 *  Free an event allocated with gpcc_event_alloc() and gpcc_event_alloc_ex()
 *  @param ev the event to free
 */
APR_DECLARE(void) gpcc_event_free(const gpcc_event_t* ev);

/** event handler */
typedef void gpcc_event_handler_t(const gpcc_event_t* ev);

/**
 *  Start the event handling thread.
 *  @param handler callback routine to handle an event 
 */
APR_DECLARE(void) gpcc_event_thread_start(gpcc_event_handler_t* handler);

/**
 *  Stop the event handling thread.
 */
APR_DECLARE(void) gpcc_event_thread_stop(void);

/**
 *  Tell the event thread to handle an event. 
 *  @param ev the new event. It will be queued and handled eventually.
 */
APR_DECLARE(void) gpcc_event_thread_put(const gpcc_event_t* ev);



#endif /* GPCC_EVENT_H */
