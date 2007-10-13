
static apr_status_t a_array_stop_units_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_warning(FLINE, "[Error] stop array failed: status=%d, reason=%s", status, reason);
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

    p->u.stop.req = apr_palloc(p->pool, sizeof(*p->u.stop.req) * p->u.stop.ntotal);
    for (i = 0 ; i < p->u.stop.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i+1];
	p->u.stop.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.stop.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.stop.req[i].mode = "immediate";
	p->u.stop.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.stop.req[i].port = info->port;
	p->u.stop.req[i].datadir = apr_pstrdup(p->pool, info->dir);
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}


static apr_status_t a_array_stop_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_stop.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_stop.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_stop_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("stop", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_stop.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_stop.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);
    return 0;
}


