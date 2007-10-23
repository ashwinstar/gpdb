

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

    gpcc_log_info("------------------------------------------- MA %s READY", slot->cmdname);
    return 0;
}

apr_status_t a_ma_teardown(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;

    printf("MA TEARDOWN\n");
    slot->countdown = 3;
    
    return 0;
}
