

static apr_status_t a_fts_start_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    memset(&cmdslot->u.fts.u, 0, sizeof(cmdslot->u.fts.u));
    cmdslot->u.fts.u.start.replied = apr_pcalloc(cmdslot->pool, sizeof(int) * cmdslot->u.fts.nseginfo);

    cmdslot->u.fts.sndidx = pkt->mh.sndidx;
    cmdslot->u.fts.reqobj = json_tokener_parse(pkt->payload);
    return 0;
}

static apr_status_t a_fts_start_utility_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.fts.seginfo[0];    

    obj = gpcc_json_segment_request(0, 
				    seginfo->name, 
				    "start", 
				    "utility", 
				    seginfo->host, 
				    seginfo->port, 
				    seginfo->dir);    

    msg = json_object_to_json_string(obj);

    gpcc_log_info("start utility qd %s", msg);
    if (0 != (e = gpcc_net_send_packetx(seginfo->ha, SEGMENT_REQUEST_MSG, 
					cmdslot->cmdidx,
					cmdslot->u.fts.sndidx,
					msg, strlen(msg) + 1))){
	gpcc_warning(FLINE, "gpcc_net_send_packetx failed");
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->u.fts.network_failure++;
    }
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_start_utility_qd_segment_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start utility qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (cmdslot->u.fts.network_failure) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
    } else {
	if (strcmp(status, "ok") == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_warning(FLINE, "[Error] start utility qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
	}
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_start_utility_qd_net_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo = cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    if (strcmp(seginfo->ha, pkt->payload) == 0) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
    return 0;
}

static apr_status_t a_fts_start_gpconf(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo;
    int e;
    gpcc_event_t *evt;

    seginfo = &cmdslot->u.fts.seginfo[0];
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

static apr_status_t a_fts_start_utility_qd_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.fts.seginfo[0];    

    obj = gpcc_json_segment_request(0, 
				    seginfo->name, 
				    "stop", 
				    "immediate", 
				    seginfo->host, 
				    seginfo->port, 
				    seginfo->dir);    

    msg = json_object_to_json_string(obj);
    
    if (0 != (e = gpcc_net_send_packetx(seginfo->ha, SEGMENT_REQUEST_MSG, 
					cmdslot->cmdidx,
					cmdslot->u.fts.sndidx,
					msg, strlen(msg) + 1))){
	gpcc_warning(FLINE, "gpcc_net_send_packetx failed");
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->u.fts.network_failure++;
    }
    json_object_put(obj);
    return 0;
}


static apr_status_t a_fts_start_gpconf_net_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo = cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    if (strcmp(seginfo->ha, pkt->payload) == 0) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
    return 0;
}

static apr_status_t a_fts_start_utility_qd_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start utility qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (cmdslot->u.fts.network_failure) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
    } else {
	if (strcmp(status, "ok") == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_warning(FLINE, "[Error] stop utility qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
	}
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}


static apr_status_t a_fts_start_qes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, e;
    seginfo_t *seginfo;
    struct json_object *obj;
    char *msg;

    cmdslot->u.fts.u.start.nreply = 0;
    cmdslot->u.fts.u.start.nfailed = 0;
    cmdslot->u.fts.u.start.nsuccess = 0;
    cmdslot->u.fts.u.start.nrequest = 0;
    memset(cmdslot->u.fts.u.start.replied, 0, sizeof(int) * cmdslot->u.fts.nseginfo);

    for (i = 1; i < cmdslot->u.fts.nseginfo ; i++) {
	seginfo = &cmdslot->u.fts.seginfo[i];
	if (strstr(seginfo->name, "gpdb.seg.m") != 0)
	    break;

	obj = gpcc_json_segment_request(0, 
					seginfo->name, 
					"start", 
					"normal", 
					seginfo->host, 
					seginfo->port, 
					seginfo->dir);

	msg = json_object_to_json_string(obj);
	if (0 != (e = gpcc_net_send_packetx(seginfo->ha, SEGMENT_REQUEST_MSG, 
					    cmdslot->cmdidx,
					    cmdslot->u.fts.sndidx,
					    msg, strlen(msg) + 1))){
	    gpcc_warning(FLINE, "gpcc_ha_send failed");
	    cmdslot->u.fts.u.start.nreply++;
	    cmdslot->u.fts.u.start.nfailed++;
	    cmdslot->u.fts.network_failure++;
	}
	json_object_put(obj);

	cmdslot->u.fts.u.start.nrequest++;	
    }
    
    
    
    return 0;
}

static apr_status_t a_fts_start_qe_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt = 0;
    char *seg, *status, *reason;
    int i;

    switch (ev->etype) {
    case NET_CLOSED_EVENT:
	{
	pkt = (gpcc_net_packet_t *) ev->pdata;

	for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	    if (strcmp(cmdslot->u.fts.seginfo[i].ha, pkt->payload) == 0) {
		if (cmdslot->u.fts.u.start.replied[i] == 0) {
		    cmdslot->u.fts.u.start.nfailed++;
		    cmdslot->u.fts.u.start.nreply++;
		    cmdslot->u.fts.u.start.replied[i]++;
		} else {
		    cmdslot->u.fts.u.start.nfailed++;
		}
		gpcc_warning(FLINE, "[Error] segment %s connection reset", cmdslot->u.fts.seginfo[i].name);
	    }
	}
	break;
	}
    case SEGMENT_REPLY_MSG_EVENT:	    
	{
	    pkt = (gpcc_net_packet_t *) ev->pdata;
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
		json_object_put(obj);
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		return 0;
	    }
	    
	    cmdslot->u.fts.u.start.replied[i]++;
	    cmdslot->u.fts.u.start.nreply++;

	    if (strcmp(status, "ok") == 0) {
		cmdslot->u.fts.u.start.nsuccess++;
	    } else {
		cmdslot->u.fts.u.start.nfailed++;
		gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, status=%s, reason=%s", seg, status, reason);
	    }
	    break;
	}
    default:
	break;
    }

    if (cmdslot->u.fts.u.start.nreply == cmdslot->u.fts.u.start.nrequest) {
	if (cmdslot->u.fts.u.start.nfailed == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    if (cmdslot->u.fts.network_failure) {
		evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	    } else {
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
	    }
	}
	gpcc_evqueue_put(gpma.evq, evt);
    }

    if (obj) json_object_put(obj);
    return 0;
}


static apr_status_t a_fts_start_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int e;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    seginfo = &cmdslot->u.fts.seginfo[0];    

    obj = gpcc_json_segment_request(0, 
				    seginfo->name, 
				    "start", 
				    "normal", 
				    seginfo->host, 
				    seginfo->port, 
				    seginfo->dir);    

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_net_send_packetx(seginfo->ha, SEGMENT_REQUEST_MSG, 
				  cmdslot->cmdidx,
				  cmdslot->u.fts.sndidx,
				  msg, strlen(msg) + 1))) {
	gpcc_warning(FLINE, "gpcc_net_send_packetx failed");
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->u.fts.network_failure++;

    }
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_start_qd_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.0") != 0) {
	gpcc_warning(FLINE, "start production qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }

    if (cmdslot->u.fts.network_failure) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
    } else {
	if (strcmp(status, "ok") == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_warning(FLINE, "[Error] start production qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
	}
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}


static apr_status_t a_fts_start_qd_reply_net_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo = cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    
    pkt = (gpcc_net_packet_t *) ev->pdata;
    if (strcmp(seginfo->ha, pkt->payload) == 0) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
    return 0;
}


static apr_status_t a_fts_start_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    int idx, e;
    seginfo_t *seginfo;
    gpcc_event_t *evt;
    char *msg;

    idx = cmdslot->u.fts.nseginfo-1;
    if (strstr(cmdslot->u.fts.seginfo[idx].name, "gpdb.seg.m.") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
	    
    seginfo = &cmdslot->u.fts.seginfo[idx];    

    obj = gpcc_json_segment_request(0, 
				    seginfo->name, 
				    "start", 
				    "normal", 
				    seginfo->host, 
				    seginfo->port, 
				    seginfo->dir);    

    msg = json_object_to_json_string(obj);
    if (0 != (e = gpcc_net_send_packetx(seginfo->ha, SEGMENT_REQUEST_MSG, 
				       cmdslot->cmdidx,
				       cmdslot->u.fts.sndidx,
				       msg, strlen(msg) + 1))){
	gpcc_warning(FLINE, "gpcc_net_send_packetx failed");
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->u.fts.network_failure++;
    }
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_start_standby_qd_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *seg, *status, *reason;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");

    if (strcmp(seg, "gpdb.seg.m.1") != 0) {
	gpcc_warning(FLINE, "start standby qd:  invalid segment name %s", seg);
	json_object_put(obj);
	return 0;
    }
    
    if (cmdslot->u.fts.network_failure) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
    } else {
	if (strcmp(status, "ok") == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_warning(FLINE, "[Error] start standby qd failed: segment=%s, status=%d, reason=%s", seg, status, reason);
	}
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_start_standby_qd_reply_net_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo = cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    
    pkt = (gpcc_net_packet_t *) ev->pdata;
    if (strcmp(seginfo->ha, pkt->payload) == 0) {
	evt = gpcc_event_alloc(NET_FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
    return 0;
}

static apr_status_t a_fts_start_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("start", "ok", 0);
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

static apr_status_t a_fts_start_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    obj = gpcc_json_array_reply("start", "failed", 0);
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

static apr_status_t a_fts_start_cleanup(gpcc_event_t *ev, void *_cmdslot)
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

