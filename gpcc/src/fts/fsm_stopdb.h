static apr_status_t stopdb_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    apr_pool_create(&cmdslot->u.sys.request.pool, 0);
    memset(&cmdslot->u.sys.request.u, 0, sizeof(cmdslot->u.sys.request.u));
    cmdslot->u.sys.request.u.stop.replied = apr_pcalloc(cmdslot->u.sys.request.pool, sizeof(int) * cmdslot->u.sys.nseginfo);

    return 0;
}
static apr_status_t stopdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, idx, e;
    hainfo_t *hainfo;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *msg;
    apr_uint32_t *spin;

    for (i = 0 ;  i < cmdslot->u.sys.nseginfo ; i++) {
	idx = cmdslot->u.sys.seginfo[i].haidx;
	hainfo = cmdslot->u.sys.hainfo[idx];
	if (!hainfo) {
	    gpcc_warning(FLINE, "hainfo[%d] not found", idx);
	    evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, evt);
	    return 0;
	}

	cmdslot->u.sys.request.u.stop.replied[i] = 0;

	obj = json_tokener_parse(json_object_to_json_string(cmdslot->u.sys.config_json));
	obj = gpcc_json_segment_request(obj,
					cmdslot->u.sys.seginfo[i].name,
					"stop",
					gpcc_json_object_get_string(cmdslot->u.sys.request.reqobj, "mode", "immediate"),
					cmdslot->u.sys.seginfo[i].host,
					cmdslot->u.sys.seginfo[i].port,
					cmdslot->u.sys.seginfo[i].dir);

	msg = json_object_to_json_string(obj);

	spin = gpcc_spin_secure(gpfts.net_spin);
	if (!hainfo->ha || 0 != (e = gpcc_ha_send(hainfo->ha, SEGMENT_REQUEST_MSG, msg, strlen(msg) + 1))) {
	    gpcc_warning(FLINE, "send packet to %s failed", hainfo->host);
	    hainfo_close(hainfo);
	    cmdslot->u.sys.hainfo[i] = 0;
	    cmdslot->u.sys.network_failure++;
	    cmdslot->u.sys.request.u.stop.nreply++;
	    cmdslot->u.sys.request.u.stop.replied[i]++;
	    cmdslot->u.sys.request.u.stop.nfailed++;
	}
	spin = gpcc_spin_release(gpfts.net_spin);
	
	json_object_put(obj);
    }

    return 0;
}

static apr_status_t stopdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    int i;

    switch (ev->etype) {
    case NET_CLOSED_EVENT:
	{
	int index;
	memcpy(&index, ev->edata, sizeof(int));
	for (i = 0 ; i < cmdslot->u.sys.nseginfo ; i++) {
	    if (cmdslot->u.sys.seginfo[i].haidx == index) {
		if (cmdslot->u.sys.request.u.stop.replied[i] == 0) {
		    cmdslot->u.sys.request.u.stop.nfailed++;
		    cmdslot->u.sys.request.u.stop.nreply++;
		    cmdslot->u.sys.request.u.stop.replied[i]++;
		} else {
		    cmdslot->u.sys.request.u.stop.nfailed++;
		}
		gpcc_warning(FLINE, "[Error] segment %s connection reset", cmdslot->u.sys.seginfo[i].name);
	    }
	}
	break;
	}
    case SEGMENT_REPLY_MSG_EVENT:
	{
	char *seg, *status, *reason;
	gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->edata;

	obj = json_tokener_parse(pkt->mb);
	seg = gpcc_json_object_get_string(obj, "seg", 0);
	status = gpcc_json_object_get_string(obj, "status", "failed");
	reason = gpcc_json_object_get_string(obj, "reason", "");
	
	for (i = 0 ; i < cmdslot->u.sys.nseginfo ; i++) {
	    if (strcmp(seg, cmdslot->u.sys.seginfo[i].name) == 0) 
		break;
	}
	if (i >= cmdslot->u.sys.nseginfo){
	    gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	    goto bail;
	}

	cmdslot->u.sys.request.u.stop.replied[i]++;
	if (strcmp(status, "ok") == 0) {
	    cmdslot->u.sys.request.u.stop.nsuccess++;
	} else {
	    gpcc_warning(FLINE, "[Error] segment stop failed.  %s status=%s, reason=%s", seg, status, reason);
	    cmdslot->u.sys.request.u.stop.nfailed++;
	}
	cmdslot->u.sys.request.u.stop.nreply++;
	    
	break;
	}
    default:
	break;
    }

    if (cmdslot->u.sys.request.u.stop.nreply >= cmdslot->u.sys.nseginfo) {
	if (cmdslot->u.sys.request.u.stop.nfailed == 0) {
	    /* success */
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, evt);
	} else {
	    if (cmdslot->u.sys.network_failure) {
		evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	    } else {
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    }
	    gpcc_evqueue_put(gpfts.evq, evt);
	}
    }


    if (obj) json_object_put(obj);
    return 0;

    bail:

    if (obj) json_object_put(obj);
    return APR_EINVAL;
	
}

static apr_status_t stopdb_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t stopdb_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("stop", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);    
    return 0;
}

static apr_status_t stopdb_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    cmdslot->u.sys.request.outq = 0;
    if (cmdslot->u.sys.request.reqobj) {
	json_object_put(cmdslot->u.sys.request.reqobj);
	cmdslot->u.sys.request.reqobj = 0;
    }

    if (cmdslot->u.sys.request.pool) {
	apr_pool_destroy(cmdslot->u.sys.request.pool);
	cmdslot->u.sys.request.pool = 0;
    }
    
    return 0;
}
