
/* segment start */

static apr_status_t seg_start(gpcc_event_t* ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    int e;

    gpcc_log_info("[%s] segment start command received", cmdslot->cmdname);

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "json_tokener_parse error");
	return 0;
    }

    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.mode = gpcc_json_object_get_string(obj, "mode", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);
    cmdslot->u.request.isprimary = gpcc_json_object_get_string(obj, "isprimary", 0);
    cmdslot->u.request.rotate_log = gpcc_json_object_get_string(obj, "rotate_log", 0);

    if (cmdslot->u.request.port == 0 ||
	cmdslot->u.request.mode == 0 ||
	cmdslot->u.request.data_dir == 0 ||
	cmdslot->u.request.isprimary == 0) {
	seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, 1, "request error: parameter is missing");
	json_object_put(obj);
	return 0;
    }

    if (cmdslot->u.request.rotate_log && 0 != (e = gpcc_pg_log_rotate(cmdslot->u.request.data_dir, cmdslot->pool))) {
	;
    }

    if (! seg_is_qd(cmdslot)) {
	/* this is QE */

	gpcc_log_info("[%s] starting segment", gpcc_json_object_get_string(obj, "seg", ""));
	if (0 != (e = gpcc_proc_pg_ctl_start_segment(&cmdslot->u.request.proc,
						     0,
						     cmdslot->u.request.port,
						     cmdslot->u.request.data_dir,
						     0, 1, 0, 
						     cmdslot->pool))) {
	    seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl start segment failure");
	    json_object_put(obj);
	    return 0;
	}
    } else {
	/* this is QD */

	if (seg_is_qd(cmdslot) && strcmp(cmdslot->u.request.isprimary, "t") == 0) {
	    int utility = 0;

	    /* TODO: check if gpsycnmaster is running. if so, kill it before start postmaster */
	    if (! seg_is_master_qd(cmdslot)) {
		apr_proc_t proc;
		char *argv[2];

		gpcc_log_info("kill gpsyncmaster before starting master QD");
		argv[0] = "killall gpsyncmaster || pkill gpsyncmaster";
		argv[1] = 0;
		gpcc_proc_run_shell(&proc, argv[0], (const char *const *)argv, 0, 1, cmdslot->pool);
	    }

	    /* start master in utility mode or production mode */
	    gpcc_log_info("[%s] start master in mode = %s", 
			  gpcc_json_object_get_string(obj, "seg", ""),
			  gpcc_json_object_get_string(obj, "mode", ""));
	    if (strcmp(gpcc_json_object_get_string(obj, "mode", "normal"), "utility") == 0)
		utility = 1;

	    if (0 != (e = gpcc_proc_pg_ctl_start_master(&cmdslot->u.request.proc,
							0,
							cmdslot->u.request.port,
							cmdslot->u.request.data_dir,
							utility,
							1, 
							0, 
							cmdslot->pool))) {
		seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "pg_ctl start master failure");
		json_object_put(obj);
		return 0;
	    }

	} else {
	    if (0 != (e = gpcc_proc_gpsyncmaster_start(&cmdslot->u.request.proc,
						       cmdslot->u.request.port,
						       cmdslot->u.request.data_dir,
						       0, 0, 
						       cmdslot->pool))) {
		seg_request_error(FLINE, ev, _cmdslot, FAILURE_EVENT, e, "gpsyncmaster start failure");
		json_object_put(obj);
		return 0;
	    }

	}
    }	

    cmdslot->u.request.msg = obj;
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

    e = apr_proc_wait(&cmdslot->u.request.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	}
	cmdslot->u.request.exitcode = ret;
	cmdslot->u.request.reason = proc_exit_reason(exitwhy);	

	gpcc_log_info("[%s] segment start exited [retval = %d]", cmdslot->cmdname, ret);
	gpcc_evqueue_put(gpha.evq, evt);

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
