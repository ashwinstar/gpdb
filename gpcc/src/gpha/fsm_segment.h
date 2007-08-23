static event_t segment_msg_get_event(struct json_object *obj) 
{
    struct json_object *aobj = json_object_object_get(obj, "action");
    char *action;

    if (!aobj) {
	gpcc_warning(FLINE, "action is missing");
	return NULL_EVENT;
    }
    action = json_object_get_string(aobj);
    
    if (strcmp(action, "initdb") == 0) {
	return SEG_INITDB_EVENT;
    } else if (strcmp(action, "start") == 0) {
	return SEG_START_EVENT;
    } else if (strcmp(action, "stop") == 0) {
	return SEG_STOP_EVENT;
    } 

    return NULL_EVENT;

}

static apr_status_t ma_dispatch_segment(gpcc_event_t *ev, void *_cmdslot)
{
    char *seg = 0;
    int i;
    cmdslot_t *slot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    struct json_object *obj = json_tokener_parse(packet->mb);
    struct json_object *segobj;
    event_t etype;

    if (!obj) {
	gpcc_warning(FLINE, "json_tokener_parse error");
	return 0;
    }
    
    segobj = json_object_object_get(obj, "seg");


    if (!segobj) {
	gpcc_warning(FLINE, "segment name is missing");
	json_object_put(obj);
	return 0;
    }
    seg = json_object_get_string(segobj);

    switch (ev->etype) {
    case SEGMENT_REQUEST_MSG:
	etype = segment_msg_get_event(obj);
	break;
    case STATUS_REQUEST_MSG:
	etype = SEG_STATUS_EVENT;
	break;
    default:
	gpcc_warning(FLINE, "invalid message type %d", ev->etype);
	json_object_put(obj);
	return 0;
    }

    if (etype == NULL_EVENT) {
	gpcc_warning(FLINE, "action is missing");
	json_object_put(obj);
	return 0;
    }
	
    for (i = 1 ; i < CMDSLOT_MAX ; i++) {
	slot = &gpha.cmdslot[i];
	if (slot->cmdidx && strcmp(slot->cmdname, seg) == 0) {
	    ev = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len - 1);
	    memcpy(ev->edata, packet, sizeof(*packet) + packet->mh.len  - 1);
	    gpcc_evqueue_put(gpha.evq, ev);
	    json_object_put(obj);
	    return 0;
	}
    }

    /* new segment request */
    slot = cmdslot_alloc(seg, SEG_INIT_CMDSTATE, 0);
    ev = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len - 1);
    memcpy(ev->edata, packet, sizeof(*packet) + packet->mh.len  - 1);
    gpcc_evqueue_put(gpha.evq, ev);    
    
    json_object_put(obj);
    return 0;
}

/* initialize */
static apr_status_t seg_initialize(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    memset(&cmdslot->u.seg, 0, sizeof(cmdslot->u.seg));

    return 0;
}

/* segment initdb */

static apr_status_t seg_initdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;
    apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool);

    /* check initdb is valid */
    /* check data_dir exists */

    /* check database is running (port)*/

    /* initdb start */

    
    return 0;
}

static apr_status_t seg_initdb_wait(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}

static apr_status_t seg_initdb_pg_hba_append(gpcc_event_t* ev, void *cmdslot) 
{

    return 0;
}

static apr_status_t seg_initdb_checkdb(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}

static apr_status_t seg_initdb_reply(gpcc_event_t* ev, void *cmdslot)
{

    return 0;   
}

static apr_status_t seg_initdb_cleanup(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.seg.request.subpool) {
	apr_pool_destroy(cmdslot->u.seg.request.subpool);
    }
    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    return 0;
}



/* segment start */
static apr_status_t seg_start(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    struct json_object *obj = json_tokener_parse(packet->mb);
    gpcc_event_t *evt;
    int e;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));

    if (!obj) {
	gpcc_warning(FLINE, "json_tokener_parse error");
	evt = gpcc_event_alloc(SEG_START_DONE_EVENT, cmdslot->cmdidx, 0);
	cmdslot->u.seg.request.exitcode = 1;
	cmdslot->u.seg.request.reason = "json_tokener_parse error";
	gpcc_evqueue_put(gpha.evq, evt);
	json_object_put(obj);
	return 0;
    }

    apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool);
    cmdslot->u.seg.request.msg = obj;
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;
    cmdslot->u.seg.request.port = json_object_get_int(json_object_object_get(obj, "port"));
    cmdslot->u.seg.request.mode = json_object_get_string(json_object_object_get(obj, "mode"));
    cmdslot->u.seg.request.data_dir = json_object_get_string(json_object_object_get(obj, "dir"));
    
    if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.seg.request.proc,
						 cmdslot->u.seg.request.port,
						 cmdslot->u.seg.request.data_dir,
						 1, 0, 
						 cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "pg_ctl start failure");
	evt = gpcc_event_alloc(SEG_START_DONE_EVENT, cmdslot->cmdidx, 0);
	cmdslot->u.seg.request.exitcode = e;
	cmdslot->u.seg.request.reason = "pg_ctl start failure";
	gpcc_evqueue_put(gpha.evq, evt);
	return 0;
    }

    cmdslot->countdown = 1;
    return 0;
}

static char *proc_exit_reason(apr_exit_why_e exitwhy)
{
    switch (exitwhy) {
    case APR_PROC_EXIT:
	return "process terminated normally";
    case APR_PROC_SIGNAL:
	return "process was killed by a signal";
    case APR_PROC_SIGNAL_CORE:
	return "process was killed by a signal, and generated a core dump";
    default:
	return 0;
    }
    return 0;
}

static apr_status_t seg_start_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.seg.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	evt = gpcc_event_alloc(SEG_START_DONE_EVENT, cmdslot->cmdidx, 0);
	cmdslot->u.seg.request.exitcode = ret;
	cmdslot->u.seg.request.reason = proc_exit_reason(exitwhy);	
	gpcc_evqueue_put(gpha.evq, evt);

	/* save the setting port and data_dir permanently */
	if (cmdslot->u.seg.port == 0) {
	    cmdslot->u.seg.port = cmdslot->u.seg.request.port;
	}
	if (cmdslot->u.seg.data_dir == 0) {
	    cmdslot->u.seg.data_dir = apr_pstrdup(cmdslot->pool, cmdslot->u.seg.request.data_dir);
	}
	

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}


static apr_status_t seg_start_reply(gpcc_event_t* ev, void *cmdslot)
{
    /* send reply */
    
    return 0;
}

static apr_status_t seg_start_cleanup(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (cmdslot->u.seg.request.msg) {
	json_object_put(cmdslot->u.seg.request.msg);
    }
    if (cmdslot->u.seg.request.subpool) {
	apr_pool_destroy(cmdslot->u.seg.request.subpool);
    }
    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    return 0;
}

/* segment stop */
static apr_status_t seg_stop(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}

static apr_status_t seg_stop_wait(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}


static apr_status_t seg_stop_reply(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}

static apr_status_t seg_stop_cleanup(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}

/* segment status */
static apr_status_t seg_status(gpcc_event_t* ev, void *cmdslot)
{

    return 0;
}
