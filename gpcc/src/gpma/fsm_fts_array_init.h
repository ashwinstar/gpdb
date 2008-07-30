
static apr_status_t a_fts_array_config_verify() {
    int i;
    const char *v;
    int errcnt = 0;
    char buf[1024];
    char host[1024];
    char dir[1024];
    int port, nprimary, nmirror;

    nprimary = nmirror = 0;

    /* name must exist */
    if (! config_get(gpma.cfg, CONFIG_DBNAME_KEY)) {
	gpcc_log_error("\t- %s missing", CONFIG_DBNAME_KEY);
	errcnt++;
    }
	
    /* qd must exist */
    v = config_get(gpma.cfg, "seg.m.0");
    if (!v) {
	gpcc_log_error("\t- seg.m.0 not set");
	errcnt++;
    } else {
	if (setup_parse_segment("seg.m.0", v, host, dir, &port, 0, 0)) {
	    gpcc_log_error("\t- %s value syntax error.", v);
	    errcnt++;
	}
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "seg.%d.0", i);
	v = config_get(gpma.cfg, buf);
	if (!v) {
	    if (i < 2) {
		gpcc_log_error("\t- %s not set", buf);
		errcnt++;
	    }
	    break;
	} else {

	    if (setup_parse_segment(buf, v, host, dir, &port, 0, 0)) {
		gpcc_log_error("\t- %s value syntax error.", v);
		errcnt++;
	    }
	}
    }
    nprimary = i;

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "seg.%d.1", i);
	v = config_get(gpma.cfg, buf);
	if (!v) {
	    if (i < 2) {
		gpcc_log_error("\t- %s not set", buf);
		errcnt++;
	    }
	    break;
	} else {

	    if (setup_parse_segment(buf, v, host, dir, &port, 0, 0)) {
		gpcc_log_error("\t- %s value syntax error.", v);
		errcnt++;
	    }
	}
    }    
    nmirror = i;

    if (! (2 <= i && i < 512)) {
	gpcc_log_error("\t- too many segments (max 512)");
	errcnt++;
    }

    if (nprimary != nmirror) {
	gpcc_log_error("\t- number of primary (%d) != number of mirror (%d)", nprimary, nmirror);
	errcnt++;
    }
    
    /* fail out */
    if (errcnt) {
	return errcnt;
    }

    return 0;

}

static apr_status_t a_fts_array_init_start_nodes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *config;
    gpcc_net_conn_t conn;
    gpcc_event_t *evt;
    struct json_object *obj;
    int len = ev->edatasz;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    len -= sizeof(gpcc_net_conn_t);
    config = ev->pdata + sizeof(gpcc_net_conn_t);
   
    if (config_parse(gpma.cfg, config, len) || a_fts_array_config_verify()) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   "init", FAIL_STATUS, 0, "config syntax error");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);	
	return 0;
    }

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "init", OK_STATUS, ARRAY_INITIALIZING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    evt = gpcc_event_alloc(ARRAY_PRESENT_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
   
    return 0;
}

static apr_status_t a_fts_array_init_start_nodes_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;
    int i;

    /* shutdown nodes */
    for (i = 0 ; i < gpma.node_max ; i++) {
	evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    /* shutdown haset */
    evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.haset_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    gpma.node_max = 0;
    gpma.haset_cmdslot = 0;
    gpma.node_cmdtab = 0;
    cmdslot->u.fts.nnode = 0;
    cmdslot->u.fts.nopened = 0;

    return 0;
}

static apr_status_t a_fts_array_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot_t *p;
    gpcc_event_t *evt;

    cmdslot->countdown = 0;
    p = cmdslot_alloc("ARRAY_INIT", FTS_ARRAY_INIT_CMDSTATE);
    p->u.array_init.parent_cmdidx = cmdslot->cmdidx;
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);


    return 0;
}

static apr_status_t a_fts_array_init_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    obj = json_tokener_parse(pkt->payload);

    status = gpcc_json_object_get_string(obj, "status", "failure");
    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_fts_array_init_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;

    cmdslot->u.fts.active_cmdidx = 0;
    evt = gpcc_event_alloc(CFG_UPDATED_EVENT, gpma.cfg_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_fts_array_init_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    const char **keys;
    int i;

    cmdslot->u.fts.active_cmdidx = 0;

    keys = config_keys(gpma.cfg, cmdslot->pool);
    for (i = 0 ; keys[i] ; i++) {
	if (strstr(keys[i], "seg.") == keys[i] || 
	    strstr(keys[i], "node.") == keys[i] ||
	    strcmp(keys[i], CONFIG_DBNAME_KEY) == 0) {
	    config_set(gpma.cfg, keys[i], 0);
	}
    }

    config_gc(gpma.cfg);
    setup_cleanup_seginfo(ev, _cmdslot);
    apr_pool_clear(cmdslot->pool);
    return 0;
}

static apr_status_t a_fts_array_present(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;

    cmdslot->countdown = 0;
    if (config_get(gpma.cfg, CONFIG_DBNAME_KEY) == 0) {
	evt = gpcc_event_alloc(ARRAY_ABSENT_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(ARRAY_PRESENT_EVENT, cmdslot->cmdidx, 0);
    }

    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}
