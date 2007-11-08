

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

    
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
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
  send LMA_VOTENO_EVENT to ma
*/
apr_status_t a_lma_reject(gpcc_event_t *ev, void *arg)
{


    return 0;
}

/**
 *  check WINVOTE ma is equal to slot->u.lma.ma_voted
 */
apr_status_t a_lma_check_win(gpcc_event_t *ev, void *arg)
{

    return 0;
}

/**
 *   check LOSTVOTE ma is equal to slot->u.lma.ma_voted
 */
apr_status_t a_lma_check_lost(gpcc_event_t *ev, void *arg)
{

    return 0;
}


apr_status_t a_lma_ponder_vote(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.lma.nreplied++;
    switch (ev->etype) {
    case LMA_VOTEYES_EVENT:
	slot->u.lma.nyes++;
	break;
    case LMA_VOTENO_EVENT:
	slot->u.lma.nno++;
	break;
    default:
	break;
    }

    if (slot->u.lma.nvote == slot->u.lma.nreplied) {
	if (slot->u.lma.nvote == slot->u.lma.nyes) {
	    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	} else {
	    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	    slot->countdown = 3;
	}
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

