static apr_status_t a_array_sync_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    int i, e;
    int standby_qd_idx, master_qd_idx;
    char cmd[2048];

    for (i = 0 ; i < nseginfo ; i++) {
	const seginfo_t *info = &seginfo[i];
	if (strcmp(info->name, "seg.m.1") == 0)
	    break;
    }
    if (i >= nseginfo)
	return 0;

    standby_qd_idx = i;


    for (i = 0 ; i < nseginfo ; i++) {
	const seginfo_t *info = &seginfo[i];
	if (strcmp(info->name, "seg.m.0") == 0)
	    break;
    }
    if (i >= nseginfo)
	return APR_EINVAL;

    master_qd_idx = i;

    /* ssh master_host scp src_dir standby_host:dst_dst */
    gpcc_log_info("start to sync data from master QD to standby QD");
    sprintf(cmd, "scp -r %s %s:%s", seginfo[master_qd_idx].dir,
	    seginfo[standby_qd_idx].host,
	    seginfo[standby_qd_idx].dir);
    
    e = gpcc_proc_ssh(&cmdslot->u.array_init.proc, 
		      seginfo[master_qd_idx].host, 
		      cmd, 
		      0, 
		      cmdslot->pool);
    if (e) {
	gpcc_log_error("copy data from master QD datadir to standby QD datadir failed");
	return e;
    }

    cmdslot->countdown = 1;
    return 0;
}

static apr_status_t a_array_wait_sync_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
   cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int ret = 0;
    apr_exit_why_e exitwhy;
    int e;
    gpcc_event_t *evt;

    e = apr_proc_wait(&cmdslot->u.array_init.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	gpcc_net_packet_t *pkt;
	struct json_object *obj;
	char *objstr;

	if (ret == 0) {
	    obj = gpcc_json_http_reply(gpma.array_name, 0, "init", "ok", 0, "");
	    objstr = json_object_to_json_string(obj);
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, sizeof(*pkt) + strlen(objstr)+1);
	    gpcc_log_info("init array succeeded.");
	} else {
	    obj = gpcc_json_http_reply(gpma.array_name, 0, "init", "error", 0, "standby QD sync failure");
	    objstr = json_object_to_json_string(obj);
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, sizeof(*pkt) + strlen(objstr)+1);
	    gpcc_warning(FLINE, "[Error] init array failed: status='error', reason='standby QD sync failure'");
	}
	
	pkt = (gpcc_net_packet_t *) evt->pdata;
	pkt->mh.type = UNITS_REPLY_MSG_EVENT;
	pkt->mh.len = strlen(objstr)+1;
	pkt->mh.sndidx = cmdslot->cmdidx;
	pkt->mh.rcvidx = cmdslot->cmdidx;
	memcpy(pkt->payload, objstr, strlen(objstr) +1);
	
	json_object_put(obj);
	gpcc_evqueue_put(gpma.evq, evt);
    } else {
	/* continue to wait */
	cmdslot->countdown = 1;
    }

    return 0;
}

static apr_status_t a_array_init_units_init_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);

    if (strcmp(status, OK_STATUS) == 0) {
	seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
	int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
	int i;

	for (i = 0 ; i < nseginfo ; i++) {
	    const seginfo_t *info = &seginfo[i];
	    if (strcmp(info->name, "seg.m.1") == 0)
		break;
	}
	if (i >= nseginfo) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, pkt->mh.len);
	    memcpy(evt->pdata, pkt->payload, pkt->mh.len);
	} else {
	    /* start sync master QD to standby QD */
	    evt = gpcc_event_alloc(CONTINUE_EVENT, cmdslot->cmdidx, 0);
	}

    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
	gpcc_warning(FLINE, "[Error] init array failed: status=%s, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_init_do(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    gpcc_event_t *evt;
    int i;
    
    p = cmdslot_alloc("INITDB", FTS_INITDB_CMDSTATE);
    p->u.initdb.parent_cmdidx = cmdslot->cmdidx;
    p->u.initdb.ntotal = nseginfo;

    p->u.initdb.req = apr_palloc(p->pool, sizeof(*p->u.initdb.req) * p->u.initdb.ntotal);
    for (i = 0 ; i < p->u.initdb.ntotal ; i++) {
	const seginfo_t *info = &seginfo[i];
	p->u.initdb.req[i].node = apr_pstrdup(p->pool, info->ha);
	p->u.initdb.req[i].unit = apr_pstrdup(p->pool, info->name);
	p->u.initdb.req[i].mode = "";
	p->u.initdb.req[i].host = apr_pstrdup(p->pool, info->host);
	p->u.initdb.req[i].port = info->port;
	p->u.initdb.req[i].datadir = apr_pstrdup(p->pool, info->dir);
	p->u.initdb.req[i].dbid = info->dbid;
	p->u.initdb.req[i].content = info->content;
	p->u.initdb.req[i].reply = 0;
	p->u.initdb.req[i].status = 0;
    }

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_init_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_init.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_init.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_init_success(gpcc_event_t *ev, void *_cmdslot)
{
    a_array_init_reply(ev, _cmdslot);
    return 0;
}

static apr_status_t a_array_init_failure(gpcc_event_t *ev, void *_cmdslot)
{
    a_array_init_reply(ev, _cmdslot);
    return 0;
}

