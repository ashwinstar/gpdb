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

static apr_status_t initdb_check_params() 
{
    /*
    unsetenv("PORT_BASE");
    unsetenv("SEG_PREFIX");
    unsetenv("DATA_DIRECTORY");
    */
    /* reload environment variable from configurations */
    /*  
	USER, PASSWORD, SEG_PREFIX, LOCALE_SETTING, ARRAY_NAME, ENCODING, TRUSTED_SHELL, CHECK_POINT_SEGMENTS
	PORT_BASE, DATA_DIRECTORY
	MIRROR_PORT_BASE, MIRROR_DATA_DIRECTORY
	MASTER_HOSTNAME, MASTER_PORT, MASTER_DIRECTORY
    */

    return 0;
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
    gpcc_event_t *evt;


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
	    evt = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len - 1);
	    memcpy(evt->edata, packet, sizeof(*packet) + packet->mh.len  - 1);
	    gpcc_evqueue_put(gpha.evq, evt);
	    json_object_put(obj);
	    return 0;
	}
    }

    /* new segment request */
    slot = cmdslot_alloc(seg, SEG_INIT_CMDSTATE, 0);
    evt = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len - 1);
    memcpy(evt->edata, packet, sizeof(*packet) + packet->mh.len  - 1);
    gpcc_evqueue_put(gpha.evq, evt);    
    
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


static apr_status_t seg_request_cleanup(gpcc_event_t* ev, void *_cmdslot)
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

static apr_status_t seg_request_error(gpcc_event_t *ev, void *_cmdslot, event_t event, int exitcode, char *fmt, ...)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(event, cmdslot->cmdidx, 0);
    cmdslot->u.seg.request.exitcode = exitcode;
    if (fmt) {
	va_list ap;
	va_start(ap, fmt);
	cmdslot->u.seg.request.reason = apr_pvsprintf(cmdslot->u.seg.request.subpool, fmt, ap);
	va_end(ap);
    } else {
	cmdslot->u.seg.request.reason = "";
    }
    gpcc_warning(FLINE, cmdslot->u.seg.request.reason);
    gpcc_evqueue_put(gpha.evq, evt);

    return 0;


}

/* segment initdb */

static apr_status_t seg_initdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    struct json_object *obj = json_tokener_parse(packet->mb);
    int e;
    char *initdbfn = 0;
    int pid = 0;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool);

    if (!obj) {
	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, 1, "json_tokener_parse error");
	json_object_put(obj);
	return 0;
    }
    
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;
    cmdslot->u.seg.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.seg.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.seg.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.seg.request.dbid = gpcc_json_object_get_int(obj, "dbid", 0);
    cmdslot->u.seg.request.content = gpcc_json_object_get_int(obj, "content", 0);

    if (cmdslot->u.seg.request.port == 0 || 
	cmdslot->u.seg.request.mode == 0 ||
	cmdslot->u.seg.request.data_dir == 0) {	
	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, 1, "parameter is missing");
	json_object_put(obj);
	return 0;
    }

    /* check environment variable */
    
    /* check initdb is valid */
    apr_filepath_merge(&initdbfn, gpha.gphome, "initdb", 0, cmdslot->u.seg.request.subpool);
    if (! gpcc_file_exists(initdbfn)) {
	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, 1, "initdb not found");
	json_object_put(obj);
	return 0;
    }

    /* check data_dir exists */
    if (gpcc_file_directory_exists(cmdslot->u.seg.request.data_dir)) {
	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, 1, "data directory exists");
	json_object_put(obj);
	return 0;
    }	

    /* check database is running (port)*/
    if (0 == (e = gpcc_pg_pid_active(cmdslot->u.seg.request.port, &pid, cmdslot->u.seg.request.subpool))) {
	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, 1, "database is running");
	json_object_put(obj);
	return 0;	
    }

    /* initdb start */
    
    if (0 != (e = gpcc_proc_initdb(&cmdslot->u.seg.request.proc,
				   cmdslot->u.seg.request.data_dir,
				   gpcc_json_object_get_string(obj, "gpdb.encoding", "UNICODE"),
				   gpcc_json_object_get_string(obj, "gpdb.locale", "UNICODE"),
				   gpcc_json_object_get_int(obj, "gpdb.max_connect", 25),
				   gpcc_json_object_get_string(obj, "gpdb.shared_buffers", "128Mb"),
				   1,
				   cmdslot->u.seg.request.subpool))) {

	seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "initdb failure");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.msg = obj;
    cmdslot->countdown = 1;

    
    return 0;
}


static apr_status_t seg_initdb_pg_hba_append(gpcc_event_t* ev, void *_cmdslot) 
{

    return 0;
}

static apr_status_t seg_initdb_pg_conf_update(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_pg_conf_subst_t subst[5];
    int e;

    if (strstr(cmdslot->cmdname, "gpdb.seg.m.") != 0) {
	subst[0].pattern = "#log_statement";
	subst[0].insert = "log_statement=all";
	subst[1].pattern = "stats_command_string";
	subst[1].insert = "stats_command_string=on";
	subst[2].pattern = "#port";
	subst[2].insert = apr_psprintf(cmdslot->u.seg.request.subpool, "port=%d", cmdslot->u.seg.request.port);
	subst[3].pattern = "checkpoint_segments";
	subst[3].insert = apr_psprintf(cmdslot->u.seg.request.subpool, "checkpoint_segments=%d", 
				       gpcc_json_object_get_int(cmdslot->u.seg.request.msg, "gpdb.checkpoint_segments", 31));
	subst[4].pattern = 0;

    } else {
	subst[0].pattern = "listen_addresses";
	subst[0].insert = "listen_addresses='*'";
	subst[1].pattern = "#port";
	subst[1].insert = apr_psprintf(cmdslot->u.seg.request.subpool, "port=%d", cmdslot->u.seg.request.port);
	subst[2].pattern = "checkpoint_segments";
	subst[2].insert = apr_psprintf(cmdslot->u.seg.request.subpool, "checkpoint_segments=%d", 
				       gpcc_json_object_get_int(cmdslot->u.seg.request.msg, "gpdb.checkpoint_segments", 31));
	subst[3].pattern = 0;

    }
    

    if (0 != (e = gpcc_pg_update_postgresql_conf(cmdslot->u.seg.request.data_dir, subst, cmdslot->u.seg.request.subpool))) {
	seg_request_error(ev, _cmdslot, SEG_INITDB_DONE_EVENT, e, "failed to update postgresql.conf");
	return 0;
    }

    return 0;

}

static apr_status_t seg_initdb_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.seg.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    /* update pg_hba.conf */
	    if (0 != (e = seg_initdb_pg_hba_append(ev, _cmdslot))) {
		seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "update pg_hba.conf failure");
		return 0;
	    }

	    /* update postgresql.conf */
	    if (0 != (e = seg_initdb_pg_conf_update(ev, _cmdslot))) {
		seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "update postgresql.conf failure");
		return 0;
	    }

	    /* start database */
	    if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.seg.request.proc,
							 cmdslot->u.seg.request.port,
							 cmdslot->u.seg.request.data_dir,
							 1, 0,
							 cmdslot->u.seg.request.subpool))) {
		seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "pg_ctl start failure");
		return 0;
	    }

	    evt = gpcc_event_alloc(SEG_INITDB_NEXT_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpha.evq, evt);
	    

	} else {
	    seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, ret, proc_exit_reason(exitwhy));
	}

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}



static apr_status_t seg_initdb_start_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.seg.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    evt = gpcc_event_alloc(SEG_INITDB_NEXT_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpha.evq, evt);
	    
	} else {
	    seg_request_error(ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, ret, proc_exit_reason(exitwhy));
	}

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}

static apr_status_t seg_initdb_backout1(gpcc_event_t *ev, void *_cmdslot)
{
    
    return 0;
}

static apr_status_t seg_initdb_backout2(gpcc_event_t *ev, void *_cmdslot)
{

    return 0;
}

static apr_status_t seg_initdb_checkdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    /* save the setting port and data_dir permanently */
    if (cmdslot->u.seg.port == 0) {
	cmdslot->u.seg.port = cmdslot->u.seg.request.port;
    }
    if (cmdslot->u.seg.data_dir == 0) {
	cmdslot->u.seg.data_dir = apr_pstrdup(cmdslot->pool, cmdslot->u.seg.request.data_dir);
    }
    

    return 0;
}

static apr_status_t seg_initdb_reply(gpcc_event_t* ev, void *_cmdslot)
{

    return 0;   
}

/* segment start */
static apr_status_t seg_start(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    struct json_object *obj = json_tokener_parse(packet->mb);
    int e;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool);

    if (!obj) {
	seg_request_error(ev, _cmdslot, SEG_START_DONE_EVENT, 1, "json_tokener_parse error");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;
    cmdslot->u.seg.request.port = json_object_get_int(json_object_object_get(obj, "port"));
    cmdslot->u.seg.request.mode = json_object_get_string(json_object_object_get(obj, "mode"));
    cmdslot->u.seg.request.data_dir = json_object_get_string(json_object_object_get(obj, "dir"));

    
    if (strstr(json_object_get_string(json_object_object_get(obj, "seg")), "gpdb.seg.m.") == 0) {
	/* this is QE */

	if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.seg.request.proc,
						     cmdslot->u.seg.request.port,
						     cmdslot->u.seg.request.data_dir,
						     1, 0, 
						     cmdslot->u.seg.request.subpool))) {
	    seg_request_error(ev, _cmdslot, SEG_START_DONE_EVENT, e, "pg_ctl start segment failure");
	    json_object_put(obj);
	    return 0;
	}
    } else {
	/* this is QD */
	if (0 != (e = gpcc_proc_pg_ctl_start_master(&cmdslot->u.seg.request.proc,
						     cmdslot->u.seg.request.port,
						     cmdslot->u.seg.request.data_dir,
						     1, 0, 
						     cmdslot->u.seg.request.subpool))) {
	    seg_request_error(ev, _cmdslot, SEG_START_DONE_EVENT, e, "pg_ctl start master failure");
	    json_object_put(obj);
	    return 0;
	}
    }	

    cmdslot->u.seg.request.msg = obj;
    cmdslot->countdown = 1;
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

/* segment stop */
static apr_status_t seg_stop(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->edata;
    struct json_object *obj = json_tokener_parse(packet->mb);
    int e;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool);

    if (!obj) {
	seg_request_error(ev, _cmdslot, SEG_STOP_DONE_EVENT, 1, "json_tokenr_parse error");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;
    cmdslot->u.seg.request.port = json_object_get_int(json_object_object_get(obj, "port"));
    cmdslot->u.seg.request.mode = json_object_get_string(json_object_object_get(obj, "mode"));
    cmdslot->u.seg.request.data_dir = json_object_get_string(json_object_object_get(obj, "dir"));
    
    if (0 != (e = gpcc_proc_pg_ctl_stop(&cmdslot->u.seg.request.proc,
					cmdslot->u.seg.request.port,
					cmdslot->u.seg.request.data_dir,
					cmdslot->u.seg.request.mode,
					1, 0, 
					cmdslot->u.seg.request.subpool))) {

	seg_request_error(ev, _cmdslot, SEG_STOP_DONE_EVENT, e, "pg_ctl stop failure");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.msg = obj;
    cmdslot->countdown = 1;
    return 0;
}

static apr_status_t seg_stop_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.seg.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	evt = gpcc_event_alloc(SEG_STOP_DONE_EVENT, cmdslot->cmdidx, 0);
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


static apr_status_t seg_stop_reply(gpcc_event_t* ev, void *_cmdslot)
{

    return 0;
}



/* segment status */
static apr_status_t seg_status(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int e;

    if (cmdslot->u.seg.port == 0) {
	gpcc_warning(FLINE, "port is missing");
	cmdslot->u.seg.request.exitcode = 1;
	cmdslot->u.seg.request.reason = "port is missing";
	goto bail;
    }

    if (cmdslot->u.seg.data_dir == 0) {
	gpcc_warning(FLINE, "data_dir is missing");
	cmdslot->u.seg.request.exitcode = 2;
	cmdslot->u.seg.request.reason = "data directory is missing";
	goto bail;
    }

    if (0 != (e = apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool))) {
	gpcc_warning(FLINE, "not enough memory");
	cmdslot->u.seg.request.exitcode = e;
	cmdslot->u.seg.request.reason = "not enough memory";
	goto bail;
    }
    
    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.seg.port,
				     cmdslot->u.seg.data_dir,
				     cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "server is not running");
	cmdslot->u.seg.request.exitcode = e;
	cmdslot->u.seg.request.reason = "server is not running";
	goto bail;
    }

    /* reply -- server is running */


    if (cmdslot->u.seg.request.subpool) apr_pool_destroy(cmdslot->u.seg.request.subpool);
    return 0;

    bail:

    /* reply -- error */


    if (cmdslot->u.seg.request.subpool) apr_pool_destroy(cmdslot->u.seg.request.subpool);
    return 0;
}
