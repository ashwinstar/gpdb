
static apr_status_t a_array_init_units_init_reply(gpcc_event_t *ev, void *_cmdslot)
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
	gpcc_warning(FLINE, "[Error] init array failed: status=%s, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_init_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    gpcc_event_t *evt;
    int i;
    
    p = cmdslot_alloc("INITDB", FTS_INITDB_CMDSTATE);
    p->u.initdb.parent_cmdidx = cmdslot->cmdidx;
    p->u.initdb.ntotal = nseginfo;

    p->u.initdb.req = apr_palloc(p->pool, sizeof(*p->u.initdb.req) * p->u.initdb.ntotal);
    for (i = 0 ; i < p->u.initdb.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i];
	p->u.initdb.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.initdb.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.initdb.req[i].mode = "";
	p->u.initdb.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.initdb.req[i].port = info->port;
	p->u.initdb.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.initdb.req[i].dbid = info->dbid;
	p->u.initdb.req[i].content = info->content;
	p->u.initdb.req[i].reply = 0;
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_init_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_init.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_init.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_init_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    a_array_init_reply(ev, _cmdslot);
    gpcc_net_http_close(cmdslot->u.array_init.conn, 200, "OK", ev->pdata);
    return 0;
}

static apr_status_t a_array_init_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    

    a_array_init_reply(ev, _cmdslot);
    gpcc_net_http_close(cmdslot->u.array_init.conn, 500, "INTERNAL ERROR", ev->pdata);
    return 0;
}

