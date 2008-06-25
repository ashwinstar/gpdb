
apr_status_t seg_delete(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;
    char status[1024];
    int len = sizeof(status);

    gpcc_log_info("[%s] segment delete command received", cmdslot->cmdname);

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "json_tokenr_parse error");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);

    if (cmdslot->u.request.port == 0 ||
	cmdslot->u.request.mode == 0 ||
	cmdslot->u.request.data_dir == 0) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "request error: parameter is missing");
	json_object_put(obj);
	return 0;
    }
    
    /* check pg_ctl status for sure */
    gpcc_log_info("[%s] check pg_ctl status", cmdslot->cmdname);
    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.request.port,
				     cmdslot->u.request.data_dir,
				     status, 
				     &len,
				     cmdslot->pool))) {
	seg_request_error(FLINE, ev, _cmdslot, SUCCESS_EVENT, 0, "server is not running");
	cmdslot->u.request.msg = obj;
	return 0;
    }
    
    if (strstr(status, "server is running") == 0) {
	seg_request_error(FLINE, ev, _cmdslot, SUCCESS_EVENT, 0, "server is not running");
	cmdslot->u.request.msg = obj;
	return 0;
    }

    if (0 != (e = gpcc_proc_pg_ctl_stop(&cmdslot->u.request.proc,
					0,
					cmdslot->u.request.port,
					cmdslot->u.request.data_dir,
					cmdslot->u.request.mode,
					1, 0, 
					cmdslot->pool))) {

	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl stop failure");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.request.msg = obj;
    cmdslot->countdown = 1;
    return 0;    


}

static apr_status_t seg_delete_wait(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	}

	cmdslot->u.request.exitcode = ret;
	cmdslot->u.request.reason = proc_exit_reason(exitwhy);	
	gpcc_evqueue_put(gpha.evq, evt);

    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}

static apr_status_t seg_delete_rmdir(gpcc_event_t *ev, void *_cmdslot) 
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    apr_proc_t proc;
    char cmd[1024];
    const char *argv[2];
    int e;

    gpcc_log_info("[%s] removing data directory '%s'", cmdslot->cmdname, cmdslot->u.request.data_dir);
    sprintf(cmd, "rm -rf %s && if [ -f %s.log ] ; then rm -rf %s.log; fi", 
	    cmdslot->u.request.data_dir, 
	    cmdslot->u.request.data_dir,
	    cmdslot->u.request.data_dir);
    argv[0] = cmd;
    argv[1] = 0;
    if ((e = gpcc_proc_run_shell(&proc, argv[0], argv, 0, 1, cmdslot->pool)) != 0) {
	gpcc_warning(FLINE, "[%s] remove directory '%s' failure", 
		     cmdslot->cmdname, cmdslot->u.request.data_dir);
	cmdslot->u.request.exitcode = e;
	cmdslot->u.request.reason = "remove data directory failure";
    }

    gpcc_log_info("[%s] segment delete exited [retval = %d, reason =%s]", 
		  cmdslot->cmdname, 
		  cmdslot->u.request.exitcode,
		  cmdslot->u.request.reason);

    return 0;
}

static apr_status_t seg_delete_reply(gpcc_event_t* ev, void *_cmdslot)
{
    return seg_request_reply(ev, _cmdslot, SEG_DELETE_REPLY_EVENT);
}


