
#define SYSTEM_INITIALIZING_STATUS                   "system initializing"

#define ARRAY_STOPPING_STATUS                        "array stopping"
#define ARRAY_STOPPED_STATUS                         "array stopped"
#define ARRAY_STARTING_STATUS                        "array starting"
#define ARRAY_RUNNING_STATUS                         "array running"
#define ARRAY_DELETING_STATUS                        "array deleting"
#define ARRAY_INITIALIZING_STATUS                    "array initializing"
#define ARRAY_INVALID_STATUS                         "array invalid"
#define ARRAY_NOT_INITIALIZED_STATUS                 "array not initialized"
#define ARRAY_CONNECTING_STATUS                      "array connecting"
#define ARRAY_BUSY_STATUS                            "array busy"

#define SEGMENT_NOT_INITIAILZED_STATUS               "segment not initialized"
#define SEGMENT_STOPPED_STATUS                       "segment stopped"
#define SEGMENT_INITIALIZING_STATUS                  "segment initializing"
#define SEGMENT_STARTING_STATUS                      "segment starting"
#define SEGMENT_STOPPING_STATUS                      "segment stopping"
#define SEGMENT_RUNNING_STATUS                       "segment running"
#define SEGMENT_DELETING_STATUS                      "segment deleting"
#define SEGMENT_INVALID_STATUS                       "segment invalid"
#define SEGMENT_CONNECTING_STATUS                    "segment connecting"
#define SEGMENT_BUSY_STATUS                          "segment busy"

#define OK_STATUS                                    "ok"
#define FAIL_STATUS                                  "error"

static inline void setup_set_segment(cmdslot_t *cmdslot, int idx, char *name, int dbid, int content, int port,
				     char *host, char *dir, char definedprimary, char isprimary, char valid, apr_pool_t *pool)
{
    cmdslot->u.fts.seginfo[idx].name = apr_pstrdup(pool, name);
    cmdslot->u.fts.seginfo[idx].dbid = dbid;
    cmdslot->u.fts.seginfo[idx].content = content;
    cmdslot->u.fts.seginfo[idx].port = port;
    cmdslot->u.fts.seginfo[idx].dir = apr_pstrdup(pool, dir);
    cmdslot->u.fts.seginfo[idx].definedprimary = definedprimary;
    cmdslot->u.fts.seginfo[idx].isprimary = isprimary;
    cmdslot->u.fts.seginfo[idx].valid = valid;
    cmdslot->u.fts.seginfo[idx].host = apr_pstrdup(pool, host);
}

static apr_status_t setup_cleanup_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;

    cmdslot->u.fts.nseginfo = 0;
    cmdslot->u.fts.seginfo = 0;
    cmdslot->u.fts.ht = 0;
    return 0;
}


static apr_status_t setup_parse_segment(const char *key, const char *value, char *host, char *dir, int *port)
{
    char *s1, *s2;
    char portstr[256];

    if (!value)
	return APR_EINVAL;

    s1 = strchr(value, ':');
    if (!s1) {
	gpcc_log_error("value format error. %s = %s", key, value);
	return APR_EINVAL;
    }
    s2 = strchr(s1+1, ':');
    if (!s2) {
	gpcc_log_error("value format error. %s = %s", key, value);
	return APR_EINVAL;
    }

    strncpy(host, value, s1-value);
    host[s1-value] = 0;
    strncpy(portstr, s1+1, s2-s1);
    portstr[s2-s1-1] = 0;
    *port = atoi(portstr);
    strcpy(dir, s2+1);
    
    if (dir[0] != '/') {
	gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	return APR_EINVAL;
    }

    return APR_SUCCESS;
}

static apr_status_t setup_init_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int i, j, n, dbid, content, port;
    char key[128], dir[256], host[256];
    const char *value;
    char *ptr;

    /* build the segments */
    for (i = 0 ; ; i++) {
	sprintf(key, "seg.m.%d", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
    }
    
    if (i == 0) {
	gpcc_log_error("gpdb must have at least one master segment seg.m.0");
	return APR_EINVAL;
    }
	

    n = i;
    for (i = 0 ; ; i++) {
	sprintf(key, "seg.%d.0", i);
	content = i;
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;

	n++;

	sprintf(key, "seg.%d.1", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;
	n++;
    }

    cmdslot->u.fts.nseginfo = n;
    cmdslot->u.fts.seginfo = apr_palloc(cmdslot->pool, sizeof(*cmdslot->u.fts.seginfo) * n);

    
    /* master QD */
    strcpy(key, "seg.m.0");
    value = config_get(gpma.cfg, key);
    if (!value)
	return APR_EINVAL;

    if (setup_parse_segment(key, value, host, dir, &port)) {
	return APR_EINVAL;
    }

    dbid = 1;
    content = -1;

    setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
		      host, dir, 't', 't', 't', cmdslot->pool);

    
    for (i = 0 ; ; i++) {
	/* primary QE */
	content = i;

	sprintf(key, "seg.%d.0", content);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
	if (setup_parse_segment(key, value, host, dir, &port)) {
	    return APR_EINVAL;
	}

	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 't', 't', 't', cmdslot->pool);

	/* mirror QE */
	sprintf(key, "seg.%d.1", content);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;
	if (setup_parse_segment(key, value, host, dir, &port)) {
	    return APR_EINVAL;
	}

	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 'f', 'f', 't', cmdslot->pool);
    }


    /* standby QD */
    strcpy(key, "seg.m.1");
    value = config_get(gpma.cfg, key);
    if (value) {
	if (setup_parse_segment(key, value, host, dir, &port)) {
	    return APR_EINVAL;
	}
	dbid++;
	content = -1;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 'f', 'f', 't', cmdslot->pool);
    }


    /* get the hostname */
    cmdslot->u.fts.ht = apr_hash_make(cmdslot->pool);

    for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	char hostname[256], ipaddr[1024];
	int len;

	/* get hostname */
	len = sizeof(hostname);
	if (gpcc_pg_gethostname(cmdslot->u.fts.seginfo[i].host, hostname, &len, cmdslot->pool)) {
	    gpcc_log_error("hostname %s error", cmdslot->u.fts.seginfo[i]);
	    return APR_EINVAL;
	}
	    
	ptr = gpcc_string_trim(hostname);
	
	cmdslot->u.fts.seginfo[i].hostname = apr_pstrdup(cmdslot->pool, ptr);
	/* check hashable with key = gethostname */
	if (apr_hash_get(cmdslot->u.fts.ht, ptr, APR_HASH_KEY_STRING) == 0) {
	    /* get ip addresses of the gethostname */
	    len = sizeof(ipaddr);
	    if (gpcc_pg_get_addresses(ptr, ipaddr, &len, cmdslot->pool)) {
		gpcc_log_error("get IP address from remote host %s error", hostname);
		return APR_EINVAL;
	    }

	    /* save it in hashtable */
	    ptr = gpcc_string_trim(ipaddr);
	    apr_hash_set(cmdslot->u.fts.ht, 
			 apr_pstrdup(cmdslot->pool, cmdslot->u.fts.seginfo[i].hostname),
			 APR_HASH_KEY_STRING,
			 apr_pstrdup(cmdslot->pool, ptr));

	}
    }

    {
	apr_hash_index_t *hi;
	void *val;
	const void *key;

	for (i = 0, hi = apr_hash_first(cmdslot->pool, cmdslot->u.fts.ht) ; 
	     hi ; 
	     hi = apr_hash_next(hi), i++) {
	    char node[20];

	    sprintf(node, "node.%d", i);
	    apr_hash_this(hi, &key, 0, &val);
	    config_set(gpma.cfg, node, (const char *) val);

	    for (j = 0 ; j < cmdslot->u.fts.nseginfo ; j++) {
		if (strcmp((const char *) key, cmdslot->u.fts.seginfo[j].hostname) == 0) { 
		    cmdslot->u.fts.seginfo[j].haidx = i;
		    cmdslot->u.fts.seginfo[j].ha = apr_psprintf(cmdslot->pool, "node.%d", i);
		}
	    }
	}

	/* get unique host as node */
	cmdslot->u.fts.nnode = i;
	cmdslot->u.fts.nopened = 0;
    }


    return 0;
}


static apr_status_t a_fts_http_redirect_leadma(gpcc_net_conn_t conn, const char *url)
{
    char location[128], json[128];
    int ma_id = 0;

    if (*gpma.lma_cmdslot->u.lma.leadma) {
	if (sscanf(gpma.lma_cmdslot->u.lma.leadma, "ma.%d", &ma_id) != 1 || ma_id >= gpma.ma_max)
	    gpcc_fatal(FLINE, "Invalid ma name %s", gpma.lma_cmdslot->u.lma.leadma);
	sprintf(location, "http://%s%s", gpma.ma_addr[ma_id], url);
	sprintf(json, "{ \"location\" : \"%s\"}", gpma.ma_addr[ma_id]);
	gpcc_net_http_redirect_close(conn, location, json);
	return 0;
    } 
    return 0;
}


void a_fts_node_init(void)
{
    char node_name[100];
    const char* node;
    int cnt, i;
    cmdslot_t* cp;
    gpcc_event_t *ev;

    for (cnt = 0; ; cnt++) {
	sprintf(node_name, "node.%d", cnt);
	node = config_get(gpma.cfg, node_name);
	if (!node) 
	    break;
    }

    if (cnt > 0) {
	gpma.node_max = cnt;
	gpma.node_cmdtab = apr_palloc(gpma.pool, (cnt + 1) * sizeof(gpma.node_cmdtab[0]));
	memset(gpma.node_cmdtab, 0, (cnt + 1) * sizeof(gpma.node_cmdtab[0]));
	for (i = 0; i < cnt; i++) {
	    sprintf(node_name, "node.%d", i);
	    node = config_get(gpma.cfg, node_name);
	    cp = gpma.node_cmdtab[i] = cmdslot_alloc(node_name, NODE_CMDSTATE);
	    if (apr_tokenize_to_argv(node, &cp->u.node.hosttab, cp->pool)) {
		gpcc_fatal(FLINE, "Illegal format for entry '%s' in configuration", node_name);
	    }
	    for (cp->u.node.hosttab_max = 0 ; 
		 cp->u.node.hosttab[cp->u.node.hosttab_max] ; 
		 cp->u.node.hosttab_max++) ;
	    cp->u.node.hostidx = 0;
	}
    }


    /* kick off haset */
    gpma.haset_cmdslot = cmdslot_alloc("HASET", HASET_OFF_CMDSTATE);
    gpma.haset_cmdslot->u.haset.total = cnt;

    ev = gpcc_event_alloc(INIT_EVENT, gpma.haset_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, ev);


    /* kick off node */
    for (i = 0 ; i < gpma.node_max ; i++) {
	ev = gpcc_event_alloc(INIT_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, ev);
    }
}


static apr_status_t a_fts_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i;
    gpcc_event_t *evt;

    if (config_get(gpma.cfg, CONFIG_DBNAME_KEY)) {
	
	if (setup_init_seginfo(ev, _cmdslot))
	    gpcc_fatal(FLINE, "seginfo setup error.");


	for (i = 0 ; i < cmdslot->u.fts.nseginfo ; i++) {
	    gpcc_log_info("name=%s, host =%s, hostname = %s, dbid = %d, content =%d, port = %d, dir = %s, haidx = %d, ha=%s",
			  cmdslot->u.fts.seginfo[i].name,
			  cmdslot->u.fts.seginfo[i].host,
			  cmdslot->u.fts.seginfo[i].hostname,
			  cmdslot->u.fts.seginfo[i].dbid,
			  cmdslot->u.fts.seginfo[i].content,
			  cmdslot->u.fts.seginfo[i].port,
			  cmdslot->u.fts.seginfo[i].dir,
			  cmdslot->u.fts.seginfo[i].haidx,
			  cmdslot->u.fts.seginfo[i].ha);
	}


	/* kick off the node */
	a_fts_node_init();
    } else {
	evt = gpcc_event_alloc(ARRAY_ABSENT_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }


    return 0;
}


static apr_status_t a_fts_waiting(gpcc_event_t *ev, void *_cmdslot)
{
    char *url, *cmd = "";
    gpcc_net_conn_t conn;
    struct json_object *obj;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    switch (ev->etype)	{ 
    case ARRAY_INIT_EVENT:
	url = "/gpcc/init";
	cmd = "init";
	break;
    case ARRAY_STOP_EVENT:
	cmd = "stop";
	break;
    case ARRAY_START_EVENT:
	cmd = "start";
	break;
    case ARRAY_DELETE_EVENT:
	cmd = "delete";
	break;
    default:
	break;
    }
	
    if (IS_NO_LEADMA) {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   cmd, FAIL_STATUS, 0, "Lead MA is not determined");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    } else if (!IS_LEADMA) {
	a_fts_http_redirect_leadma(conn, url);
    } else {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   cmd, FAIL_STATUS, 0, SYSTEM_INITIALIZING_STATUS);
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    }
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
		gpcc_net_uri_free(uri);
		obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
					   "start", FAIL_STATUS, 0, "parameter missing");
		gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
		json_object_put(obj);
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

static apr_status_t a_fts_array_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *url;
    char *state;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    if (IS_NO_LEADMA) {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
				   "status", OK_STATUS, SYSTEM_INITIALIZING_STATUS, 0);
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    } else if (!IS_LEADMA) {
	a_fts_http_redirect_leadma(conn, url);
	return 0;
    }

    /* send the current state */
    switch (cmdslot->cmdstate) {
    case FTS_CMDSTATE:
    case FTS_WAIT_CMDSTATE:
	state = ARRAY_CONNECTING_STATUS;
	break;
    case FTS_EMPTY_CMDSTATE:
	state = ARRAY_NOT_INITIALIZED_STATUS;
	break;
    case FTS_INITIALIZING_CMDSTATE:
    case FTS_INITIALIZING0_CMDSTATE:
	state = ARRAY_INITIALIZING_STATUS;
	break;
    case FTS_STOPPED_CMDSTATE:
	state = ARRAY_STOPPED_STATUS;
	break;
    case FTS_STARTING_CMDSTATE:
	state = ARRAY_STARTING_STATUS;
	break;
    case FTS_RUNNING_CMDSTATE:
	state = ARRAY_RUNNING_STATUS;
	break;
    case FTS_STOPPING_CMDSTATE:
	state = ARRAY_STOPPING_STATUS;
	break;
    case FTS_DELETING_CMDSTATE:
	state = ARRAY_DELETING_STATUS;
	break;
    default:
	state = ARRAY_INVALID_STATUS;
	break;
    }

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
			       "status", OK_STATUS, state, 0);
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
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {	
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_EMPTY_CMDSTATE) {	
	msg = ARRAY_NOT_INITIALIZED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_INITIALIZING_CMDSTATE) {	
	msg = ARRAY_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = ARRAY_STOPPED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = ARRAY_STARTING_STATUS;
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = ARRAY_RUNNING_STATUS;
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = ARRAY_STOPPING_STATUS;
    } else if (cmdslot->cmdstate == FTS_DELETING_CMDSTATE) {
	msg = ARRAY_DELETING_STATUS;
    } else {
	msg = ARRAY_INVALID_STATUS;
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "", OK_STATUS, msg, 0);
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
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {	
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_EMPTY_CMDSTATE) {	
	msg = ARRAY_NOT_INITIALIZED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_INITIALIZING_CMDSTATE) {	
	msg = ARRAY_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = ARRAY_STOPPED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = ARRAY_STARTING_STATUS;
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = ARRAY_RUNNING_STATUS;
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = ARRAY_STOPPING_STATUS;
    } else if (cmdslot->cmdstate == FTS_DELETING_CMDSTATE) {
	msg = ARRAY_DELETING_STATUS;
    } else {
	msg = ARRAY_INVALID_STATUS;
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
			       "", FAIL_STATUS, 0, msg);
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

static apr_status_t a_fts_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;
    return 0;
}

static apr_status_t a_fts_array_config_verify() {
    int i;
    const char *v;
    int errcnt = 0;
    char buf[1024];
    char host[1024];
    char dir[1024];
    int port;

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
	if (setup_parse_segment("seg.m.0", v, host, dir, &port)) {
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

	    if (setup_parse_segment(buf, v, host, dir, &port)) {
		gpcc_log_error("\t- %s value syntax error.", v);
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

    return 0;

}

static apr_status_t a_fts_array_init_start_nodes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *config;
    gpcc_net_conn_t conn;
    gpcc_event_t *evt;
    struct json_object *obj;
    int i;
    int len = ev->edatasz;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    len -= sizeof(gpcc_net_conn_t);
    config = ev->pdata + sizeof(gpcc_net_conn_t);
   
    if (gpma.maset_cmdslot->u.maset.nopened < gpma.maset_cmdslot->u.maset.total) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   "init", FAIL_STATUS, 0, "gp init can only be run when all MA's are active");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);	
	return 0;
    }

    if (config_parse(gpma.cfg, config, len) || a_fts_array_config_verify()) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   "init", FAIL_STATUS, 0, "config syntax error");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
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

    a_fts_node_init();

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "init", OK_STATUS, ARRAY_INITIALIZING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    /* check for connection timeout */
    cmdslot->countdown = 30;

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

    if (config_get(gpma.cfg, CONFIG_DBNAME_KEY) == 0) {
	evt = gpcc_event_alloc(ARRAY_ABSENT_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(ARRAY_PRESENT_EVENT, cmdslot->cmdidx, 0);
    }

    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

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

/* gp delete */
static apr_status_t a_fts_array_delete(gpcc_event_t *ev, void *_cmdslot)
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
    
    if (gpma.maset_cmdslot->u.maset.nopened < gpma.maset_cmdslot->u.maset.total) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   "delete", FAIL_STATUS, 0,
				   "gp delete can only be run when all MA's are active");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);	
	return 0;
    }


    uri = gpcc_net_uri_alloc(url);

    p = cmdslot_alloc("ARRAY_DELETE", FTS_ARRAY_DELETE_CMDSTATE);
    p->u.array_delete.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_delete.mode = apr_pstrdup(p->pool, mode);
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "delete", OK_STATUS, ARRAY_DELETING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    if (uri)
	gpcc_net_uri_free(uri);

    return 0;
}

static apr_status_t a_fts_array_delete_reply(gpcc_event_t *ev, void *_cmdslot)
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


static apr_status_t a_fts_deleted(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    const char **keys;
    int i;
    gpcc_event_t *evt;

    cmdslot->u.fts.active_cmdidx = 0;

    keys = config_keys(gpma.cfg, cmdslot->pool);
    for (i = 0 ; keys[i] ; i++) {
	if (strstr(keys[i], "seg.") == keys[i] || 
	    strstr(keys[i], "node.") == keys[i] ||
	    strcmp(keys[i], CONFIG_DBNAME_KEY) == 0) {
	    config_set(gpma.cfg, keys[i], 0);
	}
    }

    evt = gpcc_event_alloc(CFG_UPDATED_EVENT, gpma.cfg_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    setup_cleanup_seginfo(ev, _cmdslot);
    apr_pool_clear(cmdslot->pool);
    return 0;
}

static apr_status_t a_fts_array_delete_shutdown_nodes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;
    int i;

    /* shutdown nodes */
    for (i = 0 ; i < gpma.node_max ; i++) {
	evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    /* shutdown unit */
    for (i = 0 ; i < gpma.unit_max ; i++) {
	evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.unit_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    /* shutdown haset */
    evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.haset_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    gpma.node_max = 0;
    gpma.haset_cmdslot = 0;
    gpma.node_cmdtab = 0;
    gpma.unit_max = 0;
    gpma.unit_cmdtab = 0;
    cmdslot->u.fts.nnode = 0;
    cmdslot->u.fts.nopened = 0;

    return 0;
}
