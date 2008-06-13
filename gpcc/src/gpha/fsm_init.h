
/* segment initdb */

static apr_status_t seg_initdb_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;
    char *initdbfn = 0;
    int pid = 0;

    struct json_object *seg = 0;
    char *host;

    /*
    char *m_data_dir;
    char *source;
    */

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "json_tokener_parse error");
	json_object_put(obj);
	return 0;
    }
    
    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.request.dbid = gpcc_json_object_get_int(obj, "dbid", 0);
    cmdslot->u.request.content = gpcc_json_object_get_int(obj, "content", 0);

    if (cmdslot->u.request.port == 0 || 
	cmdslot->u.request.mode == 0 ||
	cmdslot->u.request.data_dir == 0) {	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "parameter is missing");
	json_object_put(obj);
	return 0;
    }

    gpcc_log_info("[%s] initdb standby QD with port = %d, data_dir = %s and mode = %s", 
		  cmdslot->cmdname,
		  cmdslot->u.request.port,
		  cmdslot->u.request.data_dir,
		  cmdslot->u.request.mode);
		  
    /* check environment variable */
    
    /* check initdb is valid */
    gpcc_log_info("[%s] checking gpsyncmaster exists", cmdslot->cmdname);
    apr_filepath_merge(&initdbfn, gpha.gphome, "bin/gpsyncmaster", 0, cmdslot->pool);
    if (! gpcc_file_exists(initdbfn)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "gpsyncmaster not found");
	json_object_put(obj);
	return 0;
    }

    /* check data_dir exists */
    gpcc_log_info("[%s] checking data directory %s exists", cmdslot->cmdname, cmdslot->u.request.data_dir);
    if (gpcc_file_directory_exists(cmdslot->u.request.data_dir)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "data directory exists");
	json_object_put(obj);
	return 0;
    }	

    /* check database is running (port)*/
    gpcc_log_info("[%s] checking database is running", cmdslot->cmdname);
    if (0 == (e = gpcc_pg_pid_active(cmdslot->u.request.port, &pid, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "gpsyncmaster is running");
	json_object_put(obj);
	return 0;	
    }


    if (0 == (seg = gpcc_json_object_get_object(obj, "seg.m.0", 0))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "seg.m.0 is missing");
	json_object_put(obj);
	return 0;
    }

    host = gpcc_json_object_get_string(seg, "host", 0);

    /*
    m_data_dir = gpcc_json_object_get_string(seg, "dir", 0);
    gpcc_log_info("[%s] copy the master QD directory to standby QD", cmdslot->cmdname);
    source = apr_psprintf(cmdslot->pool, "%s:%s", host, m_data_dir);
    if (0 != (e = gpcc_proc_scp(&cmdslot->u.request.proc,
				source,
				cmdslot->u.request.data_dir,
				0, 
				cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "scp failure");
	json_object_put(obj);
	return 0;
    }
    */

    /* check ssh to master QD */
    gpcc_log_info("[%s] check ssh connection between standby QD and master QD", cmdslot->cmdname);
    if (0 != (e = gpcc_proc_ssh(&cmdslot->u.request.proc,
				host,
				"echo helloworld",
				0,
				cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "ssh master QD failure");
	json_object_put(obj);
	return 0;
    }	

    cmdslot->u.request.msg = obj;
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

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "json_tokener_parse error");
	return 0;
    }
    
    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.request.dbid = gpcc_json_object_get_int(obj, "dbid", -10);
    cmdslot->u.request.content = gpcc_json_object_get_int(obj, "content", -10);

    if (cmdslot->u.request.port == 0 || 
	cmdslot->u.request.mode == 0 ||
	cmdslot->u.request.data_dir == 0 ||
	cmdslot->u.request.dbid == -10 || 
	cmdslot->u.request.content == -10) {	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "parameter is missing");
	json_object_put(obj);
	return 0;
    }

    gpcc_log_info("[%s] initdb QE or Master QD with port = %d, data_dir = %s, mode = %s",
		  cmdslot->cmdname,
		  cmdslot->u.request.port,
		  cmdslot->u.request.data_dir,
		  cmdslot->u.request.mode);

    /* check environment variable */
    
    /* check initdb is valid */
    gpcc_log_info("[%s] checking initdb exists", cmdslot->cmdname);
    apr_filepath_merge(&initdbfn, gpha.gphome, "bin/initdb", 0, cmdslot->pool);
    if (! gpcc_file_exists(initdbfn)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "initdb not found");
	json_object_put(obj);
	return 0;
    }

    /* check data_dir exists */
    gpcc_log_info("[%s] checking data directory %s exists", cmdslot->cmdname, cmdslot->u.request.data_dir);
    if (gpcc_file_directory_exists(cmdslot->u.request.data_dir)) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "[%s] data directory %s exists", 
			  cmdslot->cmdname, cmdslot->u.request.data_dir);
	json_object_put(obj);
	return 0;
    }	

    /* check database is running (port)*/
    gpcc_log_info("[%s] checking database is running", cmdslot->cmdname);
    if (0 == (e = gpcc_pg_pid_active(cmdslot->u.request.port, &pid, cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, 1, "[%s] database is running", cmdslot->cmdname);
	json_object_put(obj);
	return 0;	
    }

    /* initdb start */
    max_connect = gpcc_json_object_get_int(obj, CONFIG_MAX_CONNECT_KEY, CONFIG_MAX_CONNECT_VALUE);
    if (strstr(cmdslot->cmdname, "seg.m.") == 0)  {
	/* QE max_connect */
	max_connect *= 5;
    }

    gpcc_log_info("[%s] running initdb", cmdslot->cmdname);
    if (0 != (e = gpcc_proc_initdb(&cmdslot->u.request.proc,
				   cmdslot->u.request.data_dir,
				   gpcc_json_object_get_string(obj, CONFIG_ENCODING_KEY, CONFIG_ENCODING_VALUE),
				   gpcc_json_object_get_string(obj, CONFIG_LOCALE_KEY, CONFIG_LOCALE_VALUE),
				   max_connect,
				   gpcc_json_object_get_string(obj, CONFIG_SHARED_BUFFERS_KEY, CONFIG_SHARED_BUFFERS_VALUE),
				   0,
				   cmdslot->pool))) {
	
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL0_EVENT, e, "initdb failure");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.request.msg = obj;
    cmdslot->countdown = 1;

    return 0;
}

static apr_status_t seg_initdb(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    gpcc_log_info("[%s] initdb request received", cmdslot->cmdname);
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

    gpcc_log_info("[%s] adding the entries in pg_hba.conf", cmdslot->cmdname);
    
    if (seg_is_qd(cmdslot)) {
	char *user = gpcc_json_object_get_string(cmdslot->u.request.msg, CONFIG_USER_KEY, (char *) gpha.user);

	hosts = apr_pcalloc(cmdslot->pool, sizeof(char *) * (2 * MAX_HOSTS_PER_NODE + 1));	
	n = 0;
	for (i = 0 ; i < 2 ; i++) {
	    struct json_object *seg;
	    char *nodestr;

	    sprintf(segname, "seg.m.%d", i);
	    seg = gpcc_json_object_get_object(cmdslot->u.request.msg, segname, 0);
	    if (!seg)
		break;

	    nodestr = gpcc_json_object_get_string(seg, "node", 0);

	    hostarray = gpcc_json_object_get_object(cmdslot->u.request.msg, nodestr, 0);
	    
	    for (j = 0 ; j < json_object_array_length(hostarray) ; j++) {
		struct json_object *node = json_object_array_get_idx(hostarray, j);
		hosts[n++] = json_object_get_string(node);
	    }
	}

	return gpcc_master_pg_hba_append(user, cmdslot->u.request.data_dir, (const char * const *) hosts, cmdslot->pool);	
    } else {

	/* find number of nodes */
	nnode = 0;
	while (1) {
	    sprintf(nodename, "node.%d", nnode);
	    if (gpcc_json_object_get_object(cmdslot->u.request.msg, nodename, 0) == 0)
		break;
	    nnode++;
	}
	
	hosts = apr_pcalloc(cmdslot->pool, sizeof(char *) * (nnode * MAX_HOSTS_PER_NODE + 1));
	n = 0;
	for (i = 0 ; i < nnode ; i++) {
	    sprintf(nodename, "node.%d", i);
	    hostarray = gpcc_json_object_get_object(cmdslot->u.request.msg, nodename, 0);
	    
	    for (j = 0 ; j < json_object_array_length(hostarray) ; j++) {
		struct json_object *node = json_object_array_get_idx(hostarray, j);
		hosts[n++] = json_object_get_string(node);
	    }
	}
	
	return gpcc_pg_hba_append(cmdslot->u.request.data_dir, (const char * const *) hosts, cmdslot->pool);
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
	subst[2].insert = apr_psprintf(cmdslot->pool, "port=%d", cmdslot->u.request.port);
	subst[3].pattern = "checkpoint_segments";
	subst[3].insert = apr_psprintf(cmdslot->pool, "checkpoint_segments=%d", 
				       gpcc_json_object_get_int(cmdslot->u.request.msg, CONFIG_CHECKPOINT_SEGMENT_KEY, CONFIG_CHECKPOINT_SEGMENT_VALUE));
	subst[4].pattern = 0;
	
	/* gp_fault_action=readonly/continue/none */
    } else {
	subst[0].pattern = "listen_addresses";
	subst[0].insert = "listen_addresses='*'";
	subst[1].pattern = "#port";
	subst[1].insert = apr_psprintf(cmdslot->pool, "port=%d", cmdslot->u.request.port);
	subst[2].pattern = "checkpoint_segments";
	subst[2].insert = apr_psprintf(cmdslot->pool, "checkpoint_segments=%d", 
				       gpcc_json_object_get_int(cmdslot->u.request.msg, CONFIG_CHECKPOINT_SEGMENT_KEY, CONFIG_CHECKPOINT_SEGMENT_VALUE));
	subst[3].pattern = 0;

    }
    

    return gpcc_pg_update_postgresql_conf(cmdslot->u.request.data_dir, subst, cmdslot->pool);
}

static apr_status_t seg_initdb_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.request.proc, &ret, &exitwhy, APR_NOWAIT);
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
		if (0 != (e = gpcc_proc_pg_ctl_start_master(&cmdslot->u.request.proc,
							    0,
							    cmdslot->u.request.port,
							    cmdslot->u.request.data_dir,
							    1, 1, 0,
							    cmdslot->pool))) {
		    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL1_EVENT, e, "pg_ctl start failure");
		    return 0;
		}
	    } else {
		gpcc_log_info("[%s] start QE", cmdslot->cmdname);
		if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.request.proc,
							     0,
							     cmdslot->u.request.port,
							     cmdslot->u.request.data_dir,
							     0, 1, 0,
							     cmdslot->pool))) {
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
    char *gpname = gpcc_json_object_get_string(cmdslot->u.request.msg, CONFIG_DBNAME_KEY, CONFIG_DBNAME_VALUE);

    while (1) {
	sprintf(segname, "seg.%d.0", nsegment);
	if (gpcc_json_object_get_object(cmdslot->u.request.msg, segname, 0) == 0) {
	    break;
	}
	nsegment++;
    }
    
    return gpcc_pg_update_gp_id(0,
				cmdslot->u.request.port,
				gpname,
				nsegment,
				cmdslot->u.request.dbid,
				cmdslot->u.request.content,
				cmdslot->pool);
}



static apr_status_t seg_initdb_update_gp_conf_per_segment(gpcc_event_t *ev, void *_cmdslot, char *segname, char *primary)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *msg = cmdslot->u.request.msg;
    int e;
    struct json_object *seg;
    char *data_dir;
    char *host;
    int port, dbid, content;

    if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
	gpcc_log_error("seg %s not found in config", segname);
	return 1;
    }

    host = gpcc_json_object_get_string(seg, "host", 0);
    data_dir = gpcc_json_object_get_string(seg, "dir", 0);
    port = gpcc_json_object_get_int(seg, "port", 0);
    dbid = gpcc_json_object_get_int(seg, "dbid", 0);
    content = gpcc_json_object_get_int(seg, "content", 0);
    
    if (0 != (e = gpcc_pg_update_gp_configuration(0,
						  cmdslot->u.request.port, 
						  dbid, 
						  content, 
						  primary,
						  host,
						  port, 
						  data_dir,
						  cmdslot->pool))) {
	gpcc_warning(FLINE, "gpcc_pg_update_gp_configuration failure");
	return e;
    }

    return 0;
}


static apr_status_t seg_initdb_update_gp_configuration(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *msg = cmdslot->u.request.msg;
    int i, j, e;
    char segname[64];
    struct json_object *seg;
    char *primary;
    int done = 0;

    if (! seg_is_qd(cmdslot))
	return 0;

    /* update gp_configuration table */

    /* master QD segment */
    if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, "seg.m.0", "t"))) {
	gpcc_log_error("[%s] update gp_configuration for entry seg.m.0 failed", cmdslot->cmdname);
	return 1;
    }

    /* QEs segment */
    for (i = 0 ; ! done ; i++) {
	for (j = 0 ; j < 2 ; j++) {
	    sprintf(segname, "seg.%d.%d", i, j);
	    if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
		if (j == 0) 
		    done = 1;
		break;
	    }

	    primary = (j == 0) ? "t" : "f";

	    if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, segname, primary))) {
		gpcc_log_error("[%s] update gp_configuration for entry %s failed", cmdslot->cmdname, segname);
		return 1;
	    }

	}
    }

    /* standby master */
    for (i = 1 ; 1 ; i++) {
	sprintf(segname, "seg.m.%d", i);
	if ((seg = gpcc_json_object_get_object(msg, segname, 0)) == 0){
	    break;
	}

	if (0 != (e = seg_initdb_update_gp_conf_per_segment(ev, _cmdslot, segname, "f"))) {
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

    e = apr_proc_wait(&cmdslot->u.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	gpcc_log_info("[%s] start database done. retcode = %d", cmdslot->cmdname, ret);
	if (ret == 0) {
	    /* check pg_ctl status for sure */
	    gpcc_log_info("[%s] check pg_ctl status", cmdslot->cmdname);
	    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.request.port,
					     cmdslot->u.request.data_dir,
					     status, 
					     &len,
					     cmdslot->pool))) {
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
    gpcc_log_info("[%s] remove data directory %s", cmdslot->cmdname, cmdslot->u.request.data_dir);
    sprintf(cmd, "rm -rf %s", cmdslot->u.request.data_dir);
    argv[0] = cmd;
    argv[1] = 0;

    if (0 != (e = gpcc_proc_run_shell(&proc, cmd, (const char *const *) argv, 0, 1, cmdslot->pool))) {
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
	    cmdslot->u.request.port,
	    cmdslot->u.request.data_dir);

    gpcc_log_info("[%s] Execute %s", cmdslot->cmdname, cmd);

    argv[0] = cmd;
    argv[1] = 0;

    if (0 != (e = gpcc_proc_run_shell(&proc, cmd, (const char *const *)argv, 0, 1, cmdslot->pool))) {
	gpcc_warning(FLINE, "failed to run command: %s", cmd);
    }
    
    gpcc_log_info("[%s] pg_ctl stop database", cmdslot->cmdname);

    if (0 != (e = gpcc_proc_pg_ctl_stop(&proc, 
					0,
					cmdslot->u.request.port,
					cmdslot->u.request.data_dir,
					"immediate",
					1, 1, 
					cmdslot->pool))) {
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
	user = gpcc_json_object_get_string(cmdslot->u.request.msg, CONFIG_USER_KEY, (char *) gpha.user);
	passwd = gpcc_json_object_get_string(cmdslot->u.request.msg, CONFIG_PASSWORD_KEY, CONFIG_PASSWORD_VALUE);
	if (0 != (e = gpcc_pg_change_password(0,
					      cmdslot->u.request.port,
					      user,
					      passwd,
					      cmdslot->pool))) {
	    seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "change password failure");
	    return 0;
	}


    }

    /* stop database */
    if (0 != (e = gpcc_proc_pg_ctl_stop(&proc,
					0,
					cmdslot->u.request.port,
					cmdslot->u.request.data_dir,
					"immediate",
					1, 1,
					cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SEG_INITDB_FAIL2_EVENT, e, "pg_ctl stop failure");
	return 0;
    }

    evt = gpcc_event_alloc(SEG_INITDB_DONE_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpha.evq, evt);

    gpcc_log_info("[%s] initdb finished successfully", cmdslot->cmdname);
    
    return 0;
}

static apr_status_t seg_initdb_reply(gpcc_event_t* ev, void *_cmdslot)
{
    return seg_request_reply(ev, _cmdslot, SEG_INITDB_REPLY_EVENT);
}

