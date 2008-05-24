
static apr_status_t a_fts_array_stop_forsure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot_t *p;
    gpcc_event_t *evt;

    p = cmdslot_alloc("ARRAY_STOP", FTS_ARRAY_STOP_CMDSTATE);
    p->u.array_stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_stop.mode = apr_pstrdup(p->pool, "immediate");
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}


static apr_status_t a_fts_array_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url, *mode = "immediate";
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;
    struct json_object *obj;
    gpcc_net_uri_t *uri;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    uri = gpcc_net_uri_alloc(url);
    if (uri) {
	char *m = gpcc_net_uri_param(uri, "mode");
	if (m) {
	    char *opt[3] = {"immediate", "fast", "smart"};
	    int i;
	    for (i = 0 ; i < gpcc_lengthof(opt) ; i++) {
		if (apr_strnatcasecmp(m, opt[i]) == 0)
		    break;
	    }
	    if (i >= gpcc_lengthof(opt)) {
		/* invalid mode */
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		gpcc_net_uri_free(uri);
		obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
					   "stop", FAIL_STATUS, 0, "parameter missing");
		gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
		json_object_put(obj);
		return 0;
	    }
	    
	    mode = m;
	}
    }

    p = cmdslot_alloc("ARRAY_STOP", FTS_ARRAY_STOP_CMDSTATE);
    p->u.array_stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_stop.mode = apr_pstrdup(p->pool, mode);
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
			       "stop", OK_STATUS, ARRAY_STOPPING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    if (uri)
	gpcc_net_uri_free(uri);

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

    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);
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
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;
    return 0;
}
