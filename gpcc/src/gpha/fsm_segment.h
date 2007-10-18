
#define seg_is_standby_qd(c) (c->u.seg.isqd && ! c->u.seg.primary)
#define seg_is_master_qd(c) (c->u.seg.isqd && c->u.seg.primary)
#define seg_is_qd(c)  (c->u.seg.isqd)

static event_t segment_msg_get_event(struct json_object *obj) 
{
    struct json_object *aobj = json_object_object_get(obj, "action");
    char *action;

    if (!aobj) {
	gpcc_warning(FLINE, "action is missing");
	return NULL_EVENT;
    }
    action = json_object_get_string(aobj);
    
    if (strcmp(action, "init") == 0) {
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

static apr_status_t ma_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    gpcc_fatal(FLINE, "MA connection closed....shutdown");
    return 0;
}

static apr_status_t ma_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
   
    gpcc_log_info("ma_status_reply");
    return gpcc_net_send_packet(cmdslot->conn, STATUS_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);    
}

static apr_status_t ma_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    gpcc_log_info("ma_stop_reply");

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("seg = %s, status = %s", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    gpcc_log_info("ma_start_reply");

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("seg = %s, status = %s", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_initdb_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    struct json_object *obj;
    char *status;
    char *seg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    gpcc_log_info("ma_initdb_reply");

    obj = json_tokener_parse((char *) pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", 0);
    seg = gpcc_json_object_get_string(obj, "seg", 0);
    gpcc_log_info("seg = %s, status = %s", seg, status);

    json_object_put(obj);
    return gpcc_net_send_packet(cmdslot->conn, SEGMENT_REPLY_MSG, 
				cmdslot->cmdidx, pkt->mh.rcvidx, pkt->payload, pkt->mh.len);

}

static apr_status_t ma_dispatch_segment(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *seg = 0;
    int i;
    cmdslot_t *slot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(packet->payload);
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
	    evt = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len);
	    memcpy(evt->pdata, packet, sizeof(*packet) + packet->mh.len - 1);
	    packet = (gpcc_net_packet_t *) evt->pdata;
	    packet->mh.rcvidx = cmdslot->cmdidx;
	    gpcc_evqueue_put(gpha.evq, evt);
	    json_object_put(obj);
	    return 0;
	}
    }

    /* new segment request */
    slot = cmdslot_alloc(seg, SEG_INIT_CMDSTATE, 0);
    evt = gpcc_event_alloc(etype, slot->cmdidx, sizeof(*packet) + packet->mh.len);
    memcpy(evt->pdata, packet, sizeof(*packet) + packet->mh.len - 1);
    packet = (gpcc_net_packet_t *) evt->pdata;
    packet->mh.rcvidx = cmdslot->cmdidx;
    gpcc_evqueue_put(gpha.evq, evt);    
    
    json_object_put(obj);
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


static apr_status_t seg_request_reply(gpcc_event_t* ev, void *_cmdslot, event_t event)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    char *status;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    status = (cmdslot->u.seg.request.exitcode == 0) ? "ok" : "failure";

    obj = gpcc_json_segment_reply(cmdslot->cmdname, status, cmdslot->u.seg.request.reason);
    objstr = json_object_to_json_string(obj);
    evt = gpcc_event_alloc(event, cmdslot->u.seg.request.parentidx, sizeof(*pkt) + strlen(objstr)+1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = event;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.seg.request.maidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);
    gpcc_evqueue_put(gpha.evq, evt);
    json_object_put(obj);

    return 0;   
}

static apr_status_t seg_request_error(const char *fline, gpcc_event_t *ev, void *_cmdslot, event_t event, int exitcode, char *fmt, ...)
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
    gpcc_warning(fline, cmdslot->u.seg.request.reason);
    gpcc_evqueue_put(gpha.evq, evt);

    return 0;


}



static apr_status_t seg_cmdname_parse(const char *segname, int *isqd, int *idx, int *primary)
{
    int index;

    if (strstr(segname, "gpdb.seg.m.")) {
	/* this is QD */
	if (1 != sscanf(segname, "gpdb.seg.m.%d", &index)) {
	    gpcc_warning(FLINE, "segment name format error %s", segname);
	    return 1;
	}
	
	*isqd = 1;
	*idx = index;
	*primary = (index == 0) ? 1 : 0;
	return 0;
    } else {
	if (2 != sscanf(segname, "gpdb.seg.%d.%d", idx, primary)) {
	    gpcc_warning(FLINE, "segment name format error %s", segname);
	    return 1;
	}

	*isqd = 0;
	return 0;
    }

    return 0;
}

/* initialize */
static apr_status_t seg_initialize(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int e;
    gpcc_event_t *evt;

    memset(&cmdslot->u.seg, 0, sizeof(cmdslot->u.seg));
    if (0 != (e = seg_cmdname_parse(cmdslot->cmdname,
				    &cmdslot->u.seg.isqd,
				    &cmdslot->u.seg.idx,
				    &cmdslot->u.seg.primary))) {

	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpha.evq, evt);
	gpcc_warning(FLINE, "segname format error");
	return 0;
    }

    return 0;
}

/* cleanup */
static apr_status_t seg_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    /* TODO: send error request */

    return 0;
}


/* segment initdb */

static apr_status_t seg_initdb_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;
    char *initdbfn = 0;
    int pid = 0;
    struct json_object *seg = 0, *nodeobj = 0;
    char *node, *m_data_dir;
    char *source;
    char *user;
    char *host;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    cmdslot->u.seg.request.maidx = packet->mh.sndidx;
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;

    if (0 != (e = apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "apr_pool_create error");
	return 0;
    }

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "json_tokener_parse error");
	json_object_put(obj);
	return 0;
    }
    
    cmdslot->u.seg.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.seg.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.seg.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.seg.request.dbid = gpcc_json_object_get_int(obj, "dbid", 0);
    cmdslot->u.seg.request.content = gpcc_json_object_get_int(obj, "content", 0);

    if (cmdslot->u.seg.request.port == 0 || 
	cmdslot->u.seg.request.mode == 0 ||
	cmdslot->u.seg.request.data_dir == 0) {	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "parameter is missing");
	json_object_put(obj);
	return 0;
    }

    /* check environment variable */
    
    /* check initdb is valid */
    apr_filepath_merge(&initdbfn, gpha.gphome, "bin/gpsyncmaster", 0, cmdslot->u.seg.request.subpool);
    if (! gpcc_file_exists(initdbfn)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "gpsyncmaster not found");
	json_object_put(obj);
	return 0;
    }

    /* check data_dir exists */
    if (gpcc_file_directory_exists(cmdslot->u.seg.request.data_dir)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "data directory exists");
	json_object_put(obj);
	return 0;
    }	

    /* check database is running (port)*/
    if (0 == (e = gpcc_pg_pid_active(cmdslot->u.seg.request.port, &pid, cmdslot->u.seg.request.subpool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "gpsyncmaster is running");
	json_object_put(obj);
	return 0;	
    }


    if (0 == (seg = gpcc_json_object_get_object(obj, "gpdb.seg.m.0", 0))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "gpdb.seg.m.0 is missing");
	json_object_put(obj);
	return 0;
    }

    user = gpcc_json_object_get_string(obj, "gpdb.user", getenv("USER"));
    
    node = gpcc_json_object_get_string(seg, "node", 0);
    m_data_dir = gpcc_json_object_get_string(seg, "dir", 0);

    if (0 == (nodeobj = gpcc_json_object_get_object(obj, node, 0))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "%s is missing", node);
	json_object_put(obj);
	return 0;
    }
   
    host = json_object_get_string(json_object_array_get_idx(nodeobj, 0));

    source = apr_psprintf(cmdslot->u.seg.request.subpool, "%s@%s:%s", user, host, m_data_dir);
    if (0 != (e = gpcc_proc_scp(&cmdslot->u.seg.request.proc,
				cmdslot->u.seg.request.port,
				source,
				cmdslot->u.seg.request.data_dir,
				0, 
				cmdslot->u.seg.request.subpool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "scp failure");
	json_object_put(obj);
	return 0;
    }
				    
    cmdslot->u.seg.request.msg = obj;
    cmdslot->countdown = 1;
    
    return 0;
}

static apr_status_t seg_initdb_qe_and_master_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = 0;
    int e;
    char *initdbfn = 0;
    int pid = 0;
    int max_connect = 0;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    cmdslot->u.seg.request.maidx = packet->mh.sndidx;
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;

    if (0 != (e = apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "apr_pool_create error");
	return 0;
    }

    gpcc_log_info("json msg = %s", packet->payload);

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "json_tokener_parse error");
	return 0;
    }
    
    cmdslot->u.seg.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.seg.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.seg.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.seg.request.dbid = gpcc_json_object_get_int(obj, "dbid", -10);
    cmdslot->u.seg.request.content = gpcc_json_object_get_int(obj, "content", -10);

    if (cmdslot->u.seg.request.port == 0 || 
	cmdslot->u.seg.request.mode == 0 ||
	cmdslot->u.seg.request.data_dir == 0 ||
	cmdslot->u.seg.request.dbid == -10 || 
	cmdslot->u.seg.request.content == -10) {	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "parameter is missing");
	json_object_put(obj);
	return 0;
    }

    /* check environment variable */
    
    /* check initdb is valid */
    apr_filepath_merge(&initdbfn, gpha.gphome, "bin/initdb", 0, cmdslot->u.seg.request.subpool);
    if (! gpcc_file_exists(initdbfn)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "initdb not found");
	json_object_put(obj);
	return 0;
    }

    /* check data_dir exists */
    if (gpcc_file_directory_exists(cmdslot->u.seg.request.data_dir)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "[%s] data directory %s exists", 
			  cmdslot->cmdname, cmdslot->u.seg.request.data_dir);
	json_object_put(obj);
	return 0;
    }	

    /* check database is running (port)*/
    if (0 == (e = gpcc_pg_pid_active(cmdslot->u.seg.request.port, &pid, cmdslot->u.seg.request.subpool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "[%s] database is running", cmdslot->cmdname);
	json_object_put(obj);
	return 0;	
    }

    /* initdb start */
    max_connect = gpcc_json_object_get_int(obj, "gpdb.max_connect", 25);
    if (strstr(cmdslot->cmdname, "gpdb.seg.m.") == 0)  {
	/* QE max_connect */
	max_connect *= 5;
    }

    gpcc_log_info("[%s] start initdb", cmdslot->cmdname);
    if (0 != (e = gpcc_proc_initdb(&cmdslot->u.seg.request.proc,
				   cmdslot->u.seg.request.data_dir,
				   gpcc_json_object_get_string(obj, CONFIG_ENCODING_KEY, CONFIG_ENCODING_VALUE),
				   gpcc_json_object_get_string(obj, CONFIG_LOCALE_KEY, CONFIG_LOCALE_VALUE),
				   max_connect,
				   gpcc_json_object_get_string(obj, CONFIG_SHARED_BUFFERS_KEY, CONFIG_SHARED_BUFFERS_VALUE),
				   0,
				   cmdslot->u.seg.request.subpool))) {
	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "initdb failure");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.msg = obj;
    cmdslot->countdown = 1;

    return 0;
}

static apr_status_t seg_initdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    if (seg_is_standby_qd(cmdslot)) {
	return seg_initdb_standby_qd(ev, _cmdslot);
    } else {
	return seg_initdb_qe_and_master_qd(ev, _cmdslot);
    }
    
    return 0;
}


static apr_status_t seg_initdb_pg_hba_append(gpcc_event_t* ev, void *_cmdslot) 
{
#define MAX_HOSTS_PER_NODE  5

    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i, j, n, nnode;
    char nodename[64], segname[64];
    char **hosts;
    struct json_object *hostarray;

    if (seg_is_qd(cmdslot)) {
	char *user = gpcc_json_object_get_string(cmdslot->u.seg.request.msg, CONFIG_USER_KEY, getenv("USER"));

	hosts = apr_pcalloc(cmdslot->u.seg.request.subpool, sizeof(char *) * (2 * MAX_HOSTS_PER_NODE + 1));	
	n = 0;
	for (i = 0 ; i < 2 ; i++) {
	    struct json_object *seg;
	    char *nodestr;

	    sprintf(segname, "gpdb.seg.m.%d", i);
	    seg = gpcc_json_object_get_object(cmdslot->u.seg.request.msg, segname, 0);
	    if (!seg)
		break;

	    nodestr = gpcc_json_object_get_string(seg, "node", 0);

	    hostarray = gpcc_json_object_get_object(cmdslot->u.seg.request.msg, nodestr, 0);
	    
	    for (j = 1 ; j < json_object_array_length(hostarray) ; j++) {
		struct json_object *node = json_object_array_get_idx(hostarray, j);
		hosts[n++] = json_object_get_string(node);
	    }
	}

	return gpcc_master_pg_hba_append(user, cmdslot->u.seg.request.data_dir, (const char * const *) hosts, cmdslot->u.seg.request.subpool);	
    } else {

	/* find number of nodes */
	nnode = 0;
	while (1) {
	    sprintf(nodename, "node.%d", nnode);
	    if (gpcc_json_object_get_object(cmdslot->u.seg.request.msg, nodename, 0) == 0)
		break;
	    nnode++;
	}
	
	hosts = apr_pcalloc(cmdslot->u.seg.request.subpool, sizeof(char *) * (nnode * MAX_HOSTS_PER_NODE + 1));
	n = 0;
	for (i = 0 ; i < nnode ; i++) {
	    sprintf(nodename, "node.%d", i);
	    hostarray = gpcc_json_object_get_object(cmdslot->u.seg.request.msg, nodename, 0);
	    
	    for (j = 1 ; j < json_object_array_length(hostarray) ; j++) {
		struct json_object *node = json_object_array_get_idx(hostarray, j);
		hosts[n++] = json_object_get_string(node);
	    }
	}
	
	return gpcc_pg_hba_append(cmdslot->u.seg.request.data_dir, (const char * const *) hosts, cmdslot->u.seg.request.subpool);
    }

    return APR_EINVAL;
}

static apr_status_t seg_initdb_pg_conf_update(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_pg_conf_subst_t subst[5];

    if (seg_is_qd(cmdslot)) {
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
	
	/* gp_fault_action=readonly/continue/none */
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
    

    return gpcc_pg_update_postgresql_conf(cmdslot->u.seg.request.data_dir, subst, cmdslot->u.seg.request.subpool);
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
	gpcc_log_info("[%s] initdb done. retcode=%d", cmdslot->cmdname, ret);
	if (ret == 0) {
	    if (seg_is_standby_qd(cmdslot)) {
		/* this is standby QD */
		evt = gpcc_event_alloc(SEG_INITDB_DONE_EVENT, cmdslot->cmdidx, 0);
		gpcc_evqueue_put(gpha.evq, evt);		
		return 0;
	    }
	    
	    /* this is master QD or QEs */

	    /* update pg_hba.conf */
	    gpcc_log_info("[%s] update pg_hba.conf", cmdslot->cmdname);
	    if (0 != (e = seg_initdb_pg_hba_append(ev, _cmdslot))) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "update pg_hba.conf failure");
		return 0;
	    }

	    /* update postgresql.conf */
	    gpcc_log_info("[%s] update postgresql.conf", cmdslot->cmdname);
	    if (0 != (e = seg_initdb_pg_conf_update(ev, _cmdslot))) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "update postgresql.conf failure");
		return 0;
	    }

	    /* start database */
	    if (seg_is_master_qd(cmdslot)) {
		/* start QD in admin/utility mode */
		gpcc_log_info("[%s] start QD in admin mode", cmdslot->cmdname);
		if (0 != (e = gpcc_proc_pg_ctl_start_master(&cmdslot->u.seg.request.proc,
							    cmdslot->u.seg.request.port,
							    cmdslot->u.seg.request.data_dir,
							    1, 1, 0,
							    cmdslot->u.seg.request.subpool))) {
		    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "pg_ctl start failure");
		    return 0;
		}
	    } else {
		gpcc_log_info("[%s] start QE", cmdslot->cmdname);
		if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.seg.request.proc,
							     cmdslot->u.seg.request.port,
							     cmdslot->u.seg.request.data_dir,
							     0, 1, 0,
							     cmdslot->u.seg.request.subpool))) {
		    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "pg_ctl start failure");
		    return 0;
		}
	    }

	    evt = gpcc_event_alloc(SEG_INITDB_NEXT_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpha.evq, evt);
	    
	} else {
	    gpcc_log_error("[%s] failed to run initdb", cmdslot->cmdname);
	    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, ret, proc_exit_reason(exitwhy));
	}

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}

static apr_status_t seg_initdb_update_gp_id(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int nsegment = 0;
    char segname[32];
    char *gpname = gpcc_json_object_get_string(cmdslot->u.seg.request.msg, CONFIG_DBNAME_KEY, CONFIG_DBNAME_VALUE);

    while (1) {
	sprintf(segname, "gpdb.seg.%d.0", nsegment);
	if (gpcc_json_object_get_object(cmdslot->u.seg.request.msg, segname, 0) == 0) {
	    break;
	}
	nsegment++;
    }
    
    return gpcc_pg_update_gp_id(0,
				cmdslot->u.seg.request.port,
				gpname,
				nsegment,
				cmdslot->u.seg.request.dbid,
				cmdslot->u.seg.request.content,
				cmdslot->u.seg.request.subpool);
}



static apr_status_t seg_initdb_update_gp_conf_per_segment(gpcc_event_t *ev, void *_cmdslot, char *segname, int dbid, 
							  int content, char *primary, int port)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *msg = cmdslot->u.seg.request.msg;
    int e;
    struct json_object *seg, *hostarray;
    char *node;
    char *data_dir;
    char *host;

    if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
	gpcc_log_error("seg %s not found in config", segname);
	return 1;
    }

    node = gpcc_json_object_get_string(seg, "node", 0);
    data_dir = gpcc_json_object_get_string(seg, "dir", 0);
    hostarray = gpcc_json_object_get_object(msg, node, 0);
    host = json_object_get_string(json_object_array_get_idx(hostarray, 0));
    
    if (0 != (e = gpcc_pg_update_gp_configuration(0,
						  cmdslot->u.seg.request.port, 
						  dbid, 
						  content, 
						  primary,
						  host,
						  port, 
						  data_dir,
						  cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "gpcc_pg_update_gp_configuration failure");
	return e;
    }

    return 0;
}


static apr_status_t seg_initdb_update_gp_configuration(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *msg = cmdslot->u.seg.request.msg;
    int dbid = 1;
    int i, j, e;
    char segname[64];
    struct json_object *seg;
    int primary_port, mirror_port;
    int master_port, port;
    char *primary;
    int done = 0;

    if (! seg_is_qd(cmdslot))
	return 0;

    master_port = gpcc_json_object_get_int(msg, CONFIG_MASTER_PORT_KEY, CONFIG_MASTER_PORT_VALUE);
    primary_port = gpcc_json_object_get_int(msg, CONFIG_BASE_PORT_KEY, CONFIG_BASE_PORT_VALUE);
    mirror_port = gpcc_json_object_get_int(msg, CONFIG_MIRROR_BASE_PORT_KEY, CONFIG_MIRROR_BASE_PORT_VALUE);

    /* update gp_configuration table */

    /* master QD segment */
    if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, "gpdb.seg.m.0", dbid++, -1, "t", master_port))) {
	gpcc_log_error("[%s] update gp_configuration for entry gpdb.seg.m.0 failed", cmdslot->cmdname);
	return 1;
    }

    /* QEs segment */
    for (i = 0 ; ! done ; i++) {
	for (j = 0 ; j < 2 ; j++) {
	    sprintf(segname, "gpdb.seg.%d.%d", i, j);
	    if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
		if (j == 0) 
		    done = 1;
		break;
	    }

	    port = (j == 0) ? primary_port++ : mirror_port++;
	    primary = (j == 0) ? "t" : "f";

	    if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, segname, dbid++, i, primary, port))) {
		gpcc_log_error("[%s] update gp_configuration for entry %s failed", cmdslot->cmdname, segname);
		return 1;
	    }

	}
    }

    /* standby master */
    for (i = 1 ; 1 ; i++) {
	sprintf(segname, "gpdb.seg.m.%d", i);
	if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
	    break;
	}

	if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, segname, dbid++, -1, "f", master_port))) {
	    gpcc_log_error("[%s] update gp_configuration for entry %s failed", cmdslot->cmdname, segname);
	    return 1;
	}	
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
    char status[1024];
    int len = sizeof(status);

    e = apr_proc_wait(&cmdslot->u.seg.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	gpcc_log_info("[%s] start database done. retcode = %d", cmdslot->cmdname, ret);
	if (ret == 0) {
	    /* check pg_ctl status for sure */
	    gpcc_log_info("[%s] check pg_ctl status", cmdslot->cmdname);
	    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.seg.request.port,
					     cmdslot->u.seg.request.data_dir,
					     status, 
					     &len,
					     cmdslot->u.seg.request.subpool))) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "database is not running");
		return 0;
	    }

	    if (strstr(status, "server is running") == 0) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, 1, "server is not running");
		return 0;
	    }

	    /* update gp_id */
	    gpcc_log_info("[%s] update gp_id", cmdslot->cmdname);
	    if (0 != (e = seg_initdb_update_gp_id(ev, _cmdslot))) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "update gp_id table failure");
		return 0;
	    }

	    /* update gp_configuration */
	    gpcc_log_info("[%s] update gp_configuration", cmdslot->cmdname);
	    if (0 != (e = seg_initdb_update_gp_configuration(ev, _cmdslot))) {
		seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "update gp_configuration table failure");
		return 0;
	    }

	    evt = gpcc_event_alloc(SEG_INITDB_NEXT_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpha.evq, evt);
	    
	    gpcc_log_info("[%s] update gpconf done", cmdslot->cmdname);
	} else {
	    gpcc_log_error("[%s] failed to start database.", cmdslot->cmdname);
	    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, ret, proc_exit_reason(exitwhy));
	}

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}

/**
 *  backout1 -- backout after initdb
 *
 *  rm -f $DATAT_DIR 
 */
static apr_status_t seg_initdb_backout1(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    apr_proc_t proc;
    char cmd[1024];
    char *argv[2];
    int e;

    gpcc_log_info("[%s] initdb backout level 1 start", cmdslot->cmdname);
    gpcc_log_info("[%s] remove data directory %s", cmdslot->cmdname, cmdslot->u.seg.request.data_dir);
    sprintf(cmd, "rm -r %s", cmdslot->u.seg.request.data_dir);
    argv[0] = cmd;
    argv[1] = 0;

    if (0 != (e = gpcc_proc_run_shell(&proc, cmd, (const char *const *) argv, 0, 1, cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "failed to run command: %s", cmd);
    }

    return 0;
}

/**
 *  backout2 -- initdb success but start failure
 *
 *  rm -f /tmp/.s.PGSQL.$GP_PORT*
 *  rm -f $DATA_DIR.log
 *  pg_ctl stop
 */
static apr_status_t seg_initdb_backout2(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    apr_proc_t proc;
    char cmd[1024];
    char *argv[2];
    int e;

    gpcc_log_info("[%s] initdb backout level 2 start", cmdslot->cmdname);

    sprintf(cmd, "rm -f /tmp/.s.PGSQL.%d* ; rm -f %s.log", 
	    cmdslot->u.seg.request.port,
	    cmdslot->u.seg.request.data_dir);

    gpcc_log_info("[%s] Execute %s", cmdslot->cmdname, cmd);

    argv[0] = cmd;
    argv[1] = 0;

    if (0 != (e = gpcc_proc_run_shell(&proc, cmd, (const char *const *)argv, 0, 1, cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "failed to run command: %s", cmd);
    }
    
    gpcc_log_info("[%s] pg_ctl stop database", cmdslot->cmdname);

    if (0 != (e = gpcc_proc_pg_ctl_stop(&proc, cmdslot->u.seg.request.port,
					cmdslot->u.seg.request.data_dir,
					"immediate",
					1, 1, 
					cmdslot->u.seg.request.subpool))) {
	gpcc_warning(FLINE, "failed to stop database");
    }

    return 0;
}

static apr_status_t seg_initdb_checkdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    apr_proc_t proc;
    int e;
    char *user;
    char *passwd;
    gpcc_event_t *evt;

    if (seg_is_qd(cmdslot)) {
	/* this is QD */
	user = gpcc_json_object_get_string(cmdslot->u.seg.request.msg, CONFIG_USER_KEY, getenv("USER"));
	passwd = gpcc_json_object_get_string(cmdslot->u.seg.request.msg, CONFIG_PASSWORD_KEY, CONFIG_PASSWORD_VALUE);
	if (0 != (e = gpcc_pg_change_password(0,
					      cmdslot->u.seg.request.port,
					      user,
					      passwd,
					      cmdslot->u.seg.request.subpool))) {
	    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "change password failure");
	    return 0;
	}


    }

    /* stop database */
    if (0 != (e = gpcc_proc_pg_ctl_stop(&proc,
					cmdslot->u.seg.request.port,
					cmdslot->u.seg.request.data_dir,
					"immediate",
					1, 1,
					cmdslot->u.seg.request.subpool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "pg_ctl stop failure");
	return 0;
    }

    evt = gpcc_event_alloc(SEG_INITDB_DONE_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpha.evq, evt);

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
    return seg_request_reply(ev, _cmdslot, SEG_INITDB_REPLY_EVENT);
}


/* segment start */

static apr_status_t seg_start(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    cmdslot->u.seg.request.maidx = packet->mh.sndidx;
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;

    if (0 != (e = apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "apr_pool_create error");
	return 0;
    }
    
    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "json_tokener_parse error");
	return 0;
    }

    cmdslot->u.seg.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.seg.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.seg.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);

    if (cmdslot->u.seg.request.port == 0 ||
	cmdslot->u.seg.request.mode == 0 ||
	cmdslot->u.seg.request.data_dir == 0) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "request error: parameter is missing");
	json_object_put(obj);
	return 0;
    }

    
    if (! seg_is_qd(cmdslot)) {
	/* this is QE */

	gpcc_log_info("start segment %s"), gpcc_json_object_get_string(obj, "seg", "");
	if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.seg.request.proc,
						     cmdslot->u.seg.request.port,
						     cmdslot->u.seg.request.data_dir,
						     0, 1, 0, 
						     cmdslot->u.seg.request.subpool))) {
	    seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl start segment failure");
	    json_object_put(obj);
	    return 0;
	}
    } else {
	/* this is QD */
	if (0 != (e = gpcc_pg_log_rotate(cmdslot->u.seg.request.data_dir, cmdslot->u.seg.request.subpool))) {
	    ;
	}

	if (seg_is_master_qd(cmdslot)) {
	    int utility = 0;
	    /* start master in utility mode or production mode */
	    gpcc_log_info("start master %s in mode = %s", 
			  gpcc_json_object_get_string(obj, "seg", ""),
			  gpcc_json_object_get_string(obj, "mode", ""));
	    if (strcmp(gpcc_json_object_get_string(obj, "mode", "normal"), "utility") == 0)
		utility = 1;

	    if (0 != (e = gpcc_proc_pg_ctl_start_master(&cmdslot->u.seg.request.proc,
							cmdslot->u.seg.request.port,
							cmdslot->u.seg.request.data_dir,
							utility,
							1, 
							0, 
							cmdslot->u.seg.request.subpool))) {
		seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl start master failure");
		json_object_put(obj);
		return 0;
	    }

	} else {
	    if (0 != (e = gpcc_proc_gpsyncmaster_start(&cmdslot->u.seg.request.proc,
						       cmdslot->u.seg.request.port,
						       cmdslot->u.seg.request.data_dir,
						       0, 0, 
						       cmdslot->u.seg.request.subpool))) {
		seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "gpsyncmaster start failure");
		json_object_put(obj);
		return 0;
	    }

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
	if (ret == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	}
	cmdslot->u.seg.request.exitcode = ret;
	cmdslot->u.seg.request.reason = proc_exit_reason(exitwhy);	

	gpcc_log_info("seg_start exit = %d", ret);
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


static apr_status_t seg_start_reply(gpcc_event_t* ev, void *_cmdslot)
{
    /* send reply */ 
    return seg_request_reply(ev, _cmdslot, SEG_START_REPLY_EVENT);
}

/* segment stop */
static apr_status_t seg_stop(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;
    char status[1024];
    int len = sizeof(status);

    memset(&cmdslot->u.seg.request, 0, sizeof(cmdslot->u.seg.request));
    cmdslot->u.seg.request.maidx = packet->mh.sndidx;
    cmdslot->u.seg.request.parentidx = packet->mh.rcvidx;

    if (0 != (e = apr_pool_create(&cmdslot->u.seg.request.subpool, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "apr_pool_create error");
	return 0;
    }

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "json_tokenr_parse error");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.seg.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.seg.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.seg.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);

    if (cmdslot->u.seg.request.port == 0 ||
	cmdslot->u.seg.request.mode == 0 ||
	cmdslot->u.seg.request.data_dir == 0) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "request error: parameter is missing");
	json_object_put(obj);
	return 0;
    }
    
    /* check pg_ctl status for sure */
    gpcc_log_info("[%s] check pg_ctl status", cmdslot->cmdname);
    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.seg.request.port,
				     cmdslot->u.seg.request.data_dir,
				     status, 
				     &len,
				     cmdslot->u.seg.request.subpool))) {
	seg_request_error(FLINE, ev, _cmdslot, SUCCESS_EVENT, 0, "server is not running");
	json_object_put(obj);
	return 0;
    }
    
    if (strstr(status, "server is running") == 0) {
	seg_request_error(FLINE, ev, _cmdslot, SUCCESS_EVENT, 0, "server is not running");
	json_object_put(obj);
	return 0;
    }

    if (0 != (e = gpcc_proc_pg_ctl_stop(&cmdslot->u.seg.request.proc,
					cmdslot->u.seg.request.port,
					cmdslot->u.seg.request.data_dir,
					cmdslot->u.seg.request.mode,
					1, 0, 
					cmdslot->u.seg.request.subpool))) {

	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl stop failure");
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
	if (ret == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	}

	cmdslot->u.seg.request.exitcode = ret;
	cmdslot->u.seg.request.reason = proc_exit_reason(exitwhy);	
	gpcc_evqueue_put(gpha.evq, evt);

	gpcc_log_info("seg_stop_wait exit ret=%d reason =%s", ret, cmdslot->u.seg.request.reason);
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
    return seg_request_reply(ev, _cmdslot, SEG_STOP_REPLY_EVENT);
}


apr_status_t seg_busy_reply(gpcc_event_t *ev, void *_cmdslot, const char *status)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *msg;
    event_t etype;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    gpcc_net_packet_t *pkt;

    if (cmdslot->cmdstate == SEG_INIT_CMDSTATE) {
	msg = "segment is initializing";
    } else if (cmdslot->cmdstate == SEG_STOPPED_CMDSTATE) {
	msg = "segment stopped";
    } else if (cmdslot->cmdstate == SEG_STARTING_CMDSTATE) {
	msg = "segment is starting";
    } else if (cmdslot->cmdstate == SEG_RUNNING_CMDSTATE) {
	msg = "segment is running";
    } else if (cmdslot->cmdstate == SEG_STOPPING_CMDSTATE) {
	msg = "segment is stopping";
    } else {
	msg = "invalid segment state";
    }

    if (ev->etype == SEG_INITDB_EVENT) {
	etype = SEG_INITDB_REPLY_EVENT;
    } else if (ev->etype == SEG_START_EVENT) {
	etype = SEG_START_REPLY_EVENT;
    } else if (ev->etype == SEG_STOP_EVENT) {
	etype = SEG_STOP_REPLY_EVENT;
    } else {
	etype = SEG_STATUS_REPLY_EVENT;
    }

    obj = gpcc_json_segment_reply(cmdslot->cmdname, status, msg);
    objstr = json_object_to_json_string(obj);
    evt = gpcc_event_alloc(etype, packet->mh.rcvidx, sizeof(*packet) + strlen(objstr)+1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = etype;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = packet->mh.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);
    gpcc_evqueue_put(gpha.evq, evt);
    json_object_put(obj);
    return 0;
}

apr_status_t seg_busy_ok_reply(gpcc_event_t *ev, void *_cmdslot)
{
    seg_busy_reply(ev, _cmdslot, "ok");
    return 0;
}


apr_status_t seg_busy_fail_reply(gpcc_event_t *ev, void *_cmdslot)
{

    seg_busy_reply(ev, _cmdslot, "failure");
    return 0;
}

apr_status_t seg_status(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;
    char *state;
    gpcc_net_packet_t *pkt;

    /* send the current state */
    if (cmdslot->cmdstate == SEG_STOPPED_CMDSTATE) {
	state = "stopped";
    } else if (cmdslot->cmdstate == SEG_STARTING_CMDSTATE) {
	state = "starting";
    } else if (cmdslot->cmdstate == SEG_RUNNING_CMDSTATE) {
	state = "running";
    } else if (cmdslot->cmdstate == SEG_STOPPING_CMDSTATE) {
	state = "stopping";
    } else {
	state = "initializing";
    }


    obj = gpcc_json_status_reply(cmdslot->cmdname, "ok", "", state);
    objstr = json_object_to_json_string(obj);
    evt = gpcc_event_alloc(SEG_STATUS_REPLY_EVENT, packet->mh.rcvidx, sizeof(*packet) + strlen(objstr)+1);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = SEG_STATUS_REPLY_EVENT;
    pkt->mh.len = strlen(objstr)+1;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = packet->mh.sndidx;
    memcpy(pkt->payload, objstr, strlen(objstr)+1);
    gpcc_evqueue_put(gpha.evq, evt);
    json_object_put(obj);

    return 0;
}
