

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

    argc = 1;
    argv[0] = GPCC_NET_PACKET_PROTO;
    hc = 2;
    hname[0] = "X-GPCC-ROLE";
    hvalue[0] = "GPMA";
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
    /* use gpcc_net to connect to gpma here */
    if (0 != (e = gpcc_net_open(&slot->u.ma.conn, 
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

    gpcc_log_info("------------------------------------------- MA %s READY", slot->cmdname);

    evt = gpcc_event_alloc(NET_OPENED_EVENT, gpma.lma_cmdslot->cmdidx, strlen(slot->cmdname)+1);
    strcpy(evt->pdata, slot->cmdname);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

apr_status_t a_ma_teardown(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    gpcc_event_t *evt;

    printf("MA TEARDOWN\n");
    slot->countdown = 3;
    
    evt = gpcc_event_alloc(NET_CLOSED_EVENT, gpma.lma_cmdslot->cmdidx, strlen(slot->cmdname)+1);
    strcpy(evt->pdata, slot->cmdname);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}


apr_status_t a_lma_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.lma.nopened = 1;
    if (gpma.ma_max == slot->u.lma.nopened) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

apr_status_t a_lma_ma_opened(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.lma.nopened++;
    if (slot->u.lma.nopened == gpma.ma_max) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

apr_status_t a_lma_ma_closed(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    
    slot->u.lma.nopened--;
    if (strcmp(ev->pdata, gpma.ma_unit) == 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    return 0;
}

apr_status_t a_lma_leadless(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    if (gpma.ma_max == 1) {
	/* LEADME is me */
	evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    } else {
	int id = 0;
	if (sscanf(gpma.ma_unit, "ma.%d", &id) != 1) {
	    id = 0;
	}
	slot->countdown += 1 + id;
    }
    return 0;
}

/*
  got VOTEME -- reply yes or no
  fire SUCCESS_EVENT if answer is yes
*/
apr_status_t a_lma_vote_requested(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj = json_tokener_parse(pkt->payload);
    char *ma;
    char msg[64];
    int e;

    ma = gpcc_json_object_get_string(obj, "ma", 0);
    if (!ma) {
	return 0;
    }

    strncpy(slot->u.lma.voted.ma, ma, sizeof(slot->u.lma.voted.ma));
    json_object_put(obj);

    sprintf(msg, "{ \"ma\" : \"%s\"}", slot->cmdname);
    
    if (0 != (e = gpcc_net_send_packetx(ma, LMA_VOTE_YES_MSG, slot->cmdidx, pkt->mh.sndidx, msg, strlen(msg)+1))) {
	gpcc_warning(FLINE, "send vote packet failed");
	return 0;
    }

    slot->countdown = 0;
    evt = gpcc_event_alloc(LMA_VOTED_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

/* 
   fire ELECTSELF_EVENT if electing myself
*/
apr_status_t a_lma_ponder_elect(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

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

    if (gpma.ma_max == 1) {
	gpcc_event_t *evt;

	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    sprintf(msg, "{ \"ma\" : \"%s\"}", slot->cmdname);

    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, 
						LMA_VOTE_ME_MSG, 
						slot->cmdidx, 
						0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send vote packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
		continue;
	    }
	    nvote++;
	}
    }

    slot->u.lma.voteme.nvote = nvote;
    slot->u.lma.voteme.nreplied = 0;
    slot->u.lma.voteme.nyes = 0;
    slot->u.lma.voteme.nno = 0;
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
    if (!ma) {
	return 0;
    }

    sprintf(msg, "{ \"ma\" : \"%s\"}", slot->cmdname);
    
    if (0 != (e = gpcc_net_send_packetx(ma, LMA_VOTE_NO_MSG, slot->cmdidx, pkt->mh.sndidx, msg, strlen(msg)+1))) {
	gpcc_warning(FLINE, "send vote packet to %s failed", ma);
	return 0;
    }

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
    if (strcmp(slot->u.lma.voted.ma, ma) == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    } 

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
    if (strcmp(slot->u.lma.voted.ma, ma) == 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	slot->countdown = 5;
    }

    return 0;
}


apr_status_t a_lma_ponder_vote(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;

    slot->u.lma.voteme.nreplied++;
    switch (pkt->mh.type) {
    case LMA_VOTE_YES_MSG:
	slot->u.lma.voteme.nyes++;
	break;
    case LMA_VOTE_NO_MSG:
	slot->u.lma.voteme.nno++;
	break;
    default:
	break;
    }

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

    sprintf(msg, "{\"ma\" : \"%s\"}", slot->cmdname);
    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, LMA_VOTE_WIN_MSG, slot->cmdidx, 0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send win vote packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
	    }
	}
    }
    return 0;
}


apr_status_t a_lma_lead_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char msg[64];
    int i, e;

    sprintf(msg, "{\"ma\" : \"%s\"}", slot->cmdname);
    for (i = 0 ; i < gpma.ma_max ; i++) {
	if (gpma.ma_cmdtab[i]) {
	    if (0 != (e = gpcc_net_send_packetx(gpma.ma_cmdtab[i]->cmdname, LMA_VOTE_LOST_MSG, slot->cmdidx, 0, msg, strlen(msg)+1))) {
		gpcc_warning(FLINE, "send lost vote packet to %s failed", gpma.ma_cmdtab[i]->cmdname);
	    }
	}
    }
    return 0;

}
