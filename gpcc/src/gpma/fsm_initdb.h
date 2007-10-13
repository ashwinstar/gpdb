static apr_status_t a_initdb_init(gpcc_event_t *ev, void *_cmdslot)
{
    /* build config in json object */

    return 0;
}


static apr_status_t a_initdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i, e;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *msg;
    int len = 0;

    i = cmdslot->u.initdb.idx++;
    if (i >= cmdslot->u.initdb.ntotal)
	return 0;

    obj = json_tokener_parse(cmdslot->u.initdb.config);
    obj = gpcc_json_segment_request(obj,
				    cmdslot->u.initdb.req[i].node,
				    "init",
				    cmdslot->u.initdb.req[i].mode,
				    cmdslot->u.initdb.req[i].host,
				    cmdslot->u.initdb.req[i].port,
				    cmdslot->u.initdb.req[i].datadir);
    json_object_object_add(obj, "dbid", json_object_new_int(cmdslot->u.initdb.req[i].dbid));
    json_object_object_add(obj, "content", json_object_new_int(cmdslot->u.initdb.req[i].content));	
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;
    if (0 != (e = gpcc_net_send_packetx(cmdslot->u.initdb.req[i].node,
					SEGMENT_REQUEST_MSG,
					cmdslot->cmdidx,
					cmdslot->u.initdb.parent_cmdidx,
					msg, len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    json_object_put(obj);

    cmdslot->countdown = 60 * 60;
    return 0;
}


static apr_status_t a_initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt = 0;
    char *seg, *status, *reason;
    int i, success = 0;

    cmdslot->countdown = 0;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");
    
    
    for (i = 0 ; i < cmdslot->u.initdb.ntotal ; i++) {
	if (strcmp(seg, cmdslot->u.initdb.req[i].unit) == 0) 
	    break;
    }
    if (i >= cmdslot->u.initdb.ntotal){
	gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	json_object_put(obj);
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    cmdslot->u.initdb.req[i].reply = apr_palloc(cmdslot->pool, ev->edatasz);
    memcpy(cmdslot->u.initdb.req[i].reply, ev->pdata, ev->edatasz);
    
    if (strcmp(status, "ok") == 0) {
	cmdslot->u.initdb.nsuccess++;
	success = 1;
    } else {
	cmdslot->u.initdb.nfailed++;
	gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, status=%s, reason=%s", seg, status, reason);
    }

    if (cmdslot->u.initdb.nsuccess + cmdslot->u.initdb.nfailed == cmdslot->u.initdb.ntotal) {
	if (cmdslot->u.initdb.nfailed == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
	}
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->countdown = 0;
    } else {
	if (success) {
	    evt = gpcc_event_alloc(CONTINUE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    cmdslot->countdown = 0;
	}
    }

    if (obj) json_object_put(obj);
    return 0;
}

static void a_initdb_send_json_reply(gpcc_event_t *ev, cmdslot_t *cmdslot, struct json_object *obj)
{
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    char *objstr;

    objstr = json_object_to_json_string(obj);    
    evt = gpcc_event_alloc(UNITS_REPLY_MSG_EVENT, 
			   cmdslot->u.initdb.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = UNITS_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.initdb.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

}


static apr_status_t a_initdb_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}


static apr_status_t a_initdb_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("start", "failed", 0);
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_initdb_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = gpcc_json_array_reply("start", "timeout", 0);
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}
