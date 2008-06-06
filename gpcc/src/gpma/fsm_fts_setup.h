static apr_status_t a_fts_http_redirect_leadma(gpcc_net_conn_t conn, const char *url)
{
    char location[128], json[1024];

    if (gpma.lma_cmdslot->u.lma.leader) {
	sprintf(location, "http://%s%s", gpma.lma_cmdslot->u.lma.leader, url);
	sprintf(json, "{ \"a\": \"%s\", \"status\": \"error\", \"location\" : \"%s\", \"errmsg\": \"this is not a lead MA\"}", 
		gpma.array_name,
		gpma.lma_cmdslot->u.lma.leader);
	gpcc_net_http_redirect_close(conn, location, json);
    } 
    return 0;
}

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


static apr_status_t setup_parse_segment(const char *key, const char *value, char *host, char *dir, int *port, char *isprimary, char *valid)
{
    char *s1, *s2, *s3, *s4;
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
    s3 = strchr(s2+1, ':');
    if (!s3 || !isprimary || !valid) {
	/* just parse host, dir and port */
	strncpy(host, value, s1-value);
	host[s1-value] = 0;
	strncpy(portstr, s1+1, s2-s1);
	portstr[s2-s1-1] = 0;
	*port = atoi(portstr);
	strcpy(dir, s2+1);
    } else {
	s4 = strchr(s3+1, ':');
	if (!s4) {
	    gpcc_log_error("value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}

	strncpy(host, value, s1-value);
	host[s1-value] = 0;
	strncpy(portstr, s1+1, s2-s1);
	portstr[s2-s1-1] = 0;
	*port = atoi(portstr);
	strncpy(dir, s2+1, s3-s2);
	dir[s3-s2-1] = 0;
	*isprimary = *(s3+1);
	*valid = *(s4+1);
    }

    if (dir[0] != '/') {
	gpcc_warning(FLINE, "segment %s data directory %s must be in full path", key, dir);
	return APR_EINVAL;
    }

    if (!isprimary || !valid) {
	gpcc_log_info("INIT CONF: %s|%d|%s", host, *port, dir);
    } else {
	gpcc_log_info("CONF: %s|%d|%s|%c|%c", host, *port, dir, *isprimary, *valid);
    }

    return APR_SUCCESS;
}

static apr_status_t setup_init_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int i, n, dbid, content, port;
    char key[128], dir[256], host[256], isprimary, valid;
    const char *value;

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

    isprimary = 't';
    valid = 't';
    if (setup_parse_segment(key, value, host, dir, &port, &isprimary, &valid)) {
	return APR_EINVAL;
    }

    dbid = 1;
    content = -1;

    setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
		      host, dir, 't', isprimary, valid, cmdslot->pool);

    
    for (i = 0 ; ; i++) {
	/* primary QE */
	content = i;

	sprintf(key, "seg.%d.0", content);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
	isprimary = 't';
	valid = 't';
	if (setup_parse_segment(key, value, host, dir, &port, &isprimary, &valid)) {
	    return APR_EINVAL;
	}

	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 't', isprimary, valid, cmdslot->pool);

	/* mirror QE */
	sprintf(key, "seg.%d.1", content);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;
	isprimary = 'f';
	valid = 't';
	if (setup_parse_segment(key, value, host, dir, &port, &isprimary, &valid)) {
	    return APR_EINVAL;
	}

	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 'f', isprimary, valid, cmdslot->pool);
    }


    /* standby QD */
    strcpy(key, "seg.m.1");
    value = config_get(gpma.cfg, key);
    if (value) {
	isprimary = 'f';
	valid = 't';
	if (setup_parse_segment(key, value, host, dir, &port, &isprimary, &valid)) {
	    return APR_EINVAL;
	}
	dbid++;
	content = -1;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, port, 
			  host, dir, 'f', isprimary, valid, cmdslot->pool);
    }

    cmdslot->u.fts.nnode = 0;
    cmdslot->u.fts.nopened = 0;

    return 0;
}

static apr_status_t a_fts_init_failure(gpcc_event_t *ev, void *_cmdslot)
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

static apr_status_t a_fts_getaddress(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    cmdslot_t *s;
    gpcc_event_t *evt;
    int i, cnt;
    apr_hash_index_t *hi;
    void *val;
    const void *key;

    cnt = apr_hash_count(slot->u.fts.ht);
    s = cmdslot_alloc("GETADDRESS", SSH_ADDRESS_CMDSTATE);
    s->u.getaddress.parent_cmdidx = slot->cmdidx;
    s->u.getaddress.hosts = apr_palloc(s->pool, sizeof(char *) * cnt);
    s->u.getaddress.address = apr_palloc(s->pool, sizeof(char *) * cnt);
    
    /* non-thread safe with apr_pool_t = 0. */
    for (i = 0, hi = apr_hash_first(0, slot->u.fts.ht) ;
	 hi;
	 hi = apr_hash_next(hi), i++) {
	apr_hash_this(hi, &key, 0, &val);

	s->u.getaddress.hosts[i] = apr_pstrdup(s->pool, (char *) key);
	gpcc_log_info("GETADDRESS === %s", s->u.getaddress.hosts[i]);
    }
    s->u.getaddress.nhost = cnt;
    s->u.getaddress.timeout = 120;

    evt = gpcc_event_alloc(INIT_EVENT, s->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);


    return 0;
}

apr_status_t a_fts_getaddress_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    struct json_object *obj = json_tokener_parse(ev->pdata);
    struct json_object *rset;
    char *status;
    int i, j, len;

    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);
    if (strcmp(status, OK_STATUS) != 0) {
	goto bail;
    }

    /* fill up hostname */
    rset = json_object_object_get(obj, "rset");
    len = json_object_array_length(rset);
    
    for (i = 0 ; i < len ; i++) {
	char node[20];
	struct json_object *o = json_object_array_get_idx(rset, i);
	char *host = json_object_get_string(json_object_array_get_idx(o, 0));
	char *ip = json_object_get_string(json_object_array_get_idx(o, 1));
	sprintf(node, "node.%d", i);
	config_set(gpma.cfg, node, (const char *) ip);

	for (j = 0 ; j < slot->u.fts.nseginfo ; j++) {
	    if (strcmp(host, slot->u.fts.seginfo[j].hostname) == 0) { 
		slot->u.fts.seginfo[j].haidx = i;
		slot->u.fts.seginfo[j].ha = apr_psprintf(slot->pool, "node.%d", i);
	    }
	}
    }
    
    for (i = 0 ; i < slot->u.fts.nseginfo ; i++) {
	gpcc_log_info("name=%s, host =%s, dbid = %d, content =%d, port = %d, dir = %s, haidx = %d, ha=%s",
		      slot->u.fts.seginfo[i].name,
		      slot->u.fts.seginfo[i].host,
		      slot->u.fts.seginfo[i].dbid,
		      slot->u.fts.seginfo[i].content,
		      slot->u.fts.seginfo[i].port,
		      slot->u.fts.seginfo[i].dir,
		      slot->u.fts.seginfo[i].haidx,
		      slot->u.fts.seginfo[i].ha);
    }    


    evt = gpcc_event_alloc(NODE_START_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    
    if (obj) 
	json_object_put(obj);
    return 0;

    bail:
    
    if (obj) 
	json_object_put(obj);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

apr_status_t a_fts_gethostname(gpcc_event_t *ev, void *arg) 
{
    cmdslot_t *slot = arg;
    cmdslot_t *s;
    gpcc_event_t *evt;
    int i;

    s = cmdslot_alloc("GETHOSTNAME", SSH_HOSTNAME_CMDSTATE);
    s->u.gethostname.parent_cmdidx = slot->cmdidx;
    s->u.gethostname.hosts = apr_palloc(s->pool, sizeof(char *) * slot->u.fts.nseginfo);
    s->u.gethostname.hostname = apr_palloc(s->pool, sizeof(char *) * slot->u.fts.nseginfo);
    for (i = 0 ; i < slot->u.fts.nseginfo ; i++) {
	s->u.gethostname.hosts[i] = apr_pstrdup(s->pool, slot->u.fts.seginfo[i].host);
    }
    s->u.gethostname.nhost = slot->u.fts.nseginfo;
    s->u.gethostname.timeout = 120;

    evt = gpcc_event_alloc(INIT_EVENT, s->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    
    return 0;
}

apr_status_t a_fts_gethostname_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    struct json_object *obj = json_tokener_parse(ev->pdata);
    struct json_object *rset;
    char *status;
    int i, len;

    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);
    if (strcmp(status, OK_STATUS) != 0) {
	goto bail;
    }

    /* fill up hostname */
    rset = json_object_object_get(obj, "rset");
    len = json_object_array_length(rset);
    if (len != slot->u.fts.nseginfo) {
	goto bail;
    }
    
    for (i = 0 ; i < slot->u.fts.nseginfo ; i++) {
	struct json_object *o = json_object_array_get_idx(rset, i);
	char *s = json_object_get_string(json_object_array_get_idx(o, 1));
	slot->u.fts.seginfo[i].hostname = apr_pstrdup(slot->pool, s);
	gpcc_log_info("HOSTNAME == %s", slot->u.fts.seginfo[i].hostname);
    }
    
    slot->u.fts.ht = apr_hash_make(slot->pool);
    
    for (i = 0 ; i < slot->u.fts.nseginfo ; i++) {
	
	if (apr_hash_get(slot->u.fts.ht, slot->u.fts.seginfo[i].hostname, APR_HASH_KEY_STRING) == 0) {
	    apr_hash_set(slot->u.fts.ht,
			 slot->u.fts.seginfo[i].hostname,
			 APR_HASH_KEY_STRING,
			 "0");
	}
    }    
    
    /* number of unqiue host = number of node */
    slot->u.fts.nnode = apr_hash_count(slot->u.fts.ht);
    
    a_fts_getaddress(ev, arg);

    if (obj) 
	json_object_put(obj);
    return 0;

    bail:
    
    if (obj) 
	json_object_put(obj);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
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

static apr_status_t a_fts_setup_and_gethostname(gpcc_event_t *ev, void *_cmdslot)
{
    if (setup_init_seginfo(ev, _cmdslot))
	gpcc_fatal(FLINE, "seginfo setup error.");

    /* get hostname */
    a_fts_gethostname(ev, _cmdslot);
    
    return 0;
}

static apr_status_t a_fts_kick_off(gpcc_event_t *ev, void *_cmdslot)
{
    /* kick off the node */
    a_fts_node_init();
    return 0;
}

static apr_status_t a_fts_kick_off_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;

    a_fts_kick_off(ev, _cmdslot);
    /* check for connection timeout */
    cmdslot->countdown = 30;
    return 0;
}

static apr_status_t a_fts_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;

    if (config_get(gpma.cfg, CONFIG_DBNAME_KEY)) {
	evt = gpcc_event_alloc(ARRAY_PRESENT_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	
    } else {
	evt = gpcc_event_alloc(ARRAY_ABSENT_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }


    return 0;
}



