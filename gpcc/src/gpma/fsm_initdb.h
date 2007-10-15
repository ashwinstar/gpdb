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
    char *nodestr = strdup(value);
    char *tok, *last;
    struct json_object *array;

    if (!nodestr){
	gpcc_fatal(FLINE, "out of memory");
    }

    array = json_object_new_array();
    if (!array)
	goto bail;

    for (tok = apr_strtok(nodestr, " \t", &last) ; tok ; tok = apr_strtok(0, " \t", &last)) {
	if (*tok == 0) 
	    continue;
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

    value = config_getx(gpma.cfg, CONFIG_DBNAME_KEY, CONFIG_DBNAME_VALUE);
    json_object_object_add(obj, CONFIG_DBNAME_KEY, json_object_new_string((char *)value));    

    value = config_getx(gpma.cfg, CONFIG_USER_KEY, getenv("USER"));
    json_object_object_add(obj, CONFIG_USER_KEY, json_object_new_string((char *)value));    

    value = config_getx(gpma.cfg, CONFIG_PASSWORD_KEY, CONFIG_PASSWORD_VALUE);
    json_object_object_add(obj, CONFIG_PASSWORD_KEY, json_object_new_string((char *)value));    

    intvalue = config_get_or_set_int(gpma.cfg, CONFIG_MAX_CONNECT_KEY, CONFIG_MAX_CONNECT_VALUE);
    json_object_object_add(obj, CONFIG_MAX_CONNECT_KEY, json_object_new_int(intvalue));

    value = config_getx(gpma.cfg, CONFIG_ENCODING_KEY, CONFIG_ENCODING_VALUE);
    json_object_object_add(obj, CONFIG_ENCODING_KEY, json_object_new_string((char *)value));    

    value = config_getx(gpma.cfg, CONFIG_LOCALE_KEY, CONFIG_LOCALE_VALUE);
    json_object_object_add(obj, CONFIG_LOCALE_KEY, json_object_new_string((char *)value));    

    value = config_getx(gpma.cfg, CONFIG_SHARED_BUFFERS_KEY, CONFIG_SHARED_BUFFERS_VALUE);
    json_object_object_add(obj, CONFIG_SHARED_BUFFERS_KEY, json_object_new_string((char *)value));    

    intvalue = config_get_or_set_int(gpma.cfg, CONFIG_CHECKPOINT_SEGMENT_KEY, CONFIG_CHECKPOINT_SEGMENT_VALUE);
    json_object_object_add(obj, CONFIG_CHECKPOINT_SEGMENT_KEY, json_object_new_int(intvalue));

    intvalue = config_get_or_set_int(gpma.cfg, CONFIG_MASTER_PORT_KEY, CONFIG_MASTER_PORT_VALUE);
    json_object_object_add(obj, CONFIG_MASTER_PORT_KEY, json_object_new_int(intvalue));

    intvalue = config_get_or_set_int(gpma.cfg, CONFIG_BASE_PORT_KEY, CONFIG_BASE_PORT_VALUE);
    json_object_object_add(obj, CONFIG_BASE_PORT_KEY, json_object_new_int(intvalue));

    intvalue = config_get_or_set_int(gpma.cfg, CONFIG_MIRROR_BASE_PORT_KEY, CONFIG_MIRROR_BASE_PORT_VALUE);
    json_object_object_add(obj, CONFIG_MIRROR_BASE_PORT_KEY, json_object_new_int(intvalue));

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


static apr_status_t a_initdb_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj = config_to_json_object();
    char *msg = json_object_to_json_string(obj);
    
    cmdslot->u.initdb.config = apr_pstrdup(cmdslot->pool, msg);
    json_object_put(obj);
    return 0;
}


static apr_status_t a_initdb_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    int i, e;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *msg;
    int len = 0;

    i = cmdslot->u.initdb.idx++;
    if (i >= cmdslot->u.initdb.ntotal)
	return 0;

    obj = json_tokener_parse(cmdslot->u.initdb.config);
    obj = gpcc_json_segment_request(obj,
				    cmdslot->u.initdb.req[i].unit,
				    "init",
				    cmdslot->u.initdb.req[i].mode,
				    cmdslot->u.initdb.req[i].host,
				    cmdslot->u.initdb.req[i].port,
				    cmdslot->u.initdb.req[i].datadir);
    json_object_object_add(obj, "dbid", json_object_new_int(cmdslot->u.initdb.req[i].dbid));
    json_object_object_add(obj, "content", json_object_new_int(cmdslot->u.initdb.req[i].content));	
    msg = json_object_to_json_string(obj);
    len = strlen(msg) + 1;
    if (0 != (e = gpcc_net_send_packetx(cmdslot->u.initdb.req[i].node,
					SEGMENT_REQUEST_MSG,
					cmdslot->cmdidx,
					cmdslot->u.initdb.parent_cmdidx,
					msg, len))) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	json_object_put(obj);
	return 0;
    }
    json_object_put(obj);

    cmdslot->countdown = 60 * 60;
    return 0;
}


static apr_status_t a_initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj = 0;
    gpcc_event_t *evt = 0;
    char *seg, *status, *reason;
    int i, success = 0;

    cmdslot->countdown = 0;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    status = gpcc_json_object_get_string(obj, "status", "failed");
    reason = gpcc_json_object_get_string(obj, "reason", "");
    
    
    for (i = 0 ; i < cmdslot->u.initdb.ntotal ; i++) {
	if (strcmp(seg, cmdslot->u.initdb.req[i].unit) == 0) 
	    break;
    }
    if (i >= cmdslot->u.initdb.ntotal){
	gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	json_object_put(obj);
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }
    
    cmdslot->u.initdb.req[i].reply = apr_pstrdup(cmdslot->pool, pkt->payload);
    
    if (strcmp(status, "ok") == 0) {
	cmdslot->u.initdb.nsuccess++;
	success = 1;
    } else {
	cmdslot->u.initdb.nfailed++;
	gpcc_warning(FLINE, "[Error] segment reply failed: segment=%s, status=%s, reason=%s", seg, status, reason);
    }

    if (cmdslot->u.initdb.nsuccess + cmdslot->u.initdb.nfailed == cmdslot->u.initdb.ntotal) {
	if (cmdslot->u.initdb.nfailed == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
	}
	gpcc_evqueue_put(gpma.evq, evt);
	cmdslot->countdown = 0;
    } else {
	if (success) {
	    evt = gpcc_event_alloc(CONTINUE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    cmdslot->countdown = 0;
	}
    }

    if (obj) json_object_put(obj);
    return 0;
}

static void a_initdb_send_json_reply(gpcc_event_t *ev, cmdslot_t *cmdslot, struct json_object *obj)
{
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;
    char *objstr;

    objstr = json_object_to_json_string(obj);    
    evt = gpcc_event_alloc(UNITS_REPLY_MSG_EVENT, 
			   cmdslot->u.initdb.parent_cmdidx, sizeof(*pkt) + strlen(objstr) + 1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = UNITS_REPLY_MSG_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.initdb.parent_cmdidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);   

    gpcc_evqueue_put(gpma.evq, evt);

}

static struct json_object * mkjson_initdb_reply(gpcc_event_t *ev, void *_cmdslot, 
						const char *action, const char *status, const char *reason)
{
    cmdslot_t *cmdslot = (cmdslot_t*) _cmdslot;
    struct json_object *obj, *rsetobj, *arrobj;
    int i, n;

    obj = gpcc_json_http_reply(action, status, reason);
    rsetobj = json_object_new_object();
    arrobj = json_object_new_array();
    for (i = 0, n = 0 ; i < cmdslot->u.initdb.ntotal ; i++) {
	if (cmdslot->u.initdb.req[i].reply) {
	    json_object_array_add(arrobj, json_object_new_string(cmdslot->u.initdb.req[i].reply));
	    n++;
	}
    }
    json_object_object_add(rsetobj, "total", json_object_new_int(n));
    json_object_object_add(rsetobj, "result", arrobj);
    json_object_object_add(obj, "rset", rsetobj);

    return obj;
}



static apr_status_t a_initdb_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = mkjson_initdb_reply(ev, _cmdslot, "init", "ok", 0);
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}


static apr_status_t a_initdb_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = mkjson_initdb_reply(ev, _cmdslot, "init", "failure", 0);
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_initdb_timeout(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;

    obj = mkjson_initdb_reply(ev, _cmdslot, "init", "failure", "timeout");
    a_initdb_send_json_reply(ev, cmdslot, obj);
    json_object_put(obj);
    return 0;

}
