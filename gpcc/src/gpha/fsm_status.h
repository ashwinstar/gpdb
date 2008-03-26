
/* ONLY CALLED WHEN SEGMENT IS UP */
apr_status_t seg_status(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    gpcc_net_packet_t *packet = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = 0;
    char *objstr;
    gpcc_event_t *evt;
    char *state, *health, *alert;
    gpcc_net_packet_t *pkt;
    int e;
    char status[1024];
    int len = sizeof(status);

    health = "failed";
    state = "stopped";
    alert = "";

    obj = json_tokener_parse(packet->payload);
    if (!obj) {
	health = "failed";
	state = "unknown";
	alert = "invalid json format";
	goto finish;
    }

    cmdslot->u.request.port = gpcc_json_object_get_int(obj, "port", 0);
    cmdslot->u.request.data_dir = gpcc_json_object_get_string(obj, "dir", 0);

    if (cmdslot->u.request.port == 0 ||
	cmdslot->u.request.data_dir == 0) {
	health = "failed";
	state = "unknown";
	alert = "invalid parameter";
	goto finish;
    }

    /* check pg_ctl status for sure */
    if (0 != (e = gpcc_pg_ctl_status(cmdslot->u.request.port,
				     cmdslot->u.request.data_dir,
				     status, 
				     &len,
				     cmdslot->pool))) {
	health = "failed";
	state = "stopped";
	alert = "pg_ctl status failed";
	goto finish;
    }

    health = "ok";
    if (strstr(status, "server is running")) {
	state = "running";
    } else {
	state = "stopped";
    }
    

    finish:

    if (obj) json_object_put(obj);

    obj = gpcc_json_status_reply(cmdslot->cmdname, health, alert, state);
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


