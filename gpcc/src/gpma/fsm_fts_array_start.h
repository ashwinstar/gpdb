
static apr_status_t a_fts_array_start(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url, *mode = "normal";
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;
    struct json_object *obj;
    gpcc_net_uri_t *uri;
    char *y, *R, *r, *x;

    y = R = r = x = 0;
    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_alloc(url);
    if (uri) {
	char *m = gpcc_net_uri_param(uri, "mode");
	if (m) {
	    char *opt[2] = {"normal", "force"};
	    int i;
	    for (i = 0 ; i < gpcc_lengthof(opt) ; i++) {
		if (apr_strnatcasecmp(m, opt[i]) == 0)
		    break;
	    }
	    if (i >= gpcc_lengthof(opt)) {
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		gpcc_net_uri_free(uri);
		obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
					   "start", FAIL_STATUS, 0, "parameter missing");
		gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
		json_object_put(obj);
		return 0;
	    }
	    mode = m;
	}

	y = gpcc_net_uri_param(uri, "y");
	R = gpcc_net_uri_param(uri, "R");
	r = gpcc_net_uri_param(uri, "r");
	x = gpcc_net_uri_param(uri, "x");
    }

    p = cmdslot_alloc("ARRAY_START", FTS_ARRAY_START_CMDSTATE);
    p->u.array_start.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_start.mode = apr_pstrdup(p->pool, mode);
    if (y && *y == 't')
	p->u.array_start.nostandbyqd = 1;
    if (R && *R == 't')
	p->u.array_start.rotate_qd_log = 1;
    if (r && *r == 't')
	p->u.array_start.rotate_qes_log = 1;
    if (x && *x == 't')
	p->u.array_start.restricted = 1;

    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "start", OK_STATUS, ARRAY_STARTING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    if (uri)
	gpcc_net_uri_free(uri);
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

    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);
    if (strcmp(status, OK_STATUS) == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}
