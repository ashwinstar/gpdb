static apr_status_t a_stop_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i, e;
    struct json_object *obj;
    gpcc_event_t *evt;

    gpcc_log_info("a_stop_do...total = %d", cmdslot->u.stop.ntotal);
    for (i = 0 ; i < cmdslot->u.stop.ntotal ; i++) {
	char *msg;
	int len = 0;

	gpcc_log_info("node=%s, mode = %s, host = %s, port =%d, dir=%s",
		      cmdslot->u.stop.req[i].node,
		      cmdslot->u.stop.req[i].mode,
		      cmdslot->u.stop.req[i].host,
		      cmdslot->u.stop.req[i].port,
		      cmdslot->u.stop.req[i].datadir);		      
	obj = gpcc_json_segment_request(0,
					cmdslot->u.stop.req[i].unit,
					"stop",
					cmdslot->u.stop.req[i].mode,
					cmdslot->u.stop.req[i].host,
					cmdslot->u.stop.req[i].port,
					cmdslot->u.stop.req[i].datadir);
	msg = json_object_to_json_string(obj);
	len = strlen(msg) + 1;

	gpcc_log_info("send packetx %d bytes: %s", len, msg);
	if (0 != (e = gpcc_net_send_packetx(cmdslot->u.stop.req[i].node,
					    SEGMENT_REQUEST_MSG,
					    cmdslot->cmdidx,
					    0,
					    msg, len))) {
	    gpcc_warning(FLINE, "gpcc_net_send_packetx %d bytes to host %s failed",
			 len, cmdslot->u.stop.req[i].node);
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    json_object_put(obj);
	    return 0;
	}
	json_object_put(obj);

	gpcc_log_info("send packetx end");
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
    
    cmdslot->u.stop.req[i].reply = apr_palloc(cmdslot->pool, ev->edatasz);
    memcpy(cmdslot->u.stop.req[i].reply, ev->pdata, ev->edatasz);
    
    if (strcmp(status, "ok") == 0) {
	cmdslot->u.stop.nsuccess++;
    } else {
	cmdslot->u.stop.nfailed++;
	gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, status=%s, reason=%s", seg, status, reason);
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


static apr_status_t a_stop_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}


static apr_status_t a_stop_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("stop", "failed", 0);
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_stop_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("stop", "timeout", 0);
    a_stop_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}
