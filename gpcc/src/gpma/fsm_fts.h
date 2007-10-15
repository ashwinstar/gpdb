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
	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, cmdslot->pool);
    }	

    return 0;
}



static apr_status_t a_fts_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i;
    setup_init_seginfo(ev, _cmdslot);

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


static apr_status_t a_fts_array_start(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    p = cmdslot_alloc("ARRAY_START", FTS_ARRAY_START_CMDSTATE);
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
    char *url;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply("", "failure", "state conflict");
    gpcc_net_http_close(conn, 409, "STATE CONFLICT", json_object_to_json_string(obj));
    return 0;
}

static apr_status_t a_fts_running(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;

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
    
    p = cmdslot_alloc("ARRAY_STOP", FTS_ARRAY_STOP_CMDSTATE);
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
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;
    return 0;
}

static apr_status_t a_fts_failure(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}


static apr_status_t a_fts_array_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    p = cmdslot_alloc("ARRAY_INIT", FTS_ARRAY_INIT_CMDSTATE);
    p->u.array_init.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_init.conn = conn;
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

static apr_status_t a_fts_initialized(gpcc_event_t *ev, void *_cmdslot)
{
    return 0;
}
