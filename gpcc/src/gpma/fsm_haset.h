/* connect to HAs */
apr_status_t a_haset_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->u.haset.nopened = 0;
    
    return 0;
}


apr_status_t a_haset_opened(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.haset.nopened++;
    
    if (slot->u.haset.nopened == gpma.node_max) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

apr_status_t a_haset_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.haset.nopened--;
    if (slot->u.haset.nopened == 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

apr_status_t a_haset_ready(gpcc_event_t *ev, void *arg)
{
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, gpma.fts_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

apr_status_t a_haset_failure(gpcc_event_t *ev, void *arg)
{
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(BREAK_EVENT, gpma.fts_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    
    return 0;
}
