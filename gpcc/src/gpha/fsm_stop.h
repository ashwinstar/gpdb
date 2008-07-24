
/* segment stop */
static apr_status_t seg_stop(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;
    char status[1024];
    int len = sizeof(status);

    gpcc_log_info("[%s] segment stop command received", cmdslot->cmdname);

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "json_tokenr_parse error");
	json_object_put(obj);
	return 0;
    }

    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.request.isprimary = gpcc_json_object_get_string(obj, "isprimary", 0);

    if (cmdslot->u.request.port == 0 ||
	cmdslot->u.request.mode == 0 ||
	cmdslot->u.request.data_dir == 0 ||
        cmdslot->u.request.isprimary == 0) {
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
	json_object_put(obj);
	return 0;
    }
    
    if (strstr(status, "server is running") == 0) {
	seg_request_error(FLINE, ev, _cmdslot, SUCCESS_EVENT, 0, "server is not running");
	json_object_put(obj);
	return 0;
    }

    if (seg_is_qd(cmdslot) && strcmp(cmdslot->u.request.isprimary, "f") == 0) {
	apr_proc_t proc;
	char *argv[2];

	gpcc_log_info("[%s] kill gpsyncmaster", cmdslot->cmdname);
	argv[0] = "killall -9 gpsyncmaster || pkill -9 gpsyncmaster || true";
	argv[1] = 0;
	if (0 != (e = gpcc_proc_run_shell(&cmdslot->u.request.proc,
					  argv[0],
					  (const char *const *) argv, 0, 0, cmdslot->pool))) {
		seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "killall gpsyncmaster failure");
		json_object_put(obj);
		return 0;
	}
    } else {

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
    }
    
    cmdslot->u.request.msg = obj;
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

	gpcc_log_info("[%s] segment stop exited [retval = %d, reason =%s]", cmdslot->cmdname, ret, cmdslot->u.request.reason);

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
