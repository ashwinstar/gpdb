#define HA_DEFAULT_PORT  9876

static void u_queue_timeup_event(cmdslot_t* cmdslot)
{
    gpcc_event_t* ev = gpcc_event_alloc(TIMEUP_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpfts.evq, ev);
}

static apr_status_t timer_init(gpcc_event_t *ev, void *_cmdslot)
{


    return 0;
}

static apr_status_t timer_tick(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t* p = &gpfts.cmdslot[0];
    cmdslot_t* q = &gpfts.cmdslot[CMDSLOT_MAX];

    for ( ; p < q; p++) {
	if (p->cmdidx && p->countdown > 0)
	    if (--p->countdown == 0) 
		u_queue_timeup_event(p);
    }
    return 0;
}

static apr_status_t system_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    
    memset(&cmdslot->u.sys, 0, sizeof(cmdslot->u.sys));
    return 0;
}

static void *ha_net_opened(gpcc_ha_t *ha, void *arg)
{
    hainfo_t *hainfo = (hainfo_t *) arg;
    gpcc_event_t *ev = gpcc_event_alloc(NET_OPENED_EVENT, gpfts.system_cmdidx, sizeof(int));
    memcpy(ev->edata, &hainfo->index, sizeof(int));
    gpcc_evqueue_put(gpfts.evq, ev);
    return arg;

}

static void ha_net_closed(gpcc_ha_t *ha, void *arg)
{
    hainfo_t *hainfo = (hainfo_t *) arg;
    gpcc_event_t *ev;
    
    if (hainfo->index < 0)
	return;

    ev = gpcc_event_alloc(NET_CLOSED_EVENT, gpfts.system_cmdidx, sizeof(int));
    memcpy(ev->edata, &hainfo->index, sizeof(int));
    gpcc_evqueue_put(gpfts.evq, ev);

}


static void ha_net_recv(gpcc_ha_t *ha, gpcc_net_packet_t *packet, void *arg)
{
    hainfo_t *hainfo = (hainfo_t *) arg;
    gpcc_net_packet_t *pkt;
    gpcc_event_t *ev = gpcc_event_alloc(NET_RECEIVED_EVENT, packet->mh.rcvidx, sizeof(gpcc_net_packet_t));
    memcpy(ev->edata, packet, sizeof(*packet));
    pkt = (gpcc_net_packet_t *) ev->edata;
    pkt->mh.rcvidx = hainfo->index;
    gpcc_evqueue_put(gpfts.evq, ev);    

}


static apr_status_t ha_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int index;
    
    memcpy(&index, ev->edata, sizeof(int));
    
    if (!cmdslot->u.sys.hainfo || !cmdslot->u.sys.hainfo[index])
	return 0;

    cmdslot->u.sys.hainfo[index]->ha = 0;
    free(cmdslot->u.sys.hainfo[index]);
    cmdslot->u.sys.hainfo[index] = 0;

    return 0;
}

static apr_status_t setup_array_connect(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *p = (cmdslot_t *) _cmdslot;
    char addr[128];
    hainfo_t *hainfo;
    int i;

    if (p->u.sys.hainfo) {
	free(p->u.sys.hainfo);
	p->u.sys.hainfo = 0;
    }

    p->u.sys.nhainfo = p->u.sys.request.u.setup.nhost;
    p->u.sys.hainfo = malloc(sizeof(hainfo_t *) * p->u.sys.nhainfo);
    memset(p->u.sys.hainfo, 0, sizeof(hainfo_t *) * p->u.sys.nhainfo);
    p->u.sys.request.u.setup.ndisconnected = 0;
    p->u.sys.request.u.setup.nconnected = 0;
    p->u.sys.request.u.setup.nreply = 0;

    for (i = 0 ; i < p->u.sys.nhainfo ; i++) {
					
	sprintf(addr, "%s:%d", p->u.sys.request.u.setup.hosts[i], HA_DEFAULT_PORT);
	
	hainfo = malloc(sizeof(*hainfo));
	hainfo->index = i;
	hainfo->ha = gpcc_ha_open(p->cmdidx, addr, ha_net_opened, ha_net_closed, ha_net_recv, hainfo);
	if (!hainfo->ha) {
	    free(hainfo);
	    hainfo = 0;
	    p->u.sys.request.u.setup.ndisconnected++;
	    p->u.sys.request.u.setup.nreply++;
	}
	
	p->u.sys.hainfo[i] = hainfo;
    }

    if (p->u.sys.request.u.setup.ndisconnected == p->u.sys.nhainfo) {
	gpcc_event_t *ev = gpcc_event_alloc(SETUP_SSH_EVENT, p->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, ev);	
    } 
	
	
    return 0;
}


static apr_status_t setup_array_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int i;

    if (cmdslot->u.sys.hainfo) {
	for (i = 0 ; i < cmdslot->u.sys.nhainfo ; i++) {
	    if (cmdslot->u.sys.hainfo[i]) {
		cmdslot->u.sys.hainfo[i]->index = -1;
		if (cmdslot->u.sys.hainfo[i]->ha) {
		    gpcc_ha_close(cmdslot->u.sys.hainfo[i]->ha);
		}
		free(cmdslot->u.sys.hainfo[i]);
		cmdslot->u.sys.hainfo[i] = 0;
	    }
	}
	free(cmdslot->u.sys.hainfo);
	cmdslot->u.sys.hainfo = 0;
    }

    return 0;
}

static apr_status_t setup_init(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int e;
    int i;
    const char *value;
    char key[128], host[128], *s;

    memset(&cmdslot->u.sys.request.u, 0, sizeof(cmdslot->u.sys.request.u));
    
    e = apr_pool_create(&cmdslot->u.sys.request.pool, 0);
    if (e) {
	gpcc_warning(FLINE, "apr_pool_create failed");
	return e;
    }
    
    for (i = 0 ;  ; i++) {
	sprintf(key, "node%d", i);
	value = config_get(gpfts.config, key);
	if (! value) {
	    break;
	}
    }
    cmdslot->u.sys.request.u.setup.nhost = i;    

    cmdslot->u.sys.request.u.setup.hosts = apr_pcalloc(cmdslot->u.sys.request.pool, sizeof(char *) * (i+1));

    for (i = 0 ; ; i++) {
	sprintf(key, "node%d", i);
	value = config_get(gpfts.config, key);	
    
	if ((s = strchr(value, ',')) == 0) {
	    gpcc_warning(FLINE, "config error: %s: %s", key, value);
	    return APR_EINVAL;
	}
	strncpy(host, value+1, s - value - 1);
	host[s - value - 1] = 0;
	
	cmdslot->u.sys.request.u.setup.hosts[i] = apr_pstrdup(cmdslot->u.sys.request.pool, host);
    }

    return 0;
}

static apr_status_t setup_cleanup(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    apr_pool_destroy(cmdslot->u.sys.request.pool);

    return 0;
}

static apr_status_t setup_ha_opened(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    int index;
    
    memcpy(&index, ev->edata, sizeof(int));
    
    cmdslot->u.sys.request.u.setup.nreply++;
    cmdslot->u.sys.request.u.setup.nconnected++;

    if (cmdslot->u.sys.request.u.setup.nreply == cmdslot->u.sys.nhainfo) {
	if (cmdslot->u.sys.request.u.setup.nconnected == cmdslot->u.sys.nhainfo) {
	    gpcc_event_t *ev = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, ev);
	} else {
	    gpcc_event_t *ev = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, ev);	    
	}
    }

    return 0;
}


static apr_status_t setup_ha_closed(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;

    cmdslot->u.sys.request.u.setup.nreply++;
    cmdslot->u.sys.request.u.setup.ndisconnected++;

    if (cmdslot->u.sys.request.u.setup.nreply == cmdslot->u.sys.nhainfo) {
	if (cmdslot->u.sys.request.u.setup.ndisconnected == cmdslot->u.sys.nhainfo) {
	    gpcc_event_t *ev = gpcc_event_alloc(SETUP_SSH_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, ev);
	} else {
	    gpcc_event_t *ev = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	    gpcc_evqueue_put(gpfts.evq, ev);	    
	}
    }

    return 0;
}

/* return success reply to outq */
static apr_status_t setup_ready(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("setup", "ok", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);
    return 0;
}

/* return failure reply to outq */
static apr_status_t setup_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;    
    struct json_object *obj;
    char *objstr;
    gpcc_event_t *evt;

    obj = gpcc_json_array_reply("setup", "failed", 0);
    objstr = json_object_to_json_string(obj);

    evt = gpcc_event_alloc(ARRAY_REPLY_MSG_EVENT, 0, strlen(objstr) + 1);
    memcpy(evt->edata, objstr, strlen(objstr) + 1);

    gpcc_evqueue_put(cmdslot->u.sys.request.outq, evt);

    json_object_put(obj);    
    return 0;
}

/* start HA daemon via ssh */
static apr_status_t setup_ssh_start(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *) _cmdslot;
    char *user = getenv("USER");
    char cmd[1024];
    int e;
    gpcc_event_t *evt;

    sprintf(cmd, "%s/bin/gpha", getenv("GPHOME"));
    if (0 != (e = gpcc_proc_gpssh(&cmdslot->u.sys.request.u.setup.proc, 
				  user, 
				  (const char *const *)cmdslot->u.sys.request.u.setup.hosts, 
				  22, 
				  cmd, 
				  1, 
				  cmdslot->u.sys.request.pool))) {
	gpcc_warning(FLINE, "start gpha via ssh failed");

#ifdef SUNOS
	strcpy(cmd, "pkill gpha");
#else
	strcpy(cmd, "killall -9 gpha");
#endif
	if (0 != (e = gpcc_proc_gpssh(&cmdslot->u.sys.request.u.setup.proc,
				      user,
				      (const char *const *) cmdslot->u.sys.request.u.setup.hosts, 
				      22, 
				      cmd,
				      1,
				      cmdslot->u.sys.request.pool))) {
	    gpcc_warning(FLINE, "kill gpha failed");
	}

	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);
	gpcc_evqueue_put(gpfts.evq, ev);
	return e;
    }
    
    evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpfts.evq, ev);

    return 0;
}




