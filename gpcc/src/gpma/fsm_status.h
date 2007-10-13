static apr_status_t a_status_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i, e;
    struct json_object *obj;
    gpcc_event_t *evt;

    for (i = 0 ; i < cmdslot->u.status.ntotal ; i++) {
	char *msg;
	int len = 0;
	obj = gpcc_json_segment_request(0,
					cmdslot->u.status.req[i].node,
					"status",
					cmdslot->u.status.req[i].mode,
					cmdslot->u.status.req[i].host,
					cmdslot->u.status.req[i].port,
					cmdslot->u.status.req[i].datadir);
	msg = json_object_to_json_string(obj);
	len = strlen(msg) + 1;
	if (0 != (e = gpcc_net_send_packetx(cmdslot->u.status.req[i].node,
					    STATUS_REQUEST_MSG,
					    cmdslot->cmdidx,
					    cmdslot->u.status.parent_cmdidx,
					    msg, len))) {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	}
	json_object_put(obj);
    }

    cmdslot->countdown = 2 * 60;
    return 0;
}


static apr_status_t a_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt = 0;
    char *seg, *status, *health, *alert;
    int i;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    health = gpcc_json_object_get_string(obj, "health", "failed");
    status = gpcc_json_object_get_string(obj, "status", "");
    alert = gpcc_json_object_get_string(obj, "alert", "");
    
    
    for (i = 0 ; i < cmdslot->u.status.ntotal ; i++) {
	if (strcmp(seg, cmdslot->u.status.req[i].unit) == 0) 
	    break;
    }
    if (i >= cmdslot->u.status.ntotal){
	gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	json_object_put(obj);
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    cmdslot->u.status.req[i].reply = apr_palloc(cmdslot->pool, ev->edatasz);
    memcpy(cmdslot->u.status.req[i].reply, ev->pdata, ev->edatasz);
    
    if (strcmp(status, "ok") == 0) {
	cmdslot->u.status.nsuccess++;
    } else {
	cmdslot->u.status.nfailed++;
	gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, health=%s, status=%s", seg, health, status);
    }

    if (cmdslot->u.status.nsuccess + cmdslot->u.status.nfailed == cmdslot->u.status.ntotal) {
	if (cmdslot->u.status.nfailed == 0) {
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

static void a_status_send_json_reply(gpcc_event_t *ev, cmdslot_t *cmdslot, struct json_object *obj)
{
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    char *objstr;

    objstr = json_object_to_json_string(obj);    
    evt = gpcc_event_alloc(UNITS_REPLY_MSG_EVENT, 
			   cmdslot->u.status.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = UNITS_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.status.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

}


static apr_status_t a_status_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    a_status_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}


static apr_status_t a_status_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("start", "failed", 0);
    a_status_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_status_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("start", "timeout", 0);
    a_status_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}
