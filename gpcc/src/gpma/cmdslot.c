#include "gpma.h"
#include <apr_pools.h>
#include <apr_strings.h>


static apr_uint16_t unique = 0;
static cmdslot_t* slottab = 0;

void cmdslot_init(apr_pool_t* pool)
{
    if (slottab) 
	gpcc_fatal(FLINE, "already initialized");
    slottab = apr_palloc(pool, sizeof(*slottab) * CMDSLOT_TABLE_MAX);
    memset(slottab, 0, sizeof(*slottab) * CMDSLOT_TABLE_MAX);
}


cmdslot_t* cmdslot_alloc(const char* name, cmdstate_t state)
{
    int i;
    cmdslot_t* q = &slottab[CMDSLOT_TABLE_MAX];
    cmdslot_t* p = &slottab[unique % CMDSLOT_TABLE_MAX];

    for (i = 0; i < CMDSLOT_TABLE_MAX; i++, p++) {
	if (p == q) 
	    p = slottab;
	if (p->cmdidx == 0) {
	    apr_uint32_t idx = unique++;
	    if (idx == 0)
		idx = unique++;
	    idx = (idx << 16) | (p - slottab);
	    p->cmdidx = idx;
	    p->cmdstate = state;
	    p->countdown = 0;
	    memset(&p->u, 0, sizeof(p->u));
	    if (0 != apr_pool_create(&p->pool, 0)) 
		gpcc_fatal(FLINE, "out of memory");
	    p->cmdname = apr_pstrdup(p->pool, name);
	    return p;
	}
    }
    return 0;
}


cmdslot_t* cmdslot_find(apr_uint32_t cmdidx)
{
    int idx = cmdidx & 0xffff;
    if (0 <= idx && idx < CMDSLOT_TABLE_MAX) {
	cmdslot_t* p = &slottab[idx];
	if (p->cmdidx == cmdidx)
	    return p;
    }
    return 0;
}

void cmdslot_free(apr_uint32_t cmdidx)
{
    cmdslot_t* p = cmdslot_find(cmdidx);
    if (p) {
	if (p->pool) {
	    apr_pool_destroy(p->pool);
	}
	p->cmdidx = 0;
    }
}



void cmdslot_tick(void)
{
    int i;
    for (i = 0; i < CMDSLOT_TABLE_MAX; i++) {
	cmdslot_t* p = &slottab[i];
	if (p->cmdidx && p->countdown > 0) {
	    if (0 == (--p->countdown)) {
		gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, p->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, ev);
	    }
	}
    }
}


APR_DECLARE(void) cmdslot_broadcast_fts_net_closed(const char *node)
{
    int i;

    for (i = 0 ; i < CMDSLOT_TABLE_MAX ; i++) {
	cmdslot_t *p = &slottab[i];
	if (p->cmdidx && strstr(p->cmdname, "fts")) {
	    gpcc_event_t *ev = gpcc_event_alloc(NET_CLOSED_EVENT, p->cmdidx, strlen(node)+1);
	    memcpy(ev->pdata, node, strlen(node)+1);
	    gpcc_evqueue_put(gpma.evq, ev);
	}
    }
}
