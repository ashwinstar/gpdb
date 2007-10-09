
static void u_queue_timeup_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpha.evq, ev);
}


/* 
 *  This gets called every second. go thru all cmdslot to find anyone that 
 *  needs a TIMEUP EVENT.
 */
static apr_status_t a_system_tick(gpcc_event_t* ev, void* cmdslot_)
{
    cmdslot_t* p = &gpha.cmdslot[0];
    cmdslot_t* q = &gpha.cmdslot[CMDSLOT_MAX];

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0)
	    if (--p->countdown == 0) 
		u_queue_timeup_event(p);
    }
    return 0;
}
