static apr_status_t a_segment_stop_do(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int i, j;
    cmdslot_t *p;
    gpcc_event_t *evt;

    p = cmdslot_alloc("STOP", FTS_STOP_CMDSTATE);
    p->u.stop.parent_cmdidx = slot->cmdidx;
    p->u.stop.ntotal = slot->u.segment_stop.nunit;

    p->u.stop.req = apr_palloc(p->pool, sizeof(*p->u.stop.req) * p->u.stop.ntotal);

    for (i = 0 ; i < slot->u.segment_stop.nunit ; i++) {
	const seginfo_t *info;

	for (j = 0 ; j < gpma.fts_cmdslot->u.fts.nseginfo &&
		 strcmp(gpma.fts_cmdslot->u.fts.seginfo[j].name, slot->u.segment_stop.unit[i]) != 0 ; j++) ;
	if (j >= gpma.fts_cmdslot->u.fts.nseginfo) {
	    continue;
	}
	
	info = &gpma.fts_cmdslot->u.fts.seginfo[j];

	p->u.stop.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.stop.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.stop.req[i].mode = apr_pstrdup(p->pool, slot->u.segment_stop.mode);
	p->u.stop.req[i].host = apr_pstrdup(p->pool, info->host);
    	p->u.stop.req[i].port = info->port;
	p->u.stop.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.stop.req[i].reply = 0;

    }
	
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_segment_stop_units_stop_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *cmdslot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);

    if (strcmp(status, OK_STATUS) == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);	
	gpcc_warning(FLINE, "[Error] stop segment failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);

    return 0;
}


static apr_status_t a_segment_stop_success(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->u.segment_stop.parent_cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_segment_stop_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->u.segment_stop.parent_cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}
