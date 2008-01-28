
#define UNIT_STOPPING_STATUS                        "stopping"
#define UNIT_STOPPED_STATUS                         "stopped"
#define UNIT_STARTING_STATUS                        "starting"
#define UNIT_RUNNING_STATUS                         "running"
#define UNIT_DELETING_STATUS                        "deleting"
#define UNIT_INITIALIZING_STATUS                    "initializing"
#define UNIT_INVALID_STATUS                         "invalid"
#define UNIT_CONNECTING_STATUS                      "connecting"


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
    case UNIT_DELETING_CMDSTATE:
	reason = "segment is deleting";
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


static apr_status_t a_unit_status(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_conn_t conn;
    char *state;
    struct json_object *obj;

    memcpy(&conn, ev->pdata, sizeof(conn));

    switch (slot->cmdstate) {
    case UNIT_CMDSTATE:
	state = UNIT_CONNECTING_STATUS;
	break;
    case UNIT_STOPPING_CMDSTATE:
	state = UNIT_STOPPING_STATUS;
	break;
    case UNIT_STOPPED_CMDSTATE:
	state = UNIT_STOPPED_STATUS;
	break;
    case UNIT_STARTING_CMDSTATE:
	state = UNIT_STARTING_STATUS;
	break;
    case UNIT_RUNNING_CMDSTATE:
	state = UNIT_RUNNING_STATUS;
	break;
    case UNIT_DELETING_CMDSTATE:
	state = UNIT_DELETING_STATUS;
	break;
    default:
	state = UNIT_INVALID_STATUS;
	break;
    }
    
    obj = gpcc_json_http_segment_status_reply(slot->cmdname, "ok", state);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}


static apr_status_t a_unit_delete(gpcc_event_t *ev, void *arg)
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

static apr_status_t a_unit_delete_reply(gpcc_event_t *ev, void *arg)
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


static apr_status_t a_unit_postgres_status(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int e;
    struct json_object *obj;
    char *msg;
    int len;

    obj = gpcc_json_segment_request(0,
				    slot->cmdname,
				    "status",
				    "",
				    slot->u.unit.host,
				    slot->u.unit.port,
				    slot->u.unit.datadir);
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;
    if (0 != (e = gpcc_net_send_packetx(slot->u.unit.node,
					STATUS_REQUEST_MSG,
					slot->cmdidx,
					0,
					msg, len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	json_object_put(obj);
	return 0;
    }
    
    json_object_put(obj);
    slot->countdown = 30;

    return 0;
}

static apr_status_t a_unit_postgres_status_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    char *seg, *status, *health;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "stopped");
    health = gpcc_json_object_get_string(obj, "health", "failed");

    if (strcmp(health, "ok") == 0 && strcmp(status, "running") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    } else {
	gpcc_log_info("seg = %s health = %s, status = %s, (%s)", slot->cmdname, health, status, pkt->payload);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);

    slot->countdown = 0;
    return 0;

}


static apr_status_t a_unit_schedule_check(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = (cmdslot_t *) arg;

    slot->countdown = 30;
    return 0;
}

static apr_status_t a_unit_failover(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = (cmdslot_t *) arg;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    int i, idx;
    int content;
    char definedprimary;

    for (i = 0 ; i < nseginfo ; i++) {
	if (strcmp(seginfo[i].name, slot->cmdname) == 0)
	    break;
    }

    idx = i;
    seginfo[i].valid = 'f';

    /* failover */
    if (seginfo[i].isprimary != 't') {
	/* set gp_configuration valid = 'f' */
	gpcc_warning(FLINE, "[%s] mirror segment is down.", slot->cmdname);
	return 0;
    }

    /* if there is mirror, switch to mirror otherwise system down */
    content = seginfo[i].content;
    definedprimary = (seginfo[i].definedprimary == 't') ? 'f' : 't';

    
    for (i = 0 ; i < nseginfo ; i++) {
	if (seginfo[i].content == content && seginfo[i].definedprimary == definedprimary)
	    break;
    }

    if (i >= nseginfo) {
	/* no mirror, system down */
	gpcc_warning(FLINE, "[%s] segment is down and no mirror in the system.  SYSTEM DOWN.", slot->cmdname);
	return 0;
    }

    if (seginfo[i].valid != 't') {
	/* both mirror and primary is down, system down */
	gpcc_warning(FLINE, "[%s] both mirror and primary segment is down. SYSTEM DOWN.", slot->cmdname);
	return 0;
    }

    /* set mirror to primary */
    gpcc_warning(FLINE, "[%s] segment is down and switch to segment %s", slot->cmdname, seginfo[i].name);
    seginfo[idx].isprimary = 'f';
    seginfo[i].isprimary = 't';

    gpcc_log_info("number of segment = %d", nseginfo);
    gpcc_log_info("name | host | dbid | content | port | dir | haidx | ha | definedprimary | isprimary | valid");
    for (i = 0 ; i < nseginfo ; i++) {
	gpcc_log_info("%s | %s | %d | %d | %d | %s | %d | %s | %c | %c | %c",
		      seginfo[i].name,
		      seginfo[i].host,
		      seginfo[i].dbid,
		      seginfo[i].content,
		      seginfo[i].port,
		      seginfo[i].dir,
		      seginfo[i].haidx,
		      seginfo[i].ha,
		      seginfo[i].definedprimary,
		      seginfo[i].isprimary,
		      seginfo[i].valid);
    }

    return 0;
}
