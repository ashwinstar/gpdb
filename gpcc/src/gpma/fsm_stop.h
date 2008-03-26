static apr_status_t a_stop_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i;
    struct json_object *obj;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    gpcc_log_info("-------------------- segments stop -----------------------");
    gpcc_log_info("total number of segments stop = %d", cmdslot->u.stop.ntotal);
    for (i = 0 ; i < cmdslot->u.stop.ntotal ; i++) {
	char *msg;
	int len = 0;
	int j = 0;

	for (j = 0 ; j < gpma.unit_max ; j++) {
	    if (strcmp(cmdslot->u.stop.req[i].unit, gpma.unit_cmdtab[j]->cmdname) == 0)
		break;
	}
	if (j >= gpma.unit_max) {
	    gpcc_warning(FLINE, "[Error] invalid segment name %s", cmdslot->u.stop.req[i].unit);
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    return 0;
	}

	obj = gpcc_json_segment_request(0,
					cmdslot->u.stop.req[i].unit,
					"stop",
					cmdslot->u.stop.req[i].mode,
					cmdslot->u.stop.req[i].host,
					cmdslot->u.stop.req[i].port,
					cmdslot->u.stop.req[i].datadir);
	msg = json_object_to_json_string(obj);
	len = strlen(msg) + 1;

	gpcc_log_info("sending stop command to %s at %s", 
		      cmdslot->u.stop.req[i].unit,
		      cmdslot->u.stop.req[i].host);

	evt = gpcc_event_alloc(UNIT_STOP_EVENT, gpma.unit_cmdtab[j]->cmdidx, sizeof(*pkt) + len);
	pkt = (gpcc_net_packet_t *) evt->pdata;
	pkt->mh.sndidx = cmdslot->cmdidx;
	pkt->mh.rcvidx = cmdslot->u.stop.parent_cmdidx;
	pkt->mh.len = len;
	strcpy(pkt->payload, msg);
	gpcc_evqueue_put(gpma.evq, evt);
    
	json_object_put(obj);
    }

    cmdslot->countdown = 2 * 60;
    return 0;
}


static apr_status_t a_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt = 0;
    char *seg, *status, *reason;
    int i;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");
    
    for (i = 0 ; i < cmdslot->u.stop.ntotal ; i++) {
	if (strcmp(seg, cmdslot->u.stop.req[i].unit) == 0) 
	    break;
    }
    if (i >= cmdslot->u.stop.ntotal){
	gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	json_object_put(obj);
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    cmdslot->u.stop.req[i].reply = apr_pstrdup(cmdslot->pool, pkt->payload);
    
    if (strcmp(status, "ok") == 0) {
	gpcc_log_info("segment %s stop reply ok", seg);
	cmdslot->u.stop.nsuccess++;
    } else {
	cmdslot->u.stop.nfailed++;
	gpcc_warning(FLINE, "[Error] segment %s stop reply failed. [status=%s, reason=%s]", seg, status, reason);
    }

    if (cmdslot->u.stop.nsuccess + cmdslot->u.stop.nfailed == cmdslot->u.stop.ntotal) {
	if (cmdslot->u.stop.nfailed == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
	}
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->countdown = 0;
    }
    
    if (obj) json_object_put(obj);
    return 0;
}

static void a_stop_send_json_reply(gpcc_event_t *ev, cmdslot_t *cmdslot, struct json_object *obj)
{
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    char *objstr;

    objstr = json_object_to_json_string(obj);    
    evt = gpcc_event_alloc(UNITS_REPLY_MSG_EVENT, 
			   cmdslot->u.stop.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = UNITS_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.stop.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

}

static struct json_object * mkjson_stop_reply(gpcc_event_t *ev, void *_cmdslot, 
					      const char *action, const char *status, const char *reason)
{
    cmdslot_t *cmdslot = (cmdslot_t*) _cmdslot;
    struct json_object *obj, *rsetobj, *arrobj;
    int i, n;

    obj = gpcc_json_http_reply(action, status, reason);
    rsetobj = json_object_new_object();
    arrobj = json_object_new_array();
    for (i = 0, n = 0 ; i < cmdslot->u.stop.ntotal ; i++) {
	if (cmdslot->u.stop.req[i].reply) {
	    json_object_array_add(arrobj, json_object_new_string(cmdslot->u.stop.req[i].reply));
	    n++;
	}
    }
    json_object_object_add(rsetobj, "total", json_object_new_int(n));
    json_object_object_add(rsetobj, "result", arrobj);
    json_object_object_add(obj, "rset", rsetobj);

    return obj;
}


static apr_status_t a_stop_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    gpcc_log_info("segments stop finished successfully");
    gpcc_log_info("----------------------------------------------------------");
    obj = mkjson_stop_reply(ev, _cmdslot, "stop", "ok", 0);
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}


static apr_status_t a_stop_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    gpcc_log_info("segments stop failed");
    gpcc_log_info("----------------------------------------------------------");
    obj = mkjson_stop_reply(ev, _cmdslot, "stop", "failure", 0);
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_stop_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    gpcc_log_info("segments stop timed out");
    gpcc_log_info("----------------------------------------------------------");
    obj = mkjson_stop_reply(ev, _cmdslot, "stop", "failure", "timeout");
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}
