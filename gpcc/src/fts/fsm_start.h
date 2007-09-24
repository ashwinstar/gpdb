

static apr_status_t start_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    apr_pool_create(&cmdslot->u.sys.request.pool, 0);
    memset(&cmdslot->u.sys.request.u, 0, sizeof(cmdslot->u.sys.request.u));
    cmdslot->u.sys.request.u.start.replied = apr_pcalloc(cmdslot->u.sys.request.pool, sizeof(int) * cmdslot->u.sys.nseginfo);

    return 0;
}

static apr_status_t start_utility_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e, haidx;
    hainfo_t *hainfo;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.sys.seginfo[0];    
    haidx = seginfo->haidx;

    if (!cmdslot->u.sys.hainfo || !cmdslot->u.sys.hainfo[haidx]) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return APR_EINVAL;
    }

    obj = gpcc_json_segment_request(0, seginfo->name, "start", "utility", seginfo->host, seginfo->port, seginfo->dir);    
    hainfo = cmdslot->u.sys.hainfo[haidx];

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))){
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	gpcc_warning(FLINE, "gpcc_ha_send failed");
    }

    json_object_put(obj);
    return 0;
}

static apr_status_t start_utility_qd_reply_segment_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->edata;
    obj = json_tokener_parse(pkt->mb);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start utility qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_warning(FLINE, "[Error] start utility qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
    }
    
    gpcc_evqueue_put(gpfts.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t start_utility_qd_reply_net_closed(gpcc_event_t *ev, void *_cmdslot)
{

    
    return 0;
}

static apr_status_t start_utility_qd_reply(gpcc_event_t *ev, void *_cmdslot)
{
    switch (ev->etype) {
    case SEGMENT_REPLY_MSG_EVENT:
	return start_utility_qd_reply_segment_reply(ev, _cmdslot);
    case NET_CLOSED_EVENT:
	return start_utility_qd_reply_net_closed(ev, _cmdslot);
    default:
	break;
    }
    return 0;
}


static apr_status_t start_gpconf(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo;
    int e;
    gpcc_event_t *evt;

    seginfo = &cmdslot->u.sys.seginfo[0];
    if (0 != (e = gpcc_pg_reset_invalid_segment(seginfo->host, seginfo->port, cmdslot->u.sys.request.pool))) {
	gpcc_warning(FLINE, "gpcc_pg_reset_invalid_segment failed");
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return 0;
    }
    
    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpfts.evq, evt);    
    return 0;
}

static apr_status_t start_utility_qd_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e, haidx;
    hainfo_t *hainfo;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.sys.seginfo[0];    
    haidx = seginfo->haidx;

    if (!cmdslot->u.sys.hainfo || !cmdslot->u.sys.hainfo[haidx]) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return APR_EINVAL;
    }

    obj = gpcc_json_segment_request(0, seginfo->name, "stop", "immediate", seginfo->host, seginfo->port, seginfo->dir);    
    hainfo = cmdslot->u.sys.hainfo[haidx];

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))){
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	gpcc_warning(FLINE, "gpcc_ha_send failed");
    }

    json_object_put(obj);
    return 0;
}


static apr_status_t start_gpconf_network_closed(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}

static apr_status_t start_utility_qd_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->edata;
    obj = json_tokener_parse(pkt->mb);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start utility qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_warning(FLINE, "[Error] stop utility qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
    }
    
    gpcc_evqueue_put(gpfts.evq, evt);
    json_object_put(obj);
    return 0;
}


static apr_status_t start_qes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, e;
    int haidx;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    struct json_object *obj;
    hainfo_t *hainfo;
    char *msg;

    if (!cmdslot->u.sys.hainfo) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return APR_EINVAL;
    }

    for (i = 0 ; i < cmdslot->u.sys.nhainfo ; i++) {
	if (!cmdslot->u.sys.hainfo[i]) {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, evt);
	    return APR_EINVAL;
	}
    }

    memset(&cmdslot->u.sys.request.u, 0, sizeof(cmdslot->u.sys.request.u));

    for (i = 1; i < cmdslot->u.sys.nseginfo ; i++) {
	seginfo = &cmdslot->u.sys.seginfo[i];
	if (strstr(seginfo->name, "gpdb.seg.m") != 0)
	    break;

	haidx = seginfo->haidx;
	obj = gpcc_json_segment_request(0, seginfo->name, "start", "normal", seginfo->host, seginfo->port, seginfo->dir);
	hainfo = cmdslot->u.sys.hainfo[haidx];

	msg = json_object_to_json_string(obj);
	if (0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))){
	    gpcc_warning(FLINE, "gpcc_ha_send failed");
	    cmdslot->u.sys.request.u.start.nreply++;
	    cmdslot->u.sys.request.u.start.nfailed++;
	}
	
	json_object_put(obj);

	cmdslot->u.sys.request.u.start.nrequest++;	
    }
    
    
    
    return 0;
}

static apr_status_t start_qe_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt = 0;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->edata;
    obj = json_tokener_parse(pkt->mb);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    cmdslot->u.sys.request.u.start.nreply++;
    if (strcmp(status, "ok") == 0) {
	cmdslot->u.sys.request.u.start.nsuccess++;
    } else {
	cmdslot->u.sys.request.u.start.nfailed++;
	gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, status=%s, reason=%s", seg, status, reason);
    }

    if (cmdslot->u.sys.request.u.start.nreply == cmdslot->u.sys.request.u.start.nrequest) {
	if (cmdslot->u.sys.request.u.start.nfailed == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);	
	}
	gpcc_evqueue_put(gpfts.evq, evt);
    }

    json_object_put(obj);
    return 0;
}

static apr_status_t start_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e, haidx;
    hainfo_t *hainfo;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.sys.seginfo[0];    
    haidx = seginfo->haidx;

    if (!cmdslot->u.sys.hainfo || !cmdslot->u.sys.hainfo[haidx]) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return APR_EINVAL;
    }

    obj = gpcc_json_segment_request(0, seginfo->name, "start", "normal", seginfo->host, seginfo->port, seginfo->dir);    
    hainfo = cmdslot->u.sys.hainfo[haidx];

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))){
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	gpcc_warning(FLINE, "gpcc_ha_send failed");
    }

    json_object_put(obj);
    return 0;
}

static apr_status_t start_qd_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->edata;
    obj = json_tokener_parse(pkt->mb);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start production qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_warning(FLINE, "[Error] start production qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
    }
    
    gpcc_evqueue_put(gpfts.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t start_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int idx, e, haidx;
    hainfo_t *hainfo;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;
    
    idx = cmdslot->u.sys.nseginfo-1;
    if (strstr(cmdslot->u.sys.seginfo[idx].name, "gpdb.seg.m.") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return 0;
    }
	    
    seginfo = &cmdslot->u.sys.seginfo[idx];    
    haidx = seginfo->haidx;

    if (!cmdslot->u.sys.hainfo || !cmdslot->u.sys.hainfo[haidx]) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	return APR_EINVAL;
    }

    obj = gpcc_json_segment_request(0, seginfo->name, "start", "normal", seginfo->host, seginfo->port, seginfo->dir);    
    hainfo = cmdslot->u.sys.hainfo[haidx];

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))){
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, evt);
	gpcc_warning(FLINE, "gpcc_ha_send failed");
    }

    json_object_put(obj);
    return 0;
}

static apr_status_t start_standby_qd_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->edata;
    obj = json_tokener_parse(pkt->mb);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.1") != 0) {
	gpcc_warning(FLINE, "start standby qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_warning(FLINE, "[Error] start standby qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
    }
    
    gpcc_evqueue_put(gpfts.evq, evt);
    json_object_put(obj);
    return 0;
}



static apr_status_t start_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("start", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t start_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("start", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t start_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.sys.request.pool) {
	apr_pool_destroy(cmdslot->u.sys.request.pool);
	cmdslot->u.sys.request.pool = 0;
    }

    return 0;
}

