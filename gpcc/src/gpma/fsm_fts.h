static apr_status_t a_fts_init(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}


static apr_status_t a_fts_array_start(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    p = cmdslot_alloc("ARRAY_START", FTS_START_CMDSTATE);
    p->u.array_start.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_start.conn = conn;
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_fts_array_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    obj = json_tokener_parse(pkt->payload);

    status = gpcc_json_object_get_string(obj, "status", "failed");
    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}


static apr_status_t a_fts_busy_reply(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}

static apr_status_t a_fts_running(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}


static apr_status_t a_fts_array_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    p = cmdslot_alloc("ARRAY_STOP", FTS_STOP_CMDSTATE);
    p->u.array_stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_stop.conn = conn;
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_fts_array_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    obj = json_tokener_parse(pkt->payload);

    status = gpcc_json_object_get_string(obj, "status", "failed");
    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}


static apr_status_t a_fts_stopped(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}

static apr_status_t a_fts_failure(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}
