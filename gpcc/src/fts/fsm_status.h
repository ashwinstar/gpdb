static apr_status_t status_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    apr_pool_create(&cmdslot->u.sys.request.pool, 0);
    memset(&cmdslot->u.sys.request.u, 0, sizeof(cmdslot->u.sys.request.u));
    cmdslot->u.sys.request.u.status.replied = apr_pcalloc(cmdslot->u.sys.request.pool, sizeof(int) * cmdslot->u.sys.nseginfo);

    return 0;
}
static apr_status_t status_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, idx, e;
    hainfo_t *hainfo;
    struct json_object *obj;
    char *msg;

    for (i = 0 ;  i < cmdslot->u.sys.nseginfo ; i++) {
	idx = cmdslot->u.sys.seginfo[i].haidx;
	hainfo = cmdslot->u.sys.hainfo[idx];
	if (!hainfo) {
	    gpcc_warning(FLINE, "hainfo[%d] not found", idx);
	    return APR_EINVAL;
	}

	cmdslot->u.sys.request.u.status.replied[i] = 0;

	obj = json_tokener_parse(json_object_to_json_string(cmdslot->u.sys.config_json));
	obj = gpcc_json_segment_request(obj,
					cmdslot->u.sys.seginfo[i].name,
					"status",
					"",
					"",
					cmdslot->u.sys.seginfo[i].port,
					cmdslot->u.sys.seginfo[i].dir);

	msg = json_object_to_json_string(obj);

	if (0 != (e = gpcc_ha_send(hainfo->ha, STATUS_REQUEST_MSG, msg, strlen(msg) + 1))) {
	    gpcc_warning(FLINE, "send packet failed");
	    json_object_put(obj);
	    return APR_ECONNRESET;
	}

	json_object_put(obj);
    }

    return 0;
}

static apr_status_t status_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj = 0;
    gpcc_event_t *evt;
    int i;

    switch (ev->etype) {
    case NET_CLOSED_EVENT:
	{
	int index;
	memcpy(&index, ev->edata, sizeof(int));
	for (i = 0 ; i < cmdslot->u.sys.nseginfo ; i++) {
	    if (cmdslot->u.sys.seginfo[i].haidx == index) {
		if (cmdslot->u.sys.request.u.status.replied[i] == 0) {
		    cmdslot->u.sys.request.u.status.nfailed++;
		    cmdslot->u.sys.request.u.status.nreply++;
		    cmdslot->u.sys.request.u.status.replied[i]++;
		} else {
		    cmdslot->u.sys.request.u.status.nfailed++;
		}
		gpcc_warning(FLINE, "[Error] segment %s connection reset", cmdslot->u.sys.seginfo[i].name);
	    }
	}

	break;
	}
    case STATUS_REPLY_MSG_EVENT:
	{
	char *seg, *health, *mode, *alert;
	gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->edata;

	obj = json_tokener_parse(pkt->mb);
	seg = gpcc_json_object_get_string(obj, "seg", 0);
	health = gpcc_json_object_get_string(obj, "health", "failed");
	mode = gpcc_json_object_get_string(obj, "mode", "");
	alert = gpcc_json_object_get_string(obj, "alert", "");
	
	for (i = 0 ; i < cmdslot->u.sys.nseginfo ; i++) {
	    if (strcmp(seg, cmdslot->u.sys.seginfo[i].name) == 0) 
		break;
	}
	if (i >= cmdslot->u.sys.nseginfo){
	    gpcc_warning(FLINE, "[Error] invalid segment name %s", seg);
	    goto bail;
	}

	cmdslot->u.sys.request.u.status.replied[i]++;
	if (strcmp(health, "ok") == 0) {
	    cmdslot->u.sys.request.u.status.nsuccess++;
	} else {
	    gpcc_warning(FLINE, "[Error] segment status failed.  %s health=%s, mode=%s, alert=%s", seg, health, mode, alert);
	    cmdslot->u.sys.request.u.status.nfailed++;
	}
	cmdslot->u.sys.request.u.status.nreply++;
	    
	break;
	}
    default:
	break;
    }

    if (cmdslot->u.sys.request.u.status.nreply >= cmdslot->u.sys.nseginfo) {
	if (cmdslot->u.sys.request.u.status.nfailed == 0) {
	    /* success */
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, evt);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, evt);
	}
    }


    if (obj) json_object_put(obj);
    return 0;

    bail:

    if (obj) json_object_put(obj);
    return APR_EINVAL;
	
}

static apr_status_t status_success(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("status", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);
    return 0;
}

static apr_status_t status_failure(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("status", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);    
    return 0;
}

static apr_status_t status_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.sys.config_json) {
	json_object_put(cmdslot->u.sys.config_json);
    }
    if (cmdslot->u.sys.request.pool) {
	apr_pool_destroy(cmdslot->u.sys.request.pool);
	cmdslot->u.sys.request.pool = 0;
    }
    
    return 0;
}
