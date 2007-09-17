static void u_queue_timeup_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpfts.evq, ev);
}

static apr_status_t timer_init(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t timer_tick(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t* p = &gpfts.cmdslot[0];
    cmdslot_t* q = &gpfts.cmdslot[CMDSLOT_MAX];

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0)
	    if (--p->countdown == 0) 
		u_queue_timeup_event(p);
    }
    return 0;
}

static apr_status_t ha_connect_all(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}


static apr_status_t ha_opened(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}

static apr_status_t ha_closed(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}


static apr_status_t setup_ready(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t setup_failure(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t setup_ssh_start(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}


static apr_status_t setup_ssh_check(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

