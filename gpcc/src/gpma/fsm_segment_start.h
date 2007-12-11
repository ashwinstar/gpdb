static apr_status_t a_segment_start_do(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int i, j;
    cmdslot_t *p;
    gpcc_event_t *evt;

    /* get gp_configuration */

    
    /* reset valid for the segments */
    if (apr_strnatcasecmp(slot->u.segment_start.mode, "force") == 0) {
	
    }

    /* kick off start */
    p = cmdslot_alloc("START", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = slot->cmdidx;
    p->u.start.ntotal = slot->u.segment_start.nunit;

    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req) * p->u.start.ntotal);

    for (i = 0 ; i < slot->u.segment_start.nunit ; i++) {
	const seginfo_t *info;

	for (j = 0 ; j < gpma.fts_cmdslot->u.fts.nseginfo &&
		 strcmp(gpma.fts_cmdslot->u.fts.seginfo[j].name, slot->u.segment_start.unit[i]) != 0 ; j++) ;
	if (j >= gpma.fts_cmdslot->u.fts.nseginfo) {
	    continue;
	}
	
	info = &gpma.fts_cmdslot->u.fts.seginfo[j];

	p->u.start.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.start.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.start.req[i].mode = apr_pstrdup(p->pool, slot->u.segment_start.mode);
	p->u.start.req[i].host = apr_pstrdup(p->pool, info->host);
    	p->u.start.req[i].port = info->port;
	p->u.start.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.start.req[i].reply = 0;

    }
	
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    


    return 0;
}

static apr_status_t a_segment_start_units_start_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *cmdslot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", "failure");

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);	
	gpcc_warning(FLINE, "[Error] start segment failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);


    return 0;
}


static apr_status_t a_segment_start_success(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->u.segment_start.parent_cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_segment_start_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->u.segment_start.parent_cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}
