
static struct json_object *config_to_json_object() 
{
    struct json_object *obj;
    char key[128];
    int i;
    const char *value;
    int intvalue;

    obj = json_object_new_object();

    strcpy(key, "gpdb.name");
    value = config_getx(gpfts.config, key, "mygpdb");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.user");
    value = config_getx(gpfts.config, key, getenv("USER"));
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.password");
    value = config_getx(gpfts.config, key, "mpparray");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.max_connect");
    intvalue = config_get_or_set_int(gpfts.config, key, 25);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.encoding");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.locale");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.shared_buffers");
    value = config_getx(gpfts.config, key, "128MB");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.checkpoint_segments");
    intvalue = config_get_or_set_int(gpfts.config, key, 31);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.master_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QD_PORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.mirror_base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_MIRROR_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    
    for (i = 0 ;  ; i++) {
	sprintf(key, "node.%d", i);
	value = config_get(gpfts.config, key);
	if (!value) 
	    break;

	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }



    return obj;
}

static inline void initdb_set_segment(cmdslot_t *cmdslot, int idx, char *name, int dbid, int content, int port, int node, char *dir, apr_pool_t *pool)
{

    cmdslot->u.sys.request.u.initdb.segment[idx].name = apr_pstrdup(pool, name);
    cmdslot->u.sys.request.u.initdb.segment[idx].dbid = dbid;
    cmdslot->u.sys.request.u.initdb.segment[idx].content = content;
    cmdslot->u.sys.request.u.initdb.segment[idx].port = port;
    cmdslot->u.sys.request.u.initdb.segment[idx].node = node;
    cmdslot->u.sys.request.u.initdb.segment[idx].dir = apr_pstrdup(pool, dir);
}

static apr_status_t initdb_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int e, i, n, dbid, content, node;
    char key[128], dir[1024];
    const char *value;
    apr_pool_t *pool;
    int master_port, base_port, mirror_base_port;

    if (0 != (e = apr_pool_create(&cmdslot->u.sys.request.pool, 0))) {
	gpcc_warning(FLINE, "apr_pool_create failed");
	return APR_ENOMEM;
    }
    pool = cmdslot->u.sys.request.pool;

    cmdslot->u.sys.request.u.initdb.config_json = config_to_json_object();

    master_port = config_get_or_set_int(gpfts.config, key, DEFAULT_QD_PORT);
    base_port = config_get_or_set_int(gpfts.config, key, DEFAULT_QE_BASEPORT);
    mirror_base_port = config_get_or_set_int(gpfts.config, key, DEFAULT_MIRROR_QE_BASEPORT);

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
    }

    n = i;

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	
	n++;

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;

	n++;
    }	

    cmdslot->u.sys.request.u.initdb.nsegment = n;
    cmdslot->u.sys.request.u.initdb.segment = apr_palloc(cmdslot->u.sys.request.pool, sizeof(*cmdslot->u.sys.request.u.initdb.segment) * n);

    /* master QD */
    strcpy(key, "gpdb.seg.m.0");
    dbid = 1;
    content = -1;
    if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	return APR_EINVAL;
    }	
    
    initdb_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, pool);
    
    for (i = 0 ;  ; i++) {
	/* master QE */
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}	
	dbid++;
	content++;
	initdb_set_segment(cmdslot, dbid-1, key, dbid, content, base_port++, node, dir, pool);

	/* mirror QE */
	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}
	dbid++;	
	initdb_set_segment(cmdslot, dbid-1, key, dbid, content, mirror_base_port++, node, dir, pool);
    }

    /* standby QD */
    content = -1;
    for (i = 1 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}
	dbid++;
	initdb_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, pool);
    }	

    return 0;
}

static apr_status_t initdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    char seg[128];
    const char *value;
    struct json_object *obj;
    int i, dbid = 0;

    /* prepare json object */
    obj = config_to_json_object();


    /* master QD */
    strcpy(seg, "gpdb.seg.m.0");
    value = config_get(gpfts.config, seg);
    if (!value) {
	json_object_put(obj);
	return APR_EINVAL;
    }
    dbid=1;

    /* init segment */
    for (i = 0 ; ; i++) {
	sprintf(seg, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, seg);
	if (!value) {
	    break;
	}
	dbid++;


	sprintf(seg, "gpdb.seg.%d.1", i);    
	value = config_get(gpfts.config, seg);
	if (!value) {
	    continue;
	}
	dbid++;

    }

    /* standby QD */
    for (i = 1 ; ; i++) {
	sprintf(seg, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, seg);
	if (!value) {
	    break;
	}

	dbid++;
    }


    return 0;
}

static apr_status_t initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t initdb_success(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t initdb_failure(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}
