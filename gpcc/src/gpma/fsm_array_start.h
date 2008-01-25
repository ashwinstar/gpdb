

static apr_status_t a_array_start_units_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", "failure");

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
	gpcc_warning(FLINE, "[Error] start utility qd failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_start_units_stop_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *pkt;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    pkt = (gpcc_net_packet_t *) ev->pdata;
    obj = json_tokener_parse(pkt->payload);
    status = gpcc_json_object_get_string(obj, "status", "failure");

    if (strcmp(status, "ok") == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, pkt->mh.len);
	memcpy(evt->pdata, pkt->payload, pkt->mh.len);
	gpcc_warning(FLINE, "[Error] start utility qd failed: status=%d, error=%s", status, pkt->payload);
    }
    
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_array_start_utility_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("START_UTILITY_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "utility";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].isprimary = seginfo->isprimary;
    p->u.start.req[0].valid = seginfo->valid;
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static int a_array_start_findIdxFromSegInfo(seginfo_t *seginfo, int nseginfo, int port, char *datadir, char definedprimary, char *host)
{
    int i;
    
    gpcc_log_info("search port=%d, dir=%s, definedprimary=%c, host=%s", port, datadir, definedprimary, host);
    for (i = 0 ; i < nseginfo ; i++) {
	if (seginfo[i].port == port && 
	    strcmp(seginfo[i].dir, datadir) == 0 && 
	    seginfo[i].definedprimary == definedprimary && 
	    strcmp(seginfo[i].host, host) == 0)
	    return i;
    }

    return -1;
}

static apr_status_t a_array_start_gpconf(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    seginfo_t *seginfo;
    int i, e, nseginfo, idx;
    gpcc_event_t *evt;
    char line[1024], *last;
    apr_proc_t proc;

    seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;

    if (apr_strnatcasecmp(cmdslot->u.array_start.mode, "force") == 0) {
	if (0 != (e = gpcc_pg_reset_invalid_segment(seginfo->host, seginfo->port, cmdslot->pool))) {
	    gpcc_warning(FLINE, "gpcc_pg_reset_invalid_segment failed");
	    evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	    return 0;
	}
    }

    /* check for valid segment */
    if ((e = gpcc_proc_psql(&proc, seginfo->host, seginfo->port, "template1", 
			    "select * from gp_configuration order by dbid", 
			    1, 1, cmdslot->pool)) != 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    gpcc_log_info("number of segment = %d", nseginfo);
    gpcc_log_info("name | host | dbid | content | port | dir | haidx | ha | definedprimary | isprimary | valid");
    for (i = 0 ; i < nseginfo ; i++) {
	gpcc_log_info("%s | %s | %d | %d | %d | %s | %d | %s | %c | %c | %c",
		      seginfo[i].name,
		      seginfo[i].host,
		      seginfo[i].dbid,
		      seginfo[i].content,
		      seginfo[i].port,
		      seginfo[i].dir,
		      seginfo[i].haidx,
		      seginfo[i].ha,
		      seginfo[i].definedprimary,
		      seginfo[i].isprimary,
		      seginfo[i].valid);
    }


    for (i = 0; apr_file_gets(line, sizeof(line), proc.out) == 0 ; i++) {
	char *content, *definedprimary, *dbid, *isprimary, *valid, *hostname, *port, *datadir;
	
	content = apr_strtok(line, "|", &last);
	definedprimary = apr_strtok(0, "|", &last);
	dbid = apr_strtok(0, "|", &last);
	isprimary = apr_strtok(0, "|", &last);
	valid = apr_strtok(0, "|", &last);
	hostname = apr_strtok(0, "|", &last);
	port = apr_strtok(0, "|", &last);
	datadir = apr_strtok(0, "|\r\n", &last);

	idx = a_array_start_findIdxFromSegInfo(seginfo, nseginfo, strtol(port, 0, 0), datadir, *definedprimary, hostname);
	if (idx < 0) {
	    gpcc_fatal(FLINE, "gp_configuration not match with gpcc config error");
	    return 0;
	}

	seginfo[idx].content = strtol(content, 0, 0);
	seginfo[idx].dbid = strtol(dbid, 0, 0);
	seginfo[idx].isprimary = *isprimary;
	seginfo[idx].valid = *valid;

    }


    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}

static apr_status_t a_array_start_utility_qd_stop(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("STOP_UTILITY_QD", FTS_STOP_CMDSTATE);
    p->u.stop.parent_cmdidx = cmdslot->cmdidx;
    p->u.stop.ntotal = 1;
    p->u.stop.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.stop.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.stop.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.stop.req[0].mode = "immediate";
    p->u.stop.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.stop.req[0].port = seginfo->port;
    p->u.stop.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.stop.req[0].isprimary = seginfo->isprimary;
    p->u.stop.req[0].valid = seginfo->valid;
    p->u.stop.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}


static apr_status_t a_array_start_qes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;
    int i, j;


    p = cmdslot_alloc("START_QES", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;

    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req) * gpma.fts_cmdslot->u.fts.nseginfo);
    for (i = 0, j = 0 ; i < gpma.fts_cmdslot->u.fts.nseginfo ; i++) {
	const seginfo_t *info = &seginfo[i];

	if (info->content != -1) {
	    if (info->valid == 't') {
		p->u.start.req[j].node = apr_pstrdup(p->pool, info->ha);
		p->u.start.req[j].unit = apr_pstrdup(p->pool, info->name);
		p->u.start.req[j].mode = "normal";
		p->u.start.req[j].host = apr_pstrdup(p->pool, info->host);
		p->u.start.req[j].port = info->port;
		p->u.start.req[j].datadir = apr_pstrdup(p->pool, info->dir);
		p->u.start.req[j].isprimary = info->isprimary;
		p->u.start.req[j].valid = info->valid;
		p->u.start.req[j].reply = 0;
		j++;
	    } else {
		int k;

		for (k = 0 ; k < gpma.unit_max ; k++) {
		    if (strcmp(info->name, gpma.unit_cmdtab[k]->cmdname) == 0)
			break;
		}
		if (k >= gpma.unit_max) {
		    gpcc_warning(FLINE, "[Error] invalid segment name %s", info->name);
		    continue;
		}
		evt = gpcc_event_alloc(INVALID_EVENT, gpma.unit_cmdtab[k]->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
	    }
	}
    }
    p->u.start.ntotal = j;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}

static apr_status_t a_array_start_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    seginfo_t *seginfo = gpma.fts_cmdslot->u.fts.seginfo;
    gpcc_event_t *evt;

    p = cmdslot_alloc("START_PRODUCTION_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "normal";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].isprimary = seginfo->isprimary;
    p->u.start.req[0].valid = seginfo->valid;
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

static apr_status_t a_array_start_standby_qd(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    cmdslot_t *p;
    int nseginfo = gpma.fts_cmdslot->u.fts.nseginfo;
    seginfo_t *seginfo = &gpma.fts_cmdslot->u.fts.seginfo[nseginfo-1];
    gpcc_event_t *evt;

    if (strstr(seginfo->name, "gpdb.seg.m.") == 0) {
	struct json_object *obj;
	char *msg = 0;
	obj = gpcc_json_http_reply("start", "ok", 0);
	msg = json_object_to_json_string(obj);
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, strlen(msg)+1);
	memcpy(evt->pdata, msg, strlen(msg)+1);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    p = cmdslot_alloc("START_STANDBY_QD", FTS_START_CMDSTATE);
    p->u.start.parent_cmdidx = cmdslot->cmdidx;
    p->u.start.ntotal = 1;
    p->u.start.req = apr_palloc(p->pool, sizeof(*p->u.start.req));
    p->u.start.req[0].node = apr_pstrdup(p->pool, seginfo->ha);
    p->u.start.req[0].unit = apr_pstrdup(p->pool, seginfo->name);
    p->u.start.req[0].mode = "normal";
    p->u.start.req[0].host = apr_pstrdup(p->pool, seginfo->host);
    p->u.start.req[0].port = seginfo->port;
    p->u.start.req[0].datadir = apr_pstrdup(p->pool, seginfo->dir);
    p->u.start.req[0].isprimary = seginfo->isprimary;
    p->u.start.req[0].valid = seginfo->valid;
    p->u.start.req[0].reply = 0;

    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;

}


static apr_status_t a_array_start_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt;

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, cmdslot->u.array_start.parent_cmdidx, sizeof(*pkt) + ev->edatasz);
    pkt = (gpcc_net_packet_t *) evt->pdata;
    pkt->mh.type = ARRAY_REPLY_MSG_EVENT;
    pkt->mh.len = ev->edatasz;
    pkt->mh.sndidx = cmdslot->cmdidx;
    pkt->mh.rcvidx = cmdslot->u.array_start.parent_cmdidx;
    memcpy(pkt->payload, ev->pdata, ev->edatasz);   

    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_array_start_success(gpcc_event_t *ev, void *_cmdslot)
{
    /*    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */
 
    a_array_start_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_start.conn, 200, "OK", ev->pdata); */
    return 0;
}

static apr_status_t a_array_start_failure(gpcc_event_t *ev, void *_cmdslot)
{
    /*    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot; */

    a_array_start_reply(ev, _cmdslot);
    /*    gpcc_net_http_close(cmdslot->u.array_start.conn, 200, "failure", ev->pdata); */
    return 0;
}

static apr_status_t a_array_start_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    return 0;
}

