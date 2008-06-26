/* connect to HAs */
apr_status_t a_haset_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->u.haset.nopened = 0;
    slot->u.haset.nodetab = apr_pcalloc(slot->pool, sizeof(*slot->u.haset.nodetab) * slot->u.haset.total);
    
    return 0;
}

apr_status_t a_haset_invalid(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int id;

    if (sscanf(ev->pdata, "node.%d", &id) != 1 || id >= slot->u.haset.total)
	gpcc_fatal(FLINE, "invalid node name %s", ev->pdata);

    gpcc_log_info("NODESET ID=%d INVALID", id);
    slot->u.haset.nodetab[id].opened = 0;
    slot->u.haset.ninvalid++;

    if (slot->u.haset.ninvalid + slot->u.haset.nopened == gpma.node_max) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
    return 0;
}

apr_status_t a_haset_opened(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int id;

    if (sscanf(ev->pdata, "node.%d", &id) != 1 || id >= slot->u.haset.total)
	gpcc_fatal(FLINE, "invalid node name %s", ev->pdata);

    gpcc_log_info("NODESET ID=%d OPENED", id);
    slot->u.haset.nodetab[id].opened = 1;
    slot->u.haset.nopened++;
    
    if (slot->u.haset.nopened + slot->u.haset.ninvalid == gpma.node_max) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

apr_status_t a_haset_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int id;

    if (sscanf(ev->pdata, "node.%d", &id) != 1 || id >= slot->u.haset.total)
	gpcc_fatal(FLINE, "invalid node name %s", ev->pdata);

    gpcc_log_info("NODESET ID=%d CLOSED", id);
    slot->u.haset.nodetab[id].opened = 0;
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
