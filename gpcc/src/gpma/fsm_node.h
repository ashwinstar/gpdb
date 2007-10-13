
apr_status_t a_node_ssh(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    char cmd[256];
    int e;
    const char *user = config_getx(gpma.cfg, CONFIG_USER_KEY, getenv("USER"));
    gpcc_event_t *nev;

    ++slot->u.node.hostidx;
    if (slot->u.node.hostidx >= slot->u.node.hosttab_max)
	slot->u.node.hostidx = 0;


    sprintf(cmd, "export GPHOME=%s && $GPHOME/bin/gpha /tmp", config_get(gpma.cfg, "gphome"));
    if (0 != (e = gpcc_proc_ssh(&slot->u.node.proc, 
				user, 
				slot->u.node.hosttab[slot->u.node.hostidx], 
				22, 
				cmd, 
				0, 
				slot->pool))) {
	gpcc_warning(FLINE, "start gpha via ssh failed");
	nev = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, nev);
	return 0;
    }

    slot->countdown = 2;
    return 0;
}

apr_status_t a_node_check_ssh(gpcc_event_t* ev, void* arg)
{
    /* disregard exit status for now */
    cmdslot_t* slot = arg;
    gpcc_event_t* nev = 0;
    int e, ret;
    apr_exit_why_e exitwhy;

    e = apr_proc_wait(&slot->u.node.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    nev = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, nev);
	    return 0;
	}
	nev = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, nev);
    } else {
	slot->countdown = 2;
    }
    
    return 0;
}

apr_status_t a_node_connect(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    char *argv[1];
    int argc;
    int hc;
    char *hname[1];
    char *hvalue[1];
    gpcc_event_t *nev;
    int e;
    char addr[128];

    argc = 1;
    argv[0] = GPCC_NET_PACKET_PROTO;
    hc = 1;
    hname[0] = "X-GPCC-ROLE";
    hvalue[0] = "GPMA";

    sprintf(addr, "%s:%d", slot->u.node.hosttab[slot->u.node.hostidx], DEFAULT_HA_PORT);
    printf("NODE CONNECT\n");
    /* use gpcc_net to connect to gpha here */
    if (0 != (e = gpcc_net_open(&slot->u.node.conn, 
				addr,
				argc, argv,
				hc, hname, hvalue, slot->cmdidx))) {
	nev = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, nev);
	return 0;
    }

    slot->countdown = 2;
    return 0;
}

apr_status_t a_node_check_conn(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    gpcc_event_t* nev = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, nev);
    printf("NODE CHECK CONN\n");
    return 0;
}

apr_status_t a_node_teardown(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;

    printf("NODE TEARDOWN\n");
    slot->countdown = 2;
    
    return 0;
}

apr_status_t a_node_ping(gpcc_event_t* ev, void* arg)
{
    printf("NODE PING\n");
    return 0;
}

