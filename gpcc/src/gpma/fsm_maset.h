
apr_status_t a_maset_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    
    slot->u.maset.nopened = 1;

    if (slot->u.maset.total == 1) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

apr_status_t a_maset_opened(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.maset.nopened++;
    gpcc_log_info("a_maset_opened START %d %d", slot->u.maset.nopened, slot->u.maset.total);
    if (slot->u.maset.nopened == slot->u.maset.total) {
	gpcc_log_info("a_maset_opened....%d", slot->u.maset.total);
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

apr_status_t a_maset_wait_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->u.maset.nopened--;
    
    return 0;
}

apr_status_t a_maset_ready_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    char *ma = ev->pdata;

    slot->u.maset.nopened--;
    
    if (slot->u.maset.nopened == 1) {
	if (*gpma.lma_cmdslot->u.lma.leadma && strcmp(gpma.lma_cmdslot->u.lma.leadma, ma) == 0) {
	    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	}
    } else {

	if (*gpma.lma_cmdslot->u.lma.leadma && strcmp(gpma.lma_cmdslot->u.lma.leadma, ma) == 0) {
	    gpcc_log_info("LEADME %s is dead", ma);
	    *gpma.lma_cmdslot->u.lma.leadma = 0;
	    evt = gpcc_event_alloc(BREAK_EVENT, gpma.lma_cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	}
    }
    return 0;
}


apr_status_t a_maset_ready(gpcc_event_t *ev, void *arg)
{
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, gpma.lma_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

apr_status_t a_maset_failure(gpcc_event_t *ev, void *arg)
{
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(BREAK_EVENT, gpma.lma_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}
