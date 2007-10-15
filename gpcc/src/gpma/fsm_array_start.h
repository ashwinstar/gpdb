

static apr_status_t a_array_start_units_start_reply(gpcc_event_t *ev, void *_cmdslot)
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
	gpcc_warning(FLINE, "[Error] start utility qd failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_start_units_stop_reply(gpcc_event_t *ev, void *_cmdslot)
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
	gpcc_warning(FLINE, "[Error] start utility qd failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_start_utility_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("START_UTILITY_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "utility";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_array_start_gpconf(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo;
    int e;
    gpcc_event_t *evt;

    seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    if (0 != (e = gpcc_pg_reset_invalid_segment(seginfo->host, seginfo->port, cmdslot->pool))) {
	gpcc_warning(FLINE, "gpcc_pg_reset_invalid_segment failed");
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}

static apr_status_t a_array_start_utility_qd_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("STOP_UTILITY_QD", FTS_STOP_CMDSTATE);
    p->u.stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.stop.ntotal = 1;
    p->u.stop.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.stop.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.stop.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.stop.req[0].mode = "immediate";
    p->u.stop.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.stop.req[0].port = seginfo->port;
    p->u.stop.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.stop.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}


static apr_status_t a_array_start_qes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    gpcc_event_t *evt;
    int i;
    
    p = cmdslot_alloc("START_QES", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = nseginfo-1;

    if (strcmp(seginfo[nseginfo-1].name, "gpdb.seg.m.1") == 0) {
	p->u.start.ntotal--;
    }

    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req) * p->u.start.ntotal);
    for (i = 0 ; i < p->u.start.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i+1];
	p->u.start.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.start.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.start.req[i].mode = "normal";
	p->u.start.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.start.req[i].port = info->port;
	p->u.start.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.start.req[i].reply = 0;
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_start_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("START_PRODUCTION_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "normal";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_array_start_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    seginfo_t *seginfo = &gpma.fts_cmdslot->u.fts.seginfo[nseginfo-1];
    gpcc_event_t *evt;

    if (strstr(seginfo->name, "gpdb.seg.m.") == 0) {
	struct json_object *obj;
	char *msg = 0;
	obj = gpcc_json_http_reply("start", "ok", 0);
	msg = json_object_to_json_string(obj);
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, strlen(msg)+1);
	memcpy(evt->pdata, msg, strlen(msg)+1);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    p = cmdslot_alloc("START_STANDBY_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "normal";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}


static apr_status_t a_array_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_start.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_start.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_start_success(gpcc_event_t *ev, void *_cmdslot)
{
    /*    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */
 
    a_array_start_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_start.conn, 200, "OK", ev->pdata); */
    return 0;
}

static apr_status_t a_array_start_failure(gpcc_event_t *ev, void *_cmdslot)
{
    /*    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */

    a_array_start_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_start.conn, 200, "failure", ev->pdata); */
    return 0;
}

static apr_status_t a_array_start_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    return 0;
}

