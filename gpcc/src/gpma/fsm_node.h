apr_status_t a_node_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    slot->countdown = 1;
    return 0;
}

#define LEOPARD

apr_status_t a_node_ssh(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    char cmd[256];
    int e;
    const char *user = config_getx(gpma.cfg, CONFIG_USER_KEY, getenv("USER"));
    gpcc_event_t *nev;

    gpcc_log_info("a_node_ssh......");

    ++slot->u.node.hostidx;
    if (slot->u.node.hostidx >= slot->u.node.hosttab_max)
	slot->u.node.hostidx = 0;

    gpcc_log_info("a_node_ssh");
#ifdef LEOPARD
    sprintf(cmd, "export GPHOME=%s && $GPHOME/bin/gpha -D %s %s", 
	    getenv("GPHOME"), 
	    config_get(gpma.cfg, "sys.guid"),
	    config_getx(gpma.cfg, "ha.datadir", "/tmp"));
#else
    sprintf(cmd, "export GPHOME=%s && $GPHOME/bin/gpha %s %s", 
	    getenv("GPHOME"), 
	    config_get(gpma.cfg, "sys.guid"),
	    config_getx(gpma.cfg, "ha.datadir", "/tmp"));
#endif
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

    cmdslot_t* slot = arg;
    gpcc_event_t* nev = 0;

#ifdef LEOPARD
    nev = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, nev);

#else
    int e, ret;
    apr_exit_why_e exitwhy;

    gpcc_log_info("check ssh\n");
    e = apr_proc_wait(&slot->u.node.proc, &ret, &exitwhy, APR_NOWAIT);
    if (e == APR_CHILD_DONE) {
	if (ret == 0) {
	    gpcc_log_info("done....success.");
	    nev = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, nev);
	    return 0;
	}
	nev = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, nev);
    } else {
	slot->countdown = 2;
    }
#endif
    
    return 0;
}

apr_status_t a_node_connect(gpcc_event_t* ev, void* arg)
{
    cmdslot_t* slot = arg;
    const char* argv[1];
    int argc;
    int hc;
    const char* hname[2];
    const char* hvalue[2];
    gpcc_event_t* nev;
    int e;
    char addr[128];

    argc = 1;
    argv[0] = GPCC_NET_PACKET_PROTO;
    hc = 2;
    hname[0] = "X-GPCC-ROLE";
    hvalue[0] = "GPMA";
    hname[1] = "X-GPCC-GUID";
    hvalue[1] = gpma.guid;

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

    //    slot->countdown = 2;
    return 0;
}

apr_status_t a_node_ready(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    slot->u.node.opened = 1;
    gpcc_net_tag(slot->u.node.conn, slot->cmdname);
    evt = gpcc_event_alloc(NET_OPENED_EVENT, gpma.haset_cmdslot->cmdidx, strlen(slot->cmdname)+1);
    memcpy(evt->pdata, slot->cmdname, strlen(slot->cmdname)+1);
    gpcc_evqueue_put(gpma.evq, evt);
    gpcc_log_info("------------------------------------------- NODE %s READY", slot->cmdname);
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
    gpcc_event_t *evt;

    printf("NODE TEARDOWN\n");
    slot->countdown = 3;
    
    if (! slot->u.node.opened) 
	return 0;
    slot->u.node.opened = 0;

    evt = gpcc_event_alloc(NET_CLOSED_EVENT, gpma.haset_cmdslot->cmdidx, strlen(slot->cmdname)+1);
    memcpy(evt->pdata, slot->cmdname, strlen(slot->cmdname)+1);
    gpcc_evqueue_put(gpma.evq, evt);
    return 0;
}

apr_status_t a_node_ping(gpcc_event_t* ev, void* arg)
{
    printf("NODE PING\n");
    return 0;
}

