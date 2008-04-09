
static apr_status_t a_array_delete_units_delete_reply(gpcc_event_t *ev, void *_cmdslot)
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
	gpcc_warning(FLINE, "[Error] delete array failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_delete_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    gpcc_event_t *evt;
    int i;
    
    p = cmdslot_alloc("DELETE", FTS_DELETE_CMDSTATE);
    p->u.delete.parent_cmdidx = cmdslot->cmdidx;
    p->u.delete.ntotal = nseginfo;
    
    gpcc_log_info("a_array_delete_do total = %d", p->u.delete.ntotal);

    p->u.delete.req = apr_palloc(p->pool, sizeof(*p->u.delete.req) * p->u.delete.ntotal);
    for (i = 0 ; i < p->u.delete.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i];

	p->u.delete.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.delete.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.delete.req[i].mode = apr_pstrdup(p->pool, cmdslot->u.array_delete.mode);
	p->u.delete.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.delete.req[i].port = info->port;
	p->u.delete.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.delete.req[i].reply = 0;
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_delete_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_delete.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_delete.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_delete_success(gpcc_event_t *ev, void *_cmdslot)
{
    /* cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */

    a_array_delete_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_delete.conn, 200, "OK", ev->pdata); */
    return 0;
}

static apr_status_t a_array_delete_failure(gpcc_event_t *ev, void *_cmdslot)
{
    /*  cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    */

    a_array_delete_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_delete.conn, 200, "failure", ev->pdata); */
    return 0;
}


