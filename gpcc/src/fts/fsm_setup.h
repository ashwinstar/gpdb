#define HA_DEFAULT_PORT  9876


static struct json_object *config_to_json_object() 
{
    struct json_object *obj;
    char key[128];
    int i;
    const char *value;
    int intvalue;

    obj = json_object_new_object();

    strcpy(key, "gpdb.name");
    value = config_getx(gpfts.config, key, "mygpdb");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.user");
    value = config_getx(gpfts.config, key, getenv("USER"));
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.password");
    value = config_getx(gpfts.config, key, "mpparray");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.max_connect");
    intvalue = config_get_or_set_int(gpfts.config, key, 25);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.encoding");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.locale");
    value = config_getx(gpfts.config, key, "UNICODE");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.shared_buffers");
    value = config_getx(gpfts.config, key, "128MB");
    json_object_object_add(obj, key, json_object_new_string((char *)value));    

    strcpy(key, "gpdb.checkpoint_segments");
    intvalue = config_get_or_set_int(gpfts.config, key, 31);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.master_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QD_PORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    strcpy(key, "gpdb.seg.mirror_base_port");
    intvalue = config_get_or_set_int(gpfts.config, key, DEFAULT_MIRROR_QE_BASEPORT);
    json_object_object_add(obj, key, json_object_new_int(intvalue));

    
    for (i = 0 ;  ; i++) {
	sprintf(key, "node.%d", i);
	value = config_get(gpfts.config, key);
	if (!value) 
	    break;

	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;
	json_object_object_add(obj, key, json_object_new_string((char *)value));    
    }



    return obj;
}



static inline void setup_set_segment(cmdslot_t *cmdslot, int idx, char *name, int dbid, int content, int port, int node, char *dir, apr_pool_t *pool)
{

    cmdslot->u.sys.seginfo[idx].name = apr_pstrdup(pool, name);
    cmdslot->u.sys.seginfo[idx].dbid = dbid;
    cmdslot->u.sys.seginfo[idx].content = content;
    cmdslot->u.sys.seginfo[idx].port = port;
    cmdslot->u.sys.seginfo[idx].haidx = node;
    cmdslot->u.sys.seginfo[idx].dir = apr_pstrdup(pool, dir);
}

static apr_status_t setup_init_seginfo(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = (cmdslot_t *)_cmdslot;
    int i, n, dbid, content, node;
    char key[128], dir[1024];
    const char *value;
    int master_port, base_port, mirror_base_port;

    cmdslot->u.sys.config_json = config_to_json_object();

    master_port = config_get_or_set_int(gpfts.config, key, DEFAULT_QD_PORT);
    base_port = config_get_or_set_int(gpfts.config, key, DEFAULT_QE_BASEPORT);
    mirror_base_port = config_get_or_set_int(gpfts.config, key, DEFAULT_MIRROR_QE_BASEPORT);

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
    }

    n = i;

    for (i = 0 ; ; i++) {
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	
	n++;

	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;

	n++;
    }	

    cmdslot->u.sys.nseginfo = n;
    cmdslot->u.sys.seginfo = apr_palloc(cmdslot->pool, sizeof(*cmdslot->u.sys.seginfo) * n);

    /* master QD */
    strcpy(key, "gpdb.seg.m.0");
    dbid = 1;
    content = -1;
    if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	return APR_EINVAL;
    }	
    
    setup_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, cmdslot->pool);
    
    for (i = 0 ;  ; i++) {
	/* master QE */
	sprintf(key, "gpdb.seg.%d.0", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}	
	dbid++;
	content++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, base_port++, node, dir, cmdslot->pool);

	/* mirror QE */
	sprintf(key, "gpdb.seg.%d.1", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    continue;

	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}
	dbid++;	
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, mirror_base_port++, node, dir, cmdslot->pool);
    }

    /* standby QD */
    content = -1;
    for (i = 1 ; ; i++) {
	sprintf(key, "gpdb.seg.m.%d", i);
	value = config_get(gpfts.config, key);
	if (!value)
	    break;
	if (sscanf(value, "node.%d:%s", &node, dir) != 2) {
	    gpcc_warning(FLINE, "value format error. %s = %s", key, value);
	    return APR_EINVAL;
	}
	dbid++;
	setup_set_segment(cmdslot, dbid-1, key, dbid, content, master_port, node, dir, cmdslot->pool);
    }	

    return 0;
}


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
	sprintf(key, "node.%d", i);
	value = config_get(gpfts.config, key);
	if (! value) {
	    break;
	}
    }
    cmdslot->u.sys.request.u.setup.nhost = i;    

    cmdslot->u.sys.request.u.setup.hosts = apr_pcalloc(cmdslot->u.sys.request.pool, sizeof(char *) * (i+1));

    for (i = 0 ; ; i++) {
	sprintf(key, "node.%d", i);
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

    cmdslot->u.sys.config_json = config_to_json_object();
    setup_init_seginfo(ev, _cmdslot);

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




