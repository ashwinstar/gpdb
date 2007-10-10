static apr_status_t config_addsegment(struct json_object *msg, char *key, const char *value)
{
    char *node, *last;
    char *dir;
    char *v = strdup(value);
    struct json_object *obj;

    if (!v) {
	gpcc_fatal(FLINE, "out of memory");
    }

    node = apr_strtok(v, ":", &last);
    if (!node)
	goto bail;

    dir = apr_strtok(0, ":", &last);
    if (!dir)
	goto bail;

    obj = json_object_new_object();
    if (!obj)
	goto bail;

    json_object_object_add(obj, "node", json_object_new_string(node));
    json_object_object_add(obj, "dir", json_object_new_string(dir));
    
    json_object_object_add(msg, key, obj);

    if (v) free(v);
    return 0;

    bail:
    if (v) free(v);
    return APR_EINVAL;

}

static apr_status_t config_addnode(struct json_object *msg, char *key, const char *value)
{
    char *bs, *es;
    char *nodestr = strdup(value);
    char *tok, *last;
    struct json_object *array;

    if (!nodestr){
	gpcc_fatal(FLINE, "out of memory");
    }

    array = json_object_new_array();
    if (!array)
	goto bail;

    bs = strchr(nodestr, '(');
    es = strchr(nodestr, ')');

    if (!bs || !es)
	goto bail;
    
    nodestr=bs+1;
    *es = 0;
    
    for (tok = apr_strtok(nodestr, " ,", &last) ; tok ; tok = apr_strtok(0, " ,", &last)) {
	json_object_array_add(array, json_object_new_string(gpcc_string_trim(tok)));
    }

    json_object_object_add(msg, key, array);

    if (nodestr) free(nodestr);
    return 0;

    bail:

    if (nodestr) free(nodestr);
    return APR_EINVAL;
}

static struct json_object *config_to_json_object() 
{
    struct json_object *obj;
    char key[128];
    int i;
    const char *value;
    int intvalue;

    obj = json_object_new_object();

    strcpy(key, "gpdb.name");
    value = config_getx(gpma.cfg, key, "mygpdb");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.user");
    value = config_getx(gpma.cfg, key, getenv("USER"));
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.password");
    value = config_getx(gpma.cfg, key, "mpparray");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.max_connect");
    intvalue = config_get_or_set_int(gpma.cfg, key, 25);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.encoding");
    value = config_getx(gpma.cfg, key, "SQL_ASCII");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.locale");
    value = config_getx(gpma.cfg, key, "en_US.utf-8");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.shared_buffers");
    value = config_getx(gpma.cfg, key, "128MB");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.checkpoint_segments");
    intvalue = config_get_or_set_int(gpma.cfg, key, 31);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.master_port");
    intvalue = config_get_or_set_int(gpma.cfg, key, DEFAULT_QD_PORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.base_port");
    intvalue = config_get_or_set_int(gpma.cfg, key, DEFAULT_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.mirror_base_port");
    intvalue = config_get_or_set_int(gpma.cfg, key, DEFAULT_MIRROR_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    
    for (i = 0 ;  ; i++) {
	sprintf(key, "node.%d", i);
	value = config_get(gpma.cfg, key);
	if (!value) 
	    break;

	config_addnode(obj, key, value);
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;

	config_addsegment(obj, key, value);
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    break;
	config_addsegment(obj, key, value);

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpma.cfg, key);
	if (!value)
	    continue;
	config_addsegment(obj, key, value);
    }

    gpcc_log_info(json_object_to_json_string(obj));

    return obj;
}



static inline void setup_set_segment(cmdslot_t *cmdslot, int idx, char *name, int dbid, int content, int port, int node, char *dir, apr_pool_t *pool)
{
    char key[128];
    const char *value, *s;
    char addr[128];

    cmdslot->u.sys.seginfo[idx].name = apr_pstrdup(pool, name);
    cmdslot->u.sys.seginfo[idx].dbid = dbid;
    cmdslot->u.sys.seginfo[idx].content = content;
    cmdslot->u.sys.seginfo[idx].port = port;
    cmdslot->u.sys.seginfo[idx].haidx = node;
    cmdslot->u.sys.seginfo[idx].ha = apr_psprintf(pool, "node.%d", node);
    cmdslot->u.sys.seginfo[idx].dir = apr_pstrdup(pool, dir);

    sprintf(key, "node.%d", node);
    value = config_get(gpma.cfg, key);
    s = strchr(value, ',');
    strncpy(addr, value+1, s - value - 1);
    addr[s-value-1] = 0;
    cmdslot->u.sys.seginfo[idx].host = apr_pstrdup(pool, addr);
}

static apr_status_t setup_init_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int i, n, dbid, content, node;
    char key[128], dir[1024];
    const char *value;
    int master_port, base_port, mirror_base_port;

    cmdslot->u.sys.config_json = config_to_json_object();

    strcpy(key, "gpdb.seg.master_port");
    master_port = config_get_or_set_int(gpma.cfg, key, DEFAULT_QD_PORT);
    strcpy(key, "gpdb.seg.base_port");
    base_port = config_get_or_set_int(gpma.cfg, key, DEFAULT_QE_BASEPORT);
    strcpy(key, "gpdb.seg.mirror_base_port");
    mirror_base_port = config_get_or_set_int(gpma.cfg, key, DEFAULT_MIRROR_QE_BASEPORT);

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

    cmdslot->u.sys.nseginfo = n;
    cmdslot->u.sys.seginfo = apr_palloc(cmdslot->pool, sizeof(*cmdslot->u.sys.seginfo) * n);

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


static apr_status_t a_fts_setup_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    gpcc_log_info("setup init");
    if (cmdslot->u.sys.config_json == 0) {
	cmdslot->u.sys.config_json = config_to_json_object();
	setup_init_seginfo(ev, _cmdslot);
	
	{
	    int i = 0;
	    for (i = 0 ; i < cmdslot->u.sys.nseginfo ; i++) {
		gpcc_log_info("seginfo[%d] = {name: %s, host: %s, dbid: %d, content: %d, port: %d, dir: %s, haidx: %d}",
			      i,
			      cmdslot->u.sys.seginfo[i].name,
			      cmdslot->u.sys.seginfo[i].host,
			      cmdslot->u.sys.seginfo[i].dbid,
			      cmdslot->u.sys.seginfo[i].content,
			      cmdslot->u.sys.seginfo[i].port,
			      cmdslot->u.sys.seginfo[i].dir,
			      cmdslot->u.sys.seginfo[i].haidx);
	    }
	}
    }

    return 0;
}
