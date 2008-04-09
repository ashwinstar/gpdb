
apr_status_t a_maset_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int id;

    slot->u.maset.nopened = 1;
    slot->u.maset.matab = apr_pcalloc(slot->pool, sizeof(*slot->u.maset.matab) * slot->u.maset.total);
    if (sscanf(gpma.ma_unit, "ma.%d", &id) != 1 || id >= slot->u.maset.total)
	gpcc_fatal(FLINE, "invalid ma name = %s", gpma.ma_unit);

    slot->u.maset.matab[id].opened = 1;

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
    int id;

    if (sscanf(ev->pdata, "ma.%d", &id) != 1 || id >= slot->u.maset.total)
	gpcc_fatal(FLINE, "invalid ma name = %s", ev->pdata);

    gpcc_log_info("MASET ID=%d OPENED", id);
    slot->u.maset.matab[id].opened = 1;
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
    int id;

    if (sscanf(ev->pdata, "ma.%d", &id) != 1 || id >= slot->u.maset.total)
	gpcc_fatal(FLINE, "invalid ma name = %s", ev->pdata);

    gpcc_log_info("MASET ID=%d CLOSED", id);
    slot->u.maset.matab[id].opened = 0;
    slot->u.maset.nopened--;
    
    return 0;
}

apr_status_t a_maset_ready_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    char *ma = ev->pdata;
    int id;

    if (sscanf(ev->pdata, "ma.%d", &id) != 1 || id >= slot->u.maset.total)
	gpcc_fatal(FLINE, "invalid ma name = %s", ev->pdata);

    gpcc_log_info("MASET ID=%d CLOSED", id);
    slot->u.maset.matab[id].opened = 0;
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
