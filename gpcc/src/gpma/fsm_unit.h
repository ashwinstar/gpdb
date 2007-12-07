static apr_status_t a_unit_stop(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    gpcc_event_t *evt;
    int e;
    struct json_object *obj;
    char *msg;
    int len;

    slot->u.unit.parentidx = pkt->mh.sndidx;

    if (0 != (e = gpcc_net_send_packetx(slot->u.unit.node,
					SEGMENT_REQUEST_MSG,
					slot->cmdidx,
					0,
					pkt->payload,
					pkt->mh.len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	slot->u.unit.parentidx = 0;

	obj = gpcc_json_segment_reply(slot->cmdname, "failed", "network failure");
	msg = json_object_to_json_string(obj);
	len = strlen(msg) + 1;
	
	evt = gpcc_event_alloc(SEGMENT_REPLY_MSG_EVENT, pkt->mh.sndidx, sizeof(*pkt) + len);
	pkt = (gpcc_net_packet_t *) evt->pdata;
	pkt->mh.sndidx = slot->cmdidx;
	pkt->mh.rcvidx = 0;
	pkt->mh.len = len;
	strcpy(pkt->payload, msg);
	
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    slot->countdown = 60 * 5;

    return 0;
}

static apr_status_t a_unit_stop_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    char *seg, *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");

    evt = gpcc_event_alloc(ev->etype, slot->u.unit.parentidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    slot->u.unit.parentidx = 0;

    slot->countdown = 0;
    return 0;
}

static apr_status_t a_unit_timeout(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    char *msg;
    int len;
    gpcc_event_t *evt;

    obj = gpcc_json_segment_reply(slot->cmdname, "failed", "timeout");
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;
    
    evt = gpcc_event_alloc(SEGMENT_REPLY_MSG_EVENT, slot->u.unit.parentidx, sizeof(*pkt) + len);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.sndidx = slot->cmdidx;
    pkt->mh.rcvidx = 0;
    pkt->mh.len = len;
    strcpy(pkt->payload, msg);
    
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}


static apr_status_t a_unit_stopped(gpcc_event_t *ev, void *arg)
{

    return 0;
}

static apr_status_t a_unit_invalid(gpcc_event_t *ev, void *arg)
{

    return 0;
}

static apr_status_t a_unit_start(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    gpcc_event_t *evt;
    int e;
    struct json_object *obj;
    char *msg;
    int len;

    slot->u.unit.parentidx = pkt->mh.sndidx;

    if (0 != (e = gpcc_net_send_packetx(slot->u.unit.node,
					SEGMENT_REQUEST_MSG,
					slot->cmdidx,
					0,
					pkt->payload,
					pkt->mh.len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	slot->u.unit.parentidx = 0;

	obj = gpcc_json_segment_reply(slot->cmdname, "failed", "network failure");
	msg = json_object_to_json_string(obj);
	len = strlen(msg) + 1;
	
	evt = gpcc_event_alloc(SEGMENT_REPLY_MSG_EVENT, pkt->mh.sndidx, sizeof(*pkt) + len);
	pkt = (gpcc_net_packet_t *) evt->pdata;
	pkt->mh.sndidx = slot->cmdidx;
	pkt->mh.rcvidx = 0;
	pkt->mh.len = len;
	strcpy(pkt->payload, msg);
	
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    slot->countdown = 60 * 5;
    return 0;
}

static apr_status_t a_unit_start_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    char *seg, *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");

    evt = gpcc_event_alloc(ev->etype, slot->u.unit.parentidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    slot->u.unit.parentidx = 0;
    slot->countdown = 0;
    return 0;
}


static apr_status_t a_unit_running(gpcc_event_t *ev, void *arg)
{

    return 0;
}

static apr_status_t a_unit_busy_ok_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj;
    char *reason = "";
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    gpcc_event_t *evt;
    char *msg;
    int len = 0;

    switch (slot->cmdstate) {
    case UNIT_CMDSTATE:
	reason = "segment is waking up";
	break;
    case UNIT_STOPPING_CMDSTATE:
	reason = "segment is stopping";
	break;
    case UNIT_STOPPED_CMDSTATE:
	reason = "segment stopped";
	break;
    case UNIT_STARTING_CMDSTATE:
	reason = "segment is starting";
	break;
    case UNIT_RUNNING_CMDSTATE:
	reason = "segment is running";
	break;
    default:
	reason = "invalid segment state";
	break;
    }

    obj = gpcc_json_segment_reply(slot->cmdname, "ok", reason);
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;

    evt = gpcc_event_alloc(SEGMENT_REPLY_MSG_EVENT, pkt->mh.sndidx, sizeof(*pkt) + len);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.sndidx = slot->cmdidx;
    pkt->mh.rcvidx = 0;
    pkt->mh.len = len;
    strcpy(pkt->payload, msg);

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_unit_busy_error_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj;
    char *reason = "";
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    gpcc_event_t *evt;
    char *msg;
    int len = 0;

    switch (slot->cmdstate) {
    case UNIT_CMDSTATE:
	reason = "segment is waking up";
	break;
    case UNIT_STOPPING_CMDSTATE:
	reason = "segment is stopping";
	break;
    case UNIT_STOPPED_CMDSTATE:
	reason = "segment stopped";
	break;
    case UNIT_STARTING_CMDSTATE:
	reason = "segment is starting";
	break;
    case UNIT_RUNNING_CMDSTATE:
	reason = "segment is running";
	break;
    default:
	reason = "invalid segment state";
	break;
    }

    obj = gpcc_json_segment_reply(slot->cmdname, "failed", reason);
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;

    evt = gpcc_event_alloc(SEGMENT_REPLY_MSG_EVENT, pkt->mh.sndidx, sizeof(*pkt) + len);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.sndidx = slot->cmdidx;
    pkt->mh.rcvidx = 0;
    pkt->mh.len = len;
    strcpy(pkt->payload, msg);

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}
