static inline void setup_set_segment(cmdslot_t *cmdslot, int idx, char *name, int dbid, int content, int port, int node, char *dir, apr_pool_t *pool)
{
    char key[128];
    const char *value, *s;
    char addr[128];

    cmdslot->u.fts.seginfo[idx].name = apr_pstrdup(pool, name);
    cmdslot->u.fts.seginfo[idx].dbid = dbid;
    cmdslot->u.fts.seginfo[idx].content = content;
    cmdslot->u.fts.seginfo[idx].port = port;
    cmdslot->u.fts.seginfo[idx].haidx = node;
    cmdslot->u.fts.seginfo[idx].ha = apr_psprintf(pool, "node.%d", node);
    cmdslot->u.fts.seginfo[idx].dir = apr_pstrdup(pool, dir);

    sprintf(key, "node.%d", node);
    value = config_get(gpma.cfg, key);
    s = strchr(value, ' ');
    if (!s) {
	strcpy(addr, value);
    } else {
	strncpy(addr, value, s - value);
	addr[s-value] = 0;
    }
    cmdslot->u.fts.seginfo[idx].host = apr_pstrdup(pool, addr);
}

static apr_status_t setup_cleanup_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;

    cmdslot->u.fts.nseginfo = 0;
    cmdslot->u.fts.seginfo = 0;
    return 0;
}

static apr_status_t setup_init_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int i, n, dbid, content, node;
    char key[128], dir[1024];
    const char *value;
    int master_port, base_port, mirror_base_port;

    master_port = config_get_or_set_int(gpma.cfg, CONFIG_MASTER_PORT_KEY, CONFIG_MASTER_PORT_VALUE);
    base_port = config_get_or_set_int(gpma.cfg, CONFIG_BASE_PORT_KEY, CONFIG_BASE_PORT_VALUE);
    mirror_base_port = config_get_or_set_int(gpma.cfg, CONFIG_MIRROR_BASE_PORT_KEY, CONFIG_MIRROR_BASE_PORT_VALUE);

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
    }
    
    if (i == 0) {
	gpcc_log_error("gpdb must have at least one master segment gpdb.seg.m.0");
	return APR_EINVAL;
    }
	

    n = i;

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
	
	n++;

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;

	n++;
    }	

    cmdslot->u.fts.nseginfo = n;
    cmdslot->u.fts.seginfo = apr_palloc(cmdslot->pool, sizeof(*cmdslot->u.fts.seginfo) * n);

    /* master QD */
    strcpy(key, "gpdb.seg.m.0");
    value = config_get(gpma.cfg, key);
    if (!value)
	return APR_EINVAL;

    dbid = 1;
    content = -1;
    if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	return APR_EINVAL;
    }	
    
    if (node >= cmdslot->u.fts.nnode) {
	gpcc_warning(FLINE, "segment %s value error.  node should be (%d < %d)", key, node, cmdslot->u.fts.nnode);
	return APR_EINVAL;
    }

    if (dir[0] != '/') {
	gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	return APR_EINVAL;
    }
	
    setup_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, cmdslot->pool);
    
    for (i = 0 ;  ; i++) {
	/* master QE */
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}	

	if (node >= cmdslot->u.fts.nnode) {
	    gpcc_warning(FLINE, "segment %s value error.  node should be (%d < %d)", key, node, cmdslot->u.fts.nnode);
	    return APR_EINVAL;
	}

	if (dir[0] != '/') {
	    gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	    return APR_EINVAL;
	}
	
	dbid++;
	content++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, base_port++, node, dir, cmdslot->pool);

	/* mirror QE */
	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}

	if (node >= cmdslot->u.fts.nnode) {
	    gpcc_warning(FLINE, "segment %s value error.  node should be (%d < %d)", key, node, cmdslot->u.fts.nnode);
	    return APR_EINVAL;
	}

	if (dir[0] != '/') {
	    gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	    return APR_EINVAL;
	}

	dbid++;	
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, mirror_base_port++, node, dir, cmdslot->pool);
    }

    /* standby QD */
    content = -1;
    for (i = 1 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}

	if (node >= cmdslot->u.fts.nnode) {
	    gpcc_warning(FLINE, "segment %s value error.  node should be (%d < %d)", key, node, cmdslot->u.fts.nnode);
	    return APR_EINVAL;
	}

	if (dir[0] != '/') {
	    gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	    return APR_EINVAL;
	}

	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, cmdslot->pool);
    }	

    return 0;
}



static apr_status_t a_fts_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i;
    char key[20];
    const char *v;

    for (i = 0 ;  ; i++) {
	sprintf(key, "node.%d", i);
	v = config_get(gpma.cfg, key);
	if (!v)
	    break;
    }
    cmdslot->u.fts.nnode = i;
    cmdslot->u.fts.nopened = 0;

    if (config_get(gpma.cfg, "gpdb.name")) {
	if (setup_init_seginfo(ev, _cmdslot))
	    gpcc_fatal(FLINE, "seginfo setup error.");
    }

    for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	gpcc_log_info("name=%s, host =%s, dbid = %d, content =%d, port = %d, dir = %s, haidx = %d, ha=%s",
		      cmdslot->u.fts.seginfo[i].name,
		      cmdslot->u.fts.seginfo[i].host,
		      cmdslot->u.fts.seginfo[i].dbid,
		      cmdslot->u.fts.seginfo[i].content,
		      cmdslot->u.fts.seginfo[i].port,
		      cmdslot->u.fts.seginfo[i].dir,
		      cmdslot->u.fts.seginfo[i].haidx,
		      cmdslot->u.fts.seginfo[i].ha);
    }

    return 0;
}

static apr_status_t a_fts_node_opened(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;

    cmdslot->u.fts.nopened++;
    if (cmdslot->u.fts.nopened == cmdslot->u.fts.nnode && cmdslot->cmdstate == FTS_WAIT_CMDSTATE){
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

static apr_status_t a_fts_node_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    
    cmdslot->u.fts.nopened--;
    return 0;
}

static apr_status_t a_fts_array_start(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url, *mode = "normal";
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
	    char *opt[2] = {"normal", "force"};
	    int i;
	    for (i = 0 ; i < gpcc_lengthof(opt) ; i++) {
		if (apr_strnatcasecmp(m, opt[i]) == 0)
		    break;
	    }
	    if (i >= gpcc_lengthof(opt)) {
		evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		return 0;
	    }
	    mode = m;
	}
    }

    p = cmdslot_alloc("ARRAY_START", FTS_ARRAY_START_CMDSTATE);
    p->u.array_start.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_start.mode = apr_pstrdup(p->pool, mode);
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply("start", "pending", "pending");
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

    status = gpcc_json_object_get_string(obj, "status", "failure");
    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_fts_array_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *url;
    char *state;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    /* send the current state */
    if (cmdslot->cmdstate == FTS_WAIT_CMDSTATE) {
	state = "connecting";
    } else if (cmdslot->cmdstate == FTS_EMPTY_CMDSTATE) {
	state = "none";
    } else if (cmdslot->cmdstate == FTS_INITIALIZING_CMDSTATE) {
	state = "initializing";
    } else if (cmdslot->cmdstate == FTS_STOPPED_CMDSTATE) {
	state = "stopped";
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	state = "starting";
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	state = "running";
    } else if (cmdslot->cmdstate ==  FTS_STOPPING_CMDSTATE) {
	state = "stopping";
    } else {
	state = "invalid state";
    }

    obj = gpcc_json_http_status_reply(config_get(gpma.cfg, "gpdb.name"), "ok", state);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_busy_ok_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *msg;

    if (cmdslot->cmdstate ==  FTS_CMDSTATE) {
	msg = "server is waking up";
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {	
	msg = "server is connecting";
    } else if (cmdslot->cmdstate ==  FTS_EMPTY_CMDSTATE) {	
	msg = "array is absent";
    } else if (cmdslot->cmdstate ==  FTS_INITIALIZING_CMDSTATE) {	
	msg = "server is initializing";
    } else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = "server stopped";
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = "server is starting";
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = "server is running";
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = "server is stopping";
    } else {
	msg = "invalid server state";
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply("", "ok", msg);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_busy_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *msg;

    if (cmdslot->cmdstate ==  FTS_CMDSTATE) {
	msg = "server is initializing";
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {
	msg = "server is setting up"; 
    }
    else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = "server is not running";
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = "server is starting";
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = "server is running";
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = "server is stopping";
    } else {
	msg = "invalid server state";
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply("", "failure", msg);
    gpcc_net_http_close(conn, 200, "failure", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_running(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;

    return 0;
}

static apr_status_t a_fts_array_stop_forsure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot_t *p;
    gpcc_event_t *evt;

    p = cmdslot_alloc("ARRAY_STOP", FTS_ARRAY_STOP_CMDSTATE);
    p->u.array_stop.parent_cmdidx = cmdslot->cmdidx;
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

    obj = gpcc_json_http_reply("stop", "pending", "pending");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

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

    status = gpcc_json_object_get_string(obj, "status", "failure");
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

static apr_status_t a_fts_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;
    return 0;
}

static apr_status_t a_fts_array_config_verify() {
    int i, n;
    const char *v;
    int errcnt = 0;
    char buf[1024];
    int nnode = 0;
    char dir[1024];

    /* node.X */
    for (i = 0 ; ; i++) {
	sprintf(buf, "node.%d", i);
	if (config_get(gpma.cfg, buf) == 0) 
	    break;
    }
    nnode = i;
    if (nnode == 0) {
	gpcc_log_error("\t- node.0 missing");
	errcnt++;
    }

    /* name must exist */
    if (! config_get(gpma.cfg, "gpdb.name")) {
	gpcc_log_error("\t- gpdb.name missing");
	errcnt++;
    }
	
    /* default master_port port if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.master_port")) {
	gpcc_log_error("\t- gpdb.seg.master_port missing");
	errcnt++;
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.master_port");
    if (! (0 < i && i < (1 << 16))) {
	gpcc_log_error("\t- gpdb.seg.master_port invalid (value %d)", i);
	errcnt++;
    }
    
    /* default qe baseport if not set */
    if (! config_get(gpma.cfg, "gpdb.seg.base_port")) {
	gpcc_log_error("\t- gpdb.seg.base_port missing");
	errcnt++;
    }
    i = config_get_int(gpma.cfg, "gpdb.seg.base_port");
    if (! (0 < i && i < (1 << 16))) {
	gpcc_log_error("\t- gpdb.seg.base_port invalid (value %d)", i);
	errcnt++;
    }
    
    /* default min mirror if not set */
    i = config_get_or_set_int(gpma.cfg, "gpdb.min_mirror", 0);
    if (! (0 <= i && i < 2)) {
	gpcc_log_error("\t- gpdb.min_mirror invalid (value %d)", i);
	errcnt++;
    }
    
    /* qd must exist */
    v = config_get(gpma.cfg, "gpdb.seg.m.0");
    if (!v) {
	gpcc_log_error("\t- gpdb.seg.m.0 not set");
	errcnt++;
    } else {
    
	if (sscanf(v, "node.%d:%s", &n, dir) == 2) {
	    
	    if (n >= nnode) {
		gpcc_log_error("\t- segment %s value error.  node should be (%d < %d)", "gpdb.seg.m.0", n, nnode);
		errcnt++;
	    }
	    
	    if (dir[0] != '/') {
		gpcc_log_error("\t- segment %s data directory %s must be in full path", "gpdb.seg.m.0", dir);
		errcnt++;
	    }
	} else {
	    gpcc_log_error("\t- %s value syntax error", v);
	    errcnt++;
	}
    }

    /* minimum 2 QEs, max 512 QEs */
    for (i = 0; i < 512; i++) {
	sprintf(buf, "gpdb.seg.%d.0", i);
	v = config_get(gpma.cfg, buf);
	if (!v) {
	    if (i < 2) {
		gpcc_log_error("\t- %s not set", buf);
		errcnt++;
	    }
	    break;
	} else {

	    if (sscanf(v, "node.%d:%s", &n, dir) == 2) {
		
		if (n >= nnode) {
		    gpcc_log_error("\t- segment %s value error.  node should be (%d < %d)", buf, n, nnode);
		    errcnt++;
		}
		
		if (dir[0] != '/') {
		    gpcc_log_error("\t- segment %s data directory %s must be in full path", buf, dir);
		    errcnt++;
		}
	    } else {
		gpcc_log_error("\t- %s value syntax error", v);
		errcnt++;
	    }
	}
    }
    if (! (2 <= i && i < 512)) {
	gpcc_log_error("\t- too many segments (max 512)");
	errcnt++;
    }
    
    /* fail out */
    if (errcnt) {
	return errcnt;
    }

    /* set the number of QEs */
    config_set_int(gpma.cfg, "gpdb.seg.max", i);

    return 0;

}

static apr_status_t a_fts_array_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *config;
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;
    struct json_object *obj;
    int i;
    int len = ev->edatasz;
    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    len -= sizeof(gpcc_net_conn_t);
    config = ev->pdata + sizeof(gpcc_net_conn_t);
    
    if (config_parse(gpma.cfg, config, len) || a_fts_array_config_verify()) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply("init", "failure", "config syntax error");
	gpcc_net_http_close(conn, 200, "failure", json_object_to_json_string(obj));
	json_object_put(obj);	
	return 0;
    }

    if (setup_init_seginfo(ev, _cmdslot))
	gpcc_fatal(FLINE, "seginfo setup error.");
 
    for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	gpcc_log_info("name=%s, host =%s, dbid = %d, content =%d, port = %d, dir = %s, haidx = %d, ha=%s",
		      cmdslot->u.fts.seginfo[i].name,
		      cmdslot->u.fts.seginfo[i].host,
		      cmdslot->u.fts.seginfo[i].dbid,
		      cmdslot->u.fts.seginfo[i].content,
		      cmdslot->u.fts.seginfo[i].port,
		      cmdslot->u.fts.seginfo[i].dir,
		      cmdslot->u.fts.seginfo[i].haidx,
		      cmdslot->u.fts.seginfo[i].ha);
    }
   
    p = cmdslot_alloc("ARRAY_INIT", FTS_ARRAY_INIT_CMDSTATE);
    p->u.array_init.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_init.conn = conn;
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);


    obj = gpcc_json_http_reply("init", "pending", "pending");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

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

    cmdslot->u.fts.active_cmdidx = 0;
    config_write(gpma.datadir, gpma.guid, gpma.cfg);
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
	if (strstr(keys[i], "gpdb.") == keys[i]) {
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

    if (config_get(gpma.cfg, "gpdb.name") == 0) {
	evt = gpcc_event_alloc(ARRAY_ABSENT_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(ARRAY_PRESENT_EVENT, cmdslot->cmdidx, 0);
    }

    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}
