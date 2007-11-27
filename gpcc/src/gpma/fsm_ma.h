

apr_status_t a_ma_connect(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    const char* argv[1];
    int argc;
    int hc;
    const char* hname[2];
    const char* hvalue[2];
    int e;
    char naddr[200];
    char buf[64];

    sprintf(buf, "GPMA:%s", gpma.ma_unit);
    argc = 1;
    argv[0] = GPCC_NET_PACKET_PROTO;
    hc = 2;
    hname[0] = "X-GPCC-ROLE";
    hvalue[0] = buf;
    hname[1] = "X-GPCC-GUID";
    hvalue[1] = gpma.guid;

    if (sizeof(naddr) - 1 
	== apr_snprintf(naddr, sizeof(naddr), "%s:%d", 
			slot->u.ma.node->u.node.hosttab[slot->u.ma.node->u.node.hostidx],
			slot->u.ma.port)) {
	gpcc_fatal(FLINE, "address line too long %s:%d", 
		   slot->u.ma.node->u.node.hosttab[slot->u.ma.node->u.node.hostidx],
		   slot->u.ma.port);
    }

    gpcc_log_info("gpcc_net_open %s", buf);
    /* use gpcc_net to connect to gpma here */
    if (0 != (e = gpcc_net_open(&slot->u.ma.self_conn, 
				naddr,
				argc, argv,
				hc, hname, hvalue, slot->cmdidx))) 
	return -1;


    return 0;
}

apr_status_t a_ma_ready(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    gpcc_net_conn_t conn;

    slot->countdown = 0;
    memcpy(&conn, ev->pdata, sizeof(conn));
    
    switch (ev->etype) {
    case NET_OPENED_EVENT:
	gpcc_log_info("------------ MA %s OPENED", slot->cmdname);
	break;
    case NET_ACCEPTED_EVENT:
	gpcc_log_info("------------ MA %s ACCEPTED", slot->cmdname);
	slot->u.ma.peer_conn = conn;
	break;
    default:
	break;
    }

    if (!slot->u.ma.conn) {
	if (ev->etype == NET_ACCEPTED_EVENT) {
	    gpcc_net_header_t hdr;
	    int e;
	    hdr.argc = 1;
	    hdr.argv[0] = GPCC_NET_PACKET_PROTO;
	    hdr.hc = 0;
	    hdr.postlen = 0;
	    if (0 != (e = gpcc_net_send_header(conn, &hdr))) {
		gpcc_warning(FLINE, "gpcc_net_send header failed");
		return 0;
	    }
	}

	gpcc_log_info("TAG CONNECTION %s", slot->cmdname);
	slot->u.ma.conn = conn;
	if (gpcc_net_tag(conn, slot->cmdname)) {
	    gpcc_log_error("TAG %s error", slot->cmdname);
	    return 0;
	}
	evt = gpcc_event_alloc(NET_OPENED_EVENT, gpma.maset_cmdslot->cmdidx, strlen(slot->cmdname)+1);
	strcpy(evt->pdata, slot->cmdname);
	gpcc_evqueue_put(gpma.evq, evt);
    } else {
	if (strcmp(gpma.ma_unit, slot->cmdname) < 0) {
	    /* cut peer connection */
	    if (slot->u.ma.peer_conn) {
		if (slot->u.ma.peer_conn == slot->u.ma.conn) {
		    gpcc_net_tag(slot->u.ma.conn, 0);   /* remove the tag */
		    slot->u.ma.conn = slot->u.ma.self_conn;
		    gpcc_net_tag(slot->u.ma.conn, slot->cmdname);
		}
		gpcc_net_close(slot->u.ma.peer_conn, 1);
		gpcc_log_info("MA CLOSE SPARE CONNECTION %s", slot->cmdname);
	    } 
	} else {
	    /* use the peer connection */
	    if (slot->u.ma.peer_conn) {
		if (slot->u.ma.peer_conn != slot->u.ma.conn) {
		    gpcc_net_tag(slot->u.ma.conn, 0);
		    slot->u.ma.conn = slot->u.ma.peer_conn;
		    gpcc_net_tag(slot->u.ma.conn, slot->cmdname);
		}
	    }
	}

    }

    return 0;
}

apr_status_t a_ma_teardown(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    gpcc_event_t *evt;
    gpcc_net_conn_t conn;

    memcpy(&conn, ev->pdata, sizeof(conn));

    gpcc_log_info("MA TEARDOWN %s", slot->cmdname);

    if (slot->u.ma.self_conn == conn) 
	slot->u.ma.self_conn = 0;
    else if (slot->u.ma.peer_conn == conn)
	slot->u.ma.peer_conn = 0;

    if (slot->u.ma.conn) {
	if (slot->u.ma.conn != conn) {
	    gpcc_log_info("SPARE CONNECTION CLOSED");
	    return 0;
	}
    } else {
	slot->countdown = 3;
	return 0;
    }

    /*  NO USE TO RECONNECT TO OTHER MA IF DISCONNECTED AND LET DEAD MA CONNECT TO ME AGAIN?
    slot->countdown = 3;
    */
    slot->u.ma.conn = 0;

    evt = gpcc_event_alloc(NET_CLOSED_EVENT, gpma.maset_cmdslot->cmdidx, strlen(slot->cmdname)+1);
    strcpy(evt->pdata, slot->cmdname);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}


apr_status_t a_lma_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    slot->countdown = 2;
    return 0;
}

apr_status_t a_lma_maset_ready(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->u.lma.maset_ready = 1;

    return 0;
}

apr_status_t a_lma_leadless(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    if (gpma.ma_max == 1 /* || slot->u.lma.nopened == 1 */) {
	/* LEADME is me */
	evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    } else {
	int id = 0;
	if (sscanf(gpma.ma_unit, "ma.%d", &id) != 1) {
	    id = 0;
	}
	slot->countdown += 1 + id * 3 ;
	gpcc_log_info("SLEEP %d seconds", slot->countdown);
    }
    return 0;
}

/*
  got VOTEME -- reply yes or no
*/
apr_status_t a_lma_vote_requested(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    char msg[64];
    int e;

    ma = gpcc_json_object_get_string(obj, "ma", 0);
    gpcc_log_info("RECV VOTEME from %s", ma);
    strcpy(slot->u.lma.voted.ma, ma);
    
    json_object_put(obj);

    sprintf(msg, "{ \"ma\" : \"%s\"}", gpma.ma_unit);
    
    if (0 != (e = gpcc_net_send_packetx(slot->u.lma.voted.ma, LMA_VOTE_YES_MSG, slot->cmdidx, pkt->mh.sndidx, msg, strlen(msg)+1))) {
	gpcc_warning(FLINE, "send vote packet failed");
	return 0;
    }

    gpcc_log_info("SEND VOTE YES to %s", slot->u.lma.voted.ma);

    slot->countdown = 0;

    return 0;
}

/* 
   fire ELECTSELF_EVENT if electing myself
*/
apr_status_t a_lma_ponder_elect(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    
    if (! slot->u.lma.maset_ready) {
	slot->countdown = 2;
	return 0;
    }

    evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

/*
 *  send voteme 
 */ 
apr_status_t a_lma_vote_me(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int i, e;
    int nvote = 0;
    char msg[64];
    gpcc_event_t *evt;

    if (gpma.ma_max == 1 /* || slot->u.lma.nopened == 1 */) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    sprintf(msg, "{ \"ma\" : \"%s\"}", gpma.ma_unit);

    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, 
						LMA_VOTE_ME_MSG, 
						slot->cmdidx, 
						0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send VOTE ME packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
		continue;
	    }
	    gpcc_log_info("SEND VOTEME to %s", gpma.ma_cmdtab[i]->cmdname);
	    nvote++;
	}
    }

    if (nvote > 0) {
	slot->u.lma.voteme.nvote = nvote;
	slot->u.lma.voteme.nreplied = 0;
	slot->u.lma.voteme.nyes = 0;
	slot->u.lma.voteme.nno = 0;
    } else {
	/* all MAs are dead */
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

/*
  send LMA_VOTENO_EVENT to ma
*/
apr_status_t a_lma_reject(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    char msg[64];
    int e;

    ma = gpcc_json_object_get_string(obj, "ma", 0);
    gpcc_log_info("RECV VOTE ME from %s", ma);

    sprintf(msg, "{ \"ma\" : \"%s\"}", gpma.ma_unit);
    
    if (0 != (e = gpcc_net_send_packetx(ma, LMA_VOTE_NO_MSG, slot->cmdidx, pkt->mh.sndidx, msg, strlen(msg)+1))) {
	gpcc_warning(FLINE, "send VOTENO packet to %s failed", ma);
	json_object_put(obj);
	return 0;
    }

    gpcc_log_info("SEND VOTE_REJECT to %s", ma);
    json_object_put(obj);
    return 0;
}

/**
 *  check WINVOTE ma is equal to slot->u.lma.ma_voted
 */
apr_status_t a_lma_check_win(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    gpcc_event_t *evt;

    ma = gpcc_json_object_get_string(obj, "ma", 0);
    gpcc_log_info("RECV VOTE WIN from %s", ma);
    if (strcmp(slot->u.lma.voted.ma, ma) == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    } 

    json_object_put(obj);
    return 0;
}

/**
 *   check LOSTVOTE ma is equal to slot->u.lma.ma_voted
 */
apr_status_t a_lma_check_lost(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    gpcc_event_t *evt;

    ma = gpcc_json_object_get_string(obj, "ma", 0);
    gpcc_log_info("RECV VOTE LOST from %s", ma);
    if (strcmp(slot->u.lma.voted.ma, ma) == 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	slot->countdown = 5;
    }

    json_object_put(obj);
    return 0;
}


apr_status_t a_lma_ponder_vote(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    ma = gpcc_json_object_get_string(obj, "ma", 0);

    slot->u.lma.voteme.nreplied++;
    switch (pkt->mh.type) {
    case LMA_VOTE_YES_MSG:
	gpcc_log_info("RECV VOTE YES from %s", ma);
	slot->u.lma.voteme.nyes++;
	break;
    case LMA_VOTE_NO_MSG:
	gpcc_log_info("RECV VOTE REJECT from %s", ma);
	slot->u.lma.voteme.nno++;
	break;
    default:
	break;
    }

    json_object_put(obj);

    if (slot->u.lma.voteme.nvote == slot->u.lma.voteme.nreplied) {
	if (slot->u.lma.voteme.nvote == slot->u.lma.voteme.nyes) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	    slot->countdown = 3;
	}
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

apr_status_t a_lma_lead(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char msg[64];
    int i, e;
    gpcc_event_t *evt;

    sprintf(msg, "{\"ma\" : \"%s\"}", gpma.ma_unit);
    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, LMA_VOTE_WIN_MSG, slot->cmdidx, 0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send win vote packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
	    }
	    gpcc_log_info("SEND WIN VOTE to %s", gpma.ma_cmdtab[i]->cmdname);
	}
    }

    
    strcpy(slot->u.lma.leadma, gpma.ma_unit);
    
    /* kick off the nodes */
    for (i = 0 ; i < gpma.node_max ; i++) {
	evt = gpcc_event_alloc(INIT_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}


apr_status_t a_lma_lead_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char msg[64];
    int i, e;

    sprintf(msg, "{\"ma\" : \"%s\"}", gpma.ma_unit);
    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, LMA_VOTE_LOST_MSG, slot->cmdidx, 0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send lost vote packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
	    }
	    gpcc_log_info("SEND LOST VOTE to %s", gpma.ma_cmdtab[i]->cmdname);
	}
    }

    return 0;
}

apr_status_t a_lma_standby(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    strcpy(slot->u.lma.leadma, slot->u.lma.voted.ma);
    return 0;
}
