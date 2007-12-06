static apr_status_t a_unit_stop(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    gpcc_event_t *evt;
    int e;

    slot->u.segment.parentidx = pkt->mh.sndidx;
    if (0 != (e = gpcc_net_send_packetx(slot->u.segment.node,
					SEGMENT_REQUEST_MSG,
					slot->cmdidx,
					0,
					pkt->payload,
					pkt->mh.len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
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

    evt = gpcc_event_alloc(ev->etype, slot->u.segment.parentidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    slot->u.segment.parentidx = 0;

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
    
    slot->u.segment.parentidx = pkt->mh.sndidx;
    if (0 != (e = gpcc_net_send_packetx(slot->u.segment.node,
					SEGMENT_REQUEST_MSG,
					slot->cmdidx,
					0,
					pkt->payload,
					pkt->mh.len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    
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

    evt = gpcc_event_alloc(ev->etype, slot->u.segment.parentidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    slot->u.segment.parentidx = 0;
    return 0;
}


static apr_status_t a_unit_running(gpcc_event_t *ev, void *arg)
{

    return 0;
}

