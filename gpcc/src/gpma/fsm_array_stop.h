
static apr_status_t a_array_stop_units_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
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
	gpcc_warning(FLINE, "[Error] stop array failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_stop_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    gpcc_event_t *evt;
    int i;
    
    p = cmdslot_alloc("STOP", FTS_STOP_CMDSTATE);
    p->u.stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.stop.ntotal = nseginfo;
    
    gpcc_log_info("a_array_stop_do total = %d", p->u.stop.ntotal);

    p->u.stop.req = apr_palloc(p->pool, sizeof(*p->u.stop.req) * p->u.stop.ntotal);
    for (i = 0 ; i < p->u.stop.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i];

	p->u.stop.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.stop.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.stop.req[i].mode = "immediate";
	p->u.stop.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.stop.req[i].port = info->port;
	p->u.stop.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.stop.req[i].reply = 0;
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_stop.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_stop.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_stop_success(gpcc_event_t *ev, void *_cmdslot)
{
    /* cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */

    a_array_stop_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_stop.conn, 200, "OK", ev->pdata); */
    return 0;
}

static apr_status_t a_array_stop_failure(gpcc_event_t *ev, void *_cmdslot)
{
    /*  cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    */

    a_array_stop_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_stop.conn, 200, "failure", ev->pdata); */
    return 0;
}


