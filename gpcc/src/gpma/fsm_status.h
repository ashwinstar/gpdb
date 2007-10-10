static apr_status_t a_fts_status_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    memset(&cmdslot->u.fts.u, 0, sizeof(cmdslot->u.fts.u));
    cmdslot->u.fts.u.status.replied = apr_pcalloc(cmdslot->pool, sizeof(int) * cmdslot->u.fts.nseginfo);

    cmdslot->u.fts.sndidx = pkt->mh.sndidx;
    cmdslot->u.fts.reqobj = json_tokener_parse(pkt->payload);

    return 0;
}
static apr_status_t a_fts_status_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, idx, e;
    struct json_object *obj;
    char *msg;

    for (i = 0 ;  i < cmdslot->u.fts.nseginfo ; i++) {
	idx = cmdslot->u.fts.seginfo[i].haidx;

	cmdslot->u.fts.u.status.replied[i] = 0;

	obj = gpcc_json_segment_request(0,
					cmdslot->u.fts.seginfo[i].name,
					"status",
					"",
					cmdslot->u.fts.seginfo[i].host,
					cmdslot->u.fts.seginfo[i].port,
					cmdslot->u.fts.seginfo[i].dir);

	msg = json_object_to_json_string(obj);

	if (0 != (e = gpcc_net_send_packetx(cmdslot->u.fts.seginfo[i].ha,
					    STATUS_REQUEST_MSG, 
					    cmdslot->cmdidx,
					    cmdslot->u.fts.sndidx,
					    msg, strlen(msg) + 1))) {
	    gpcc_warning(FLINE, "send packet failed");
	    cmdslot->u.fts.network_failure++;
	    cmdslot->u.fts.u.status.nreply++;
	    cmdslot->u.fts.u.status.replied[i]++;
	    cmdslot->u.fts.u.status.nfailed++;
	}
	json_object_put(obj);
    }

    return 0;
}

static apr_status_t a_fts_status_reply(gpcc_event_t *ev, void *_cmdslot)
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
		if (cmdslot->u.fts.u.status.replied[i] == 0) {
		    cmdslot->u.fts.u.status.nfailed++;
		    cmdslot->u.fts.u.status.nreply++;
		    cmdslot->u.fts.u.status.replied[i]++;
		} else {
		    cmdslot->u.fts.u.status.nfailed++;
		}
		gpcc_warning(FLINE, "[Error] segment %s connection reset", cmdslot->u.fts.seginfo[i].name);
	    }
	}
	break;
	}
    case STATUS_REPLY_MSG_EVENT:
	{
	char *seg, *health, *status, *alert;
	gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

	gpcc_log_info("[status_reply] %s", pkt->payload);

	obj = json_tokener_parse(pkt->payload);
	seg = gpcc_json_object_get_string(obj, "seg", 0);
	health = gpcc_json_object_get_string(obj, "health", "failed");
	status = gpcc_json_object_get_string(obj, "status", "");
	alert = gpcc_json_object_get_string(obj, "alert", "");
	
	for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	    if (strcmp(seg, cmdslot->u.fts.seginfo[i].name) == 0) 
		break;
	}
	if (i >= cmdslot->u.fts.nseginfo){
	    gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	    goto bail;
	}

	cmdslot->u.fts.u.status.replied[i]++;
	if (strcmp(health, "ok") == 0) {
	    cmdslot->u.fts.u.status.nsuccess++;
	} else {
	    gpcc_warning(FLINE, "[Error] segment status failed.  %s health=%s, alert=%s, status=%s", seg, health, alert, status);
	    cmdslot->u.fts.u.status.nfailed++;
	}
	cmdslot->u.fts.u.status.nreply++;
	    
	break;
	}
    default:
	break;
    }

    if (cmdslot->u.fts.u.status.nreply >= cmdslot->u.fts.nseginfo) {
	if (cmdslot->u.fts.u.status.nfailed == 0) {
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

static apr_status_t a_fts_status_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("status", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.fts.sndidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.fts.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_status_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("status", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.fts.sndidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.fts.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);

    gpcc_evqueue_put(gpma.evq, evt);

    json_object_put(obj);    
    return 0;
}

static apr_status_t a_fts_status_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.fts.reqobj) {
	json_object_put(cmdslot->u.fts.reqobj);
	cmdslot->u.fts.reqobj = 0;
    }

    if (cmdslot->u.fts.config_json) {
	json_object_put(cmdslot->u.fts.config_json);
	cmdslot->u.fts.config_json = 0;
    }
    
    return 0;
}
