static apr_status_t a_fts_stopdb_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    apr_pool_create(&cmdslot->u.fts.request.pool, 0);
    memset(&cmdslot->u.fts.request.u, 0, sizeof(cmdslot->u.fts.request.u));
    cmdslot->u.fts.request.u.stop.replied = apr_pcalloc(cmdslot->u.fts.request.pool, sizeof(int) * cmdslot->u.fts.nseginfo);

    cmdslot->u.fts.request.sndidx = pkt->mh.sndidx;
    cmdslot->u.fts.request.reqobj = json_tokener_parse(pkt->payload);

    return 0;
}
static apr_status_t a_fts_stopdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, idx, e;
    struct json_object *obj;
    char *msg;

    for (i = 0 ;  i < cmdslot->u.fts.nseginfo ; i++) {
	idx = cmdslot->u.fts.seginfo[i].haidx;

	cmdslot->u.fts.request.u.stop.replied[i] = 0;

	obj = json_tokener_parse(json_object_to_json_string(cmdslot->u.fts.config_json));
	obj = gpcc_json_segment_request(obj,
					cmdslot->u.fts.seginfo[i].name,
					"stop",
					gpcc_json_object_get_string(cmdslot->u.fts.request.reqobj, "mode", "immediate"),
					cmdslot->u.fts.seginfo[i].host,
					cmdslot->u.fts.seginfo[i].port,
					cmdslot->u.fts.seginfo[i].dir);

	msg = json_object_to_json_string(obj);

	if (0 != (e = gpcc_net_send_packetx(cmdslot->u.fts.seginfo[i].ha, 
					    SEGMENT_REQUEST_MSG, 
					    cmdslot->cmdidx,
					    cmdslot->u.fts.request.sndidx,
					    msg, strlen(msg) + 1))) {
	    gpcc_warning(FLINE, "send packet failed");
	    cmdslot->u.fts.network_failure++;
	    cmdslot->u.fts.request.u.stop.nreply++;
	    cmdslot->u.fts.request.u.stop.replied[i]++;
	    cmdslot->u.fts.request.u.stop.nfailed++;
	}
	
	json_object_put(obj);
    }

    return 0;
}

static apr_status_t a_fts_stopdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    int i;

    switch (ev->etype) {
    case NET_CLOSED_EVENT:
	{
	int index;
	gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
	if (1 != sscanf(pkt->payload, "node.%d", &index)) {
	    gpcc_warning(FLINE, "[Error] NET_CLOSED_EVENT format error");
	    evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    return 0;	 
	}	

	for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	    if (cmdslot->u.fts.seginfo[i].haidx == index) {
		if (cmdslot->u.fts.request.u.stop.replied[i] == 0) {
		    cmdslot->u.fts.request.u.stop.nfailed++;
		    cmdslot->u.fts.request.u.stop.nreply++;
		    cmdslot->u.fts.request.u.stop.replied[i]++;
		} else {
		    cmdslot->u.fts.request.u.stop.nfailed++;
		}
		gpcc_warning(FLINE, "[Error] segment %s connection reset", cmdslot->u.fts.seginfo[i].name);
	    }
	}
	break;
	}
    case SEGMENT_REPLY_MSG_EVENT:
	{
	char *seg, *status, *reason;
	gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

	obj = json_tokener_parse(pkt->payload);
	seg = gpcc_json_object_get_string(obj, "seg", 0);
	status = gpcc_json_object_get_string(obj, "status", "failed");
	reason = gpcc_json_object_get_string(obj, "reason", "");
	
	for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	    if (strcmp(seg, cmdslot->u.fts.seginfo[i].name) == 0) 
		break;
	}
	if (i >= cmdslot->u.fts.nseginfo){
	    gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	    goto bail;
	}

	cmdslot->u.fts.request.u.stop.replied[i]++;
	if (strcmp(status, "ok") == 0) {
	    cmdslot->u.fts.request.u.stop.nsuccess++;
	} else {
	    gpcc_warning(FLINE, "[Error] segment stop failed.  %s status=%s, reason=%s", seg, status, reason);
	    cmdslot->u.fts.request.u.stop.nfailed++;
	}
	cmdslot->u.fts.request.u.stop.nreply++;
	    
	break;
	}
    default:
	break;
    }

    if (cmdslot->u.fts.request.u.stop.nreply >= cmdslot->u.fts.nseginfo) {
	if (cmdslot->u.fts.request.u.stop.nfailed == 0) {
	    /* success */
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    if (cmdslot->u.fts.network_failure) {
		evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	    } else {
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    }
	    gpcc_evqueue_put(gpma.evq, evt);
	}
    }


    if (obj) json_object_put(obj);
    return 0;

    bail:

    if (obj) json_object_put(obj);
    return APR_EINVAL;
	
}

static apr_status_t a_fts_stopdb_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("stop", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.fts.request.sndidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.fts.request.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_stopdb_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("stop", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.fts.request.sndidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.fts.request.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);    
    return 0;
}

static apr_status_t a_fts_stopdb_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.fts.request.reqobj) {
	json_object_put(cmdslot->u.fts.request.reqobj);
	cmdslot->u.fts.request.reqobj = 0;
    }

    if (cmdslot->u.fts.request.pool) {
	apr_pool_destroy(cmdslot->u.fts.request.pool);
	cmdslot->u.fts.request.pool = 0;
    }
    
    return 0;
}
