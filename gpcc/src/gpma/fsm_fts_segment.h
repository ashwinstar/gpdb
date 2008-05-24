static apr_status_t a_fts_create_segment_slot(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *slot = _cmdslot;
    int i;
    cmdstate_t state = UNIT_CMDSTATE;

    if (gpma.unit_cmdtab) {
	gpcc_warning(FLINE, "unit cmdtab already exists");
	return 0;
    }

    gpma.unit_max = slot->u.fts.nseginfo;
    gpma.unit_cmdtab = apr_palloc(gpma.pool, (gpma.unit_max+1) * sizeof(gpma.unit_cmdtab[0]));
    
    if (slot->cmdstate == FTS_INITIALIZING_CMDSTATE)
	state = UNIT_STOPPED_CMDSTATE;
    
    for (i = 0 ; i < gpma.unit_max ; i++) {
	gpma.unit_cmdtab[i] = cmdslot_alloc(slot->u.fts.seginfo[i].name, state);
	gpma.unit_cmdtab[i]->u.unit.content = slot->u.fts.seginfo[i].content;
	gpma.unit_cmdtab[i]->u.unit.dbid = slot->u.fts.seginfo[i].dbid;
	gpma.unit_cmdtab[i]->u.unit.port = slot->u.fts.seginfo[i].port;
	gpma.unit_cmdtab[i]->u.unit.nodeid = slot->u.fts.seginfo[i].haidx;
	gpma.unit_cmdtab[i]->u.unit.node = apr_pstrdup(gpma.unit_cmdtab[i]->pool, slot->u.fts.seginfo[i].ha);
	gpma.unit_cmdtab[i]->u.unit.datadir = apr_pstrdup(gpma.unit_cmdtab[i]->pool, slot->u.fts.seginfo[i].dir);
	gpma.unit_cmdtab[i]->u.unit.host = apr_pstrdup(gpma.unit_cmdtab[i]->pool, slot->u.fts.seginfo[i].host);
    }

    return 0;
}

static apr_status_t a_fts_break(gpcc_event_t *ev, void *arg)
{
    int i;
    gpcc_event_t *evt;

    if (!gpma.unit_cmdtab)
	return 0;

    for (i = 0 ; i < gpma.unit_max ; i++) {
	evt = gpcc_event_alloc(BREAK_EVENT, gpma.unit_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

static apr_status_t a_fts_segment_start(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj = 0;
    cmdslot_t *p;
    gpcc_event_t *evt;
    gpcc_net_uri_t *uri = 0;
    gpcc_net_conn_t conn;
    char *mode, *url, *seg, *m, *errmsg;

    seg = url = 0;
    mode = "normal";
    errmsg = "";
    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_alloc(url);
    m = gpcc_net_uri_param(uri, "mode");
    seg = gpcc_net_uri_param(uri, "s");
    
    if (!seg) {
	errmsg = "parameter 's' is missing";
	goto bail;
    } else {
	int i;
	for (i = 0 ; i < slot->u.fts.nseginfo && strcmp(seg, slot->u.fts.seginfo[i].name) != 0 ; i++);
	if (i >= slot->u.fts.nseginfo) {
	    errmsg = "invalid parameter 's'";
	    goto bail;
	}
    }
    
    if (m) {
	char *opt[2] = {"normal", "force"};
	int i;
	for (i = 0 ; i < gpcc_lengthof(opt) && apr_strnatcasecmp(m, opt[i]) != 0 ; i++);
	if (i >= gpcc_lengthof(opt)) {
	    errmsg = "invalid parameter 'mode'";
	    goto bail;
	}
	mode = m;
    }

    p = cmdslot_alloc("SEGMENT_START", FTS_SEGMENT_START_CMDSTATE);
    p->u.segment_start.parent_cmdidx = slot->cmdidx;
    p->u.segment_start.nunit = 1;
    p->u.segment_start.unit = apr_palloc(p->pool, sizeof(char *) * p->u.segment_start.nunit);
    p->u.segment_start.unit[0] = apr_pstrdup(p->pool, seg);
    p->u.segment_start.mode = apr_pstrdup(p->pool, mode);
    slot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), seg,
			       "start", OK_STATUS, SEGMENT_STARTING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    
    return 0;

    bail:
    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    if (uri) gpcc_net_uri_free(uri);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), seg,
			       "start", FAIL_STATUS, 0, errmsg);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}


static apr_status_t a_fts_segment_stop(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj = 0;
    cmdslot_t *p;
    gpcc_event_t *evt;
    char *mode, *seg, *url, *m, *errmsg;
    gpcc_net_uri_t *uri = 0;
    gpcc_net_conn_t conn;

    errmsg = "";
    seg = url = 0;
    mode = "immediate";
    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    uri = gpcc_net_uri_alloc(url);
    m = gpcc_net_uri_param(uri, "mode");
    seg = gpcc_net_uri_param(uri, "s");

    if (!seg) {
	errmsg = "parameter 's' is missing";
	goto bail;
    } else {
	int i;
	for (i = 0 ; i < slot->u.fts.nseginfo && strcmp(seg, slot->u.fts.seginfo[i].name) != 0 ; i++);
	if (i >= slot->u.fts.nseginfo) {
	    errmsg = "invalid parameter 's'";
	    goto bail;
	}
    }
	    
	    
    if (m) {
	char *opt[3] = {"immediate", "fast", "smart"};
	int i;
	for (i = 0 ; i < gpcc_lengthof(opt) && apr_strnatcasecmp(m, opt[i]) != 0 ; i++) ;
	if (i >= gpcc_lengthof(opt)) {
	    /* invalid mode */
	    errmsg = "invalid parameter 'mode'";
	    goto bail;
	}
	
	mode = m;
    }

    gpcc_log_info("SEGMENT_STOP %s %s", seg, mode);

    p = cmdslot_alloc("SEGMENT_STOP", FTS_SEGMENT_STOP_CMDSTATE);
    p->u.segment_stop.parent_cmdidx = slot->cmdidx;
    p->u.segment_stop.nunit = 1;
    p->u.segment_stop.unit = apr_palloc(p->pool, sizeof(char *) * p->u.segment_stop.nunit);
    p->u.segment_stop.unit[0] = apr_pstrdup(p->pool, seg);
    p->u.segment_stop.mode = apr_pstrdup(p->pool, mode);
    slot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);


    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), seg,
			       "stop", OK_STATUS, SEGMENT_STOPPING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    
    return 0;


    bail:
    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    if (uri) gpcc_net_uri_free(uri);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), seg,
			       "stop", FAIL_STATUS, 0, errmsg);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;    
    
}

static apr_status_t a_fts_segment_command_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->u.fts.active_cmdidx = 0;

    return 0;
}
