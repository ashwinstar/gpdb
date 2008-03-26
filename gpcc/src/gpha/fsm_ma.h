
#define seg_is_standby_qd(c) (c->u.request.isqd && ! c->u.request.primary)
#define seg_is_master_qd(c) (c->u.request.isqd && c->u.request.primary)
#define seg_is_qd(c)  (c->u.request.isqd)

static event_t segment_msg_get_event(struct json_object *obj) 
{
    struct json_object *aobj = json_object_object_get(obj, "action");
    char *action;

    if (!aobj) {
	gpcc_warning(FLINE, "action is missing");
	return NULL_EVENT;
    }
    action = json_object_get_string(aobj);
    
    if (strcmp(action, "init") == 0) {
	return SEG_INITDB_EVENT;
    } else if (strcmp(action, "start") == 0) {
	return SEG_START_EVENT;
    } else if (strcmp(action, "stop") == 0) {
	return SEG_STOP_EVENT;
    } else if (strcmp(action, "delete") == 0) {
	return SEG_DELETE_EVENT;
    }

    return NULL_EVENT;

}

static char *proc_exit_reason(apr_exit_why_e exitwhy)
{
    switch (exitwhy) {
    case APR_PROC_EXIT:
	return "process terminated normally";
    case APR_PROC_SIGNAL:
	return "process was killed by a signal";
    case APR_PROC_SIGNAL_CORE:
	return "process was killed by a signal, and generated a core dump";
    default:
	return 0;
    }
    return 0;
}

static apr_status_t ma_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    gpcc_fatal(FLINE, "MA connection closed....shutdown");
    return 0;
}

static apr_status_t ma_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
   
    gpcc_log_info("ma_status_reply");
    return gpcc_net_send_packet(cmdslot->conn, STATUS_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);    
}

static apr_status_t ma_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("[%s] segment stop reply [status = %s]", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("[%s] segment start reply [status = %s]", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("[%s] segment initdb reply [status = %s]", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_delete_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("[%s] segment delete reply [status = %s]", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_dispatch_segment(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *seg = 0;
    cmdslot_t *slot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(packet->payload);
    struct json_object *segobj;
    event_t etype;
    gpcc_event_t *evt;

    if (!obj) {
	gpcc_warning(FLINE, "json_tokener_parse error");
	return 0;
    }

    segobj = json_object_object_get(obj, "seg");


    if (!segobj) {
	gpcc_warning(FLINE, "segment name is missing");
	json_object_put(obj);
	return 0;
    }
    seg = json_object_get_string(segobj);

    switch (ev->etype) {
    case SEGMENT_REQUEST_MSG:
	etype = segment_msg_get_event(obj);
	break;
    case STATUS_REQUEST_MSG:
	etype = SEG_STATUS_EVENT;
	break;
    default:
	gpcc_warning(FLINE, "invalid message type %d", ev->etype);
	json_object_put(obj);
	return 0;
    }

    if (etype == NULL_EVENT) {
	gpcc_warning(FLINE, "action is missing");
	json_object_put(obj);
	return 0;
    }
	
    /* new segment request */
    slot = cmdslot_alloc(seg, SEG_INIT_CMDSTATE, 0);
    evt = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len);
    memcpy(evt->pdata, packet, sizeof(*packet) + packet->mh.len - 1);
    packet = (gpcc_net_packet_t *) evt->pdata;
    packet->mh.rcvidx = cmdslot->cmdidx;
    gpcc_evqueue_put(gpha.evq, evt);    
    
    json_object_put(obj);
    return 0;
}


static apr_status_t seg_request_cleanup(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.request.msg) {
	json_object_put(cmdslot->u.request.msg);
    }
    return 0;
}


static apr_status_t seg_request_reply(gpcc_event_t* ev, void *_cmdslot, event_t event)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    char *status;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    status = (cmdslot->u.request.exitcode == 0) ? "ok" : "failure";

    obj = gpcc_json_segment_reply(cmdslot->cmdname, status, cmdslot->u.request.reason);
    objstr = json_object_to_json_string(obj);
    evt = gpcc_event_alloc(event, cmdslot->u.request.parentidx, sizeof(*pkt) + strlen(objstr)+1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = event;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.request.maidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);
    gpcc_evqueue_put(gpha.evq, evt);
    json_object_put(obj);

    return 0;   
}

static apr_status_t seg_request_error(const char *fline, gpcc_event_t *ev, void *_cmdslot, event_t event, int exitcode, char *fmt, ...)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(event, cmdslot->cmdidx, 0);
    cmdslot->u.request.exitcode = exitcode;
    if (fmt) {
	va_list ap;
	va_start(ap, fmt);
	cmdslot->u.request.reason = apr_pvsprintf(cmdslot->pool, fmt, ap);
	va_end(ap);
    } else {
	cmdslot->u.request.reason = "";
    }
    gpcc_warning(fline, cmdslot->u.request.reason);
    gpcc_evqueue_put(gpha.evq, evt);

    return 0;


}



static apr_status_t seg_cmdname_parse(const char *segname, int *isqd, int *idx, int *primary)
{
    int index;

    if (strstr(segname, "gpdb.seg.m.")) {
	/* this is QD */
	if (1 != sscanf(segname, "gpdb.seg.m.%d", &index)) {
	    gpcc_warning(FLINE, "segment name format error %s", segname);
	    return 1;
	}
	
	*isqd = 1;
	*idx = index;
	*primary = (index == 0) ? 1 : 0;

	return 0;
    } else {
	if (2 != sscanf(segname, "gpdb.seg.%d.%d", idx, primary)) {
	    gpcc_warning(FLINE, "segment name format error %s", segname);
	    return 1;
	}

	*isqd = 0;

	return 0;
    }

    return 0;
}

/* initialize */
static apr_status_t seg_initialize(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    int e;

    memset(&cmdslot->u.request, 0, sizeof(cmdslot->u.request));
    cmdslot->u.request.maidx = packet->mh.sndidx;
    cmdslot->u.request.parentidx = packet->mh.rcvidx;    

    if (0 != (e = seg_cmdname_parse(cmdslot->cmdname,
				    &cmdslot->u.request.isqd,
				    &cmdslot->u.request.idx,
				    &cmdslot->u.request.primary))) {

	gpcc_warning(FLINE, "segment name invalid format error");
	cmdslot->u.request.exitcode = APR_EINVAL;
	cmdslot->u.request.reason = "segment name invalid format error";
	return APR_EINVAL;
    }

    return 0;
}


