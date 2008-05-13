#define LMA_EXPIRE_PERIOD 30 /* 30 seconds */
#define LMA_RENEW_PERIOD  25 /* 25 seconds */
#define LMA_RETRY_PERIOD  10 /* 10 seconds */
#define IS_LEADMA (gpma.lma_cmdslot->u.lma.leadma_lease != NULL)
#define IS_NO_LEADMA (gpma.lma_cmdslot->u.lma.leader == NULL)

static apr_status_t a_lma_leader_lease(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char path[1024];
    zhandle_t *zh;
    int e;

    sprintf(path, "/gp/%s/%s/gpma/lock", gpma.user, gpma.array_name);
    
    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    e = zookeeperExtra_lease(slot->u.lma.zhandle, &zh, path, 1, LMA_EXPIRE_PERIOD);
    switch (e) {
    case ZOK:
	slot->u.lma.leadma_lease = zh;
	return APR_SUCCESS;
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
    case ZMARSHALLINGERROR:
    default:
	return APR_EINVAL;
    }    
    return APR_EINVAL;

}

static apr_status_t a_lma_leader_lease_getExpireTime(gpcc_event_t *ev, void *arg, time_t *expireTime)
{
    cmdslot_t *slot = arg;
    char path[1024];
    int e;

    sprintf(path, "/gp/%s/%s/gpma/lock", gpma.user, gpma.array_name);
    
    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    if ((e = zookeeperExtra_lease_getExpireTime(slot->u.lma.zhandle, expireTime)) != 0)
	return APR_EINVAL;
    return APR_SUCCESS;
}

static apr_status_t a_lma_leader_lease_renew(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    if (!slot->u.lma.leadma_lease)
	return APR_EINVAL;

    if (is_unrecoverable(slot->u.lma.leadma_lease)) {
	gpcc_log_error("paxos: can't renew leader lease. session expired already.");
	zookeeper_close(slot->u.lma.leadma_lease);
	slot->u.lma.leadma_lease = 0;
	return APR_EINVAL;
    }

    e = zookeeperExtra_lease_renew(slot->u.lma.leadma_lease, LMA_EXPIRE_PERIOD);
    switch (e) {
    case ZOK:
	return APR_SUCCESS;
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
    case ZMARSHALLINGERROR:
    default:
	gpcc_log_error("paxos: can't renew leader lease. session expired already.");
	zookeeper_close(slot->u.lma.leadma_lease);
	slot->u.lma.leadma_lease = 0;	
	return APR_EINVAL;
    }    
    return APR_EINVAL;
    
}


static apr_status_t a_lma_leader_lease_giveup(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    if (!slot->u.lma.leadma_lease)
	return APR_SUCCESS;

    if (is_unrecoverable(slot->u.lma.leadma_lease)) {
	gpcc_log_info("paxos: leader lease gave up because of session expired already.");
	zookeeper_close(slot->u.lma.leadma_lease);
	slot->u.lma.leadma_lease = 0;
	return APR_SUCCESS;
    }

    e = zookeeperExtra_lease_giveUp(slot->u.lma.leadma_lease);
    switch (e) {
    case ZOK:
	slot->u.lma.leadma_lease = 0;
	return APR_SUCCESS;
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
    case ZMARSHALLINGERROR:
    default:
	gpcc_log_error("paxos: cannot give up the lease. close connection anyway.");
	zookeeper_close(slot->u.lma.leadma_lease);
	slot->u.lma.leadma_lease = 0;	
	return APR_EINVAL;
    }    
    return APR_EINVAL;
}

static int a_lma_check_same_host(const char *addr, int ignore_port, apr_pool_t *pool)
{
    char host[256], hostname[256];
    int port, len;
    struct in_addr **addr_list;
    struct hostent *he;
    int i;
    char *s;

    s = strchr(addr, ':');
    if (!s) {
	gpcc_log_error("invalid addr format %s", addr);
	return 0;
    }

    strncpy(host, addr, s - addr);
    host[s-addr] = 0;

    if (!ignore_port) {
	port = strtol(s+1, 0, 0);

	/* check port */
	if (port != gpma.ma_port)
	    return 0;
    }

    /* check hostname directly */
    if (strcmp(gpma.hostname, host) == 0)
	return 1;
    
    /* check IP addresses */
    he = gethostbyname(gpma.hostname);
    addr_list = (struct in_addr **) he->h_addr_list;
    for (i = 0 ; addr_list[i] ; i++) {
	if (strcmp(addr, inet_ntoa(*addr_list[i])) == 0)
	    return 1;
    }
    
    /* ssh remote host to get the hostname */
    len = sizeof(hostname);
    if (gpcc_pg_gethostname(host, hostname, &len, pool) != 0)  {
	gpcc_log_error("invalid remote host %s", host);
	return 0;
    }

    if (strcmp(hostname, gpma.hostname) == 0)
	return 1;
	
    return 0;
}

static apr_status_t a_lma_paxos_get(gpcc_event_t *ev, void *arg, char *path, char *buf, int *len, struct Stat *stat)
{
    cmdslot_t *slot = arg;
    int e;

    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    e = zoo_get(slot->u.lma.zhandle, path, 0, buf, len, stat);
    switch (e) {
    case ZOK:
	return APR_SUCCESS;
    case ZNONODE:
    case ZNOAUTH:
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
    case ZMARSHALLINGERROR:
    default:
	return APR_EINVAL;
    }    
    return APR_EINVAL;
}

static apr_status_t a_lma_paxos_exists(gpcc_event_t *ev, void *arg, const char *path, int *exists)
{
    cmdslot_t *slot = arg;
    int e;
    struct Stat stat;

    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    e = zoo_exists(slot->u.lma.zhandle, path, 0, &stat);
    switch (e) {
    case ZOK:
	*exists = 1;
	return 0;
    case ZNONODE:
	/* elect ourself to be a lead MA */
	*exists = 0;
	return 0;
    case ZNOAUTH:
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
	/* session expired or auth failed */
    case ZMARSHALLINGERROR:
	/* out of memory */
    default:
	gpcc_log_error("paxos error on checking file %s exist. ret = %d", path, e);
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
	return APR_EINVAL;
    }
	
    return 0;
}


apr_status_t a_lma_paxos_set(gpcc_event_t *ev, void *arg, char *path, char *content, int version)
{
    cmdslot_t *slot = arg;
    struct Stat stat;
    int exists = 0, close = 0;
    int e;

    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    e = zoo_exists(slot->u.lma.zhandle, path, 0, &stat);
    switch (e) {
    case ZOK:
	exists = 1;
	break;
    case ZNONODE:
	/* elect ourself to be a lead MA */
	exists = 0;
	break;
    case ZNOAUTH:
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:
	/* session expired or auth failed */
    case ZMARSHALLINGERROR:
	/* out of memory */
    default:
	gpcc_log_error("paxos error on checking file %s exist. ret = %d", path, e);
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
	return APR_EINVAL;
    } 

    if (!exists) {
	/* zoo_create */
	e = zoo_create(slot->u.lma.zhandle, path, content, strlen(content), 0, 0,
		       content, strlen(content));

	switch (e) {
	case ZOK:
	    /* become a lead MA */
	    gpcc_log_info("paxos: create node %s success.", path);
	    return 0;
	case ZNONODE:
	    /* create node failed */
	    gpcc_log_error("paxos: create node %s failed.", path);
	    break;
	case ZNODEEXISTS:
	    /* node already exists. someone created the file. */
	    gpcc_log_error("paxos: node %s already exists. someone created the file", path);
	    break;
	case ZNOAUTH:
	    /* client does not have permission */
	    gpcc_log_error("paxos: client does not have permission to create node %s", path);
	    break;
	case ZNOCHILDRENFOREPHEMERALS:
	    gpcc_log_error("paxos: no children for ephemreals. path=%s", path);
	    break;
	case ZBADARGUMENTS:
	    gpcc_log_error("paxos: create node %s error. bad arguments", path);
	    break;
	case ZINVALIDSTATE:
	    /* session expired or auth failed */
	case ZMARSHALLINGERROR:
	    /* out of memory */
	    gpcc_log_error("paxos: create node %s error. invalid state or marshalling error. ret=%d", path, e);
	    close = 1;
	    break;
	default:
	    gpcc_log_error("paxos: create node %s error. ret=%d", path, e);
	    break;
	}
    } else {
	/* zoo_set */
	e = zoo_set(slot->u.lma.zhandle, path, content, strlen(content), version);

	switch (e) {
	case ZOK:
	    /* zoo_set ok and become a lead MA */
	    gpcc_log_info("paxos: set node % success.", path);
	    return 0;
	case ZNONODE:
	    /* zoo_set, node not exist */
	    gpcc_log_error("paxos: set node %s error. node not exist", path);
	    break;
	case ZNOAUTH:
	    gpcc_log_error("paxos: client does not have permission to set node %s", path);
	    break;
	case ZBADARGUMENTS:
	    gpcc_log_error("paxos: set node %s error. bad arguments", path);
	    break;
	case ZBADVERSION:
	    /* bad version. someone updated the file before I did. */
	    gpcc_log_error("paxos: set node %s error. bad version.  some updated the file before I did.", path);
	    break;
	case ZINVALIDSTATE:
	    /* session expired or auth failed */
	case ZMARSHALLINGERROR:
	    /* out of memory */
	    gpcc_log_error("paxos: set node %s error. invalid state or marshalling error. ret=%d", path, e);
	    close = 1;
	    break;
	default:
	    gpcc_log_error("paxos. set node %s error. ret = %d", path, e);
	    break;
	}
	    
    }

    if (close) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }
    
    return APR_EINVAL;
}

static apr_status_t a_lma_check_qd_is_localhost(gpcc_event_t *ev, void *arg, char *path, int *is_localhost)
{
    cmdslot_t *slot = arg;
    char buf[1024];
    int len;
    struct Stat stat;

    len = sizeof(buf);

    if (a_lma_paxos_get(ev, arg, path, buf, &len, &stat) != 0) {
	return APR_EINVAL;
    }

    *is_localhost = a_lma_check_same_host(buf, 1, slot->pool);
    return APR_SUCCESS;
}

/* sync the gp_configuration and gpcc config */
static apr_status_t a_lma_sync_config(gpcc_event_t *ev, void *arg)
{
    int i, e = 0, stop = 0, len, exists;
    apr_pool_t *p = 0;
    apr_proc_t proc;
    char key[128], host[256], dir[1028], line[1024];
    int master_port;
    char *last, *s;
    char path[1024];
    struct Stat stat;

    /* TODO: check paxos master file to look for master host and port */
    sprintf(path, "/gp/%s/%s/gpma/qd", gpma.user, gpma.array_name);

    exists = 0;
    if ((e = a_lma_paxos_exists(ev, arg, path, &exists)) != 0)
	return APR_EINVAL;

    if (!exists) 
	return 0;
	
    len = sizeof(line);
    if ((e = a_lma_paxos_get(ev, arg, path, line, &len, &stat)) != 0) {
	return APR_EINVAL;
    }

    s = strchr(line, ':');
    strncpy(host, line, s - line);
    host[s-line] = 0;
    master_port = strtol(s+1, 0, 0);

#if 0
    /* check master data directory exists */
    if (!gpcc_file_directory_exists(gpma.master_datadir)) {
	/* no array found */
	return 0;
    }

    /* master QD */
    if ((e = gpcc_pg_get_master_port(gpma.master_datadir, &master_port, gpma.pool)) != 0) {
	gpcc_log_info("master data directory does not exist, no array found");
	return 0;
    }
#endif

    if (apr_pool_create(&p, 0))
	gpcc_fatal(FLINE, "apr_pool_create: not enough memory\n");
	
    /* check if QD is running */
    /* get gp_configuration and reset config */
    if ((e = gpcc_proc_psql(&proc, host, master_port, "template1",
			    "select * from gp_configuration order by content",
			    1, 1, p)) != 0) {

	/* start master QD in admin mode */
	if ((e = gpcc_proc_pg_ctl_start_master(&proc, host, master_port, dir, 1, 1, 1, p)) != 0) {
	    gpcc_log_error("start master QD failure");
	    goto bail;
	}
	
	if ((e = gpcc_proc_psql(&proc, host, master_port, "template1",
				"select * from gp_configuration order by content",
				1, 1, p)) != 0) {
	    gpcc_log_error("psql -- select statement failure");
	    goto bail;
	}
	
	stop = 1;
    }

    /* read gp_configuration */
    for (i = 0; apr_file_gets(line, sizeof(line), proc.out) == 0 ; i++) {
	char *content, *definedprimary, *dbid, *isprimary, *valid, *hostname, *port, *datadir;
	char v[1024];
	int c;

	gpcc_log_info(line);

	content = apr_strtok(line, "|", &last);
	definedprimary = apr_strtok(0, "|", &last);
	dbid = apr_strtok(0, "|", &last);
	isprimary = apr_strtok(0, "|", &last);
	valid = apr_strtok(0, "|", &last);
	hostname = apr_strtok(0, "|", &last);
	port = apr_strtok(0, "|", &last);
	datadir = apr_strtok(0, "|\r\n", &last);

	/* key */
	strcpy(key, "seg.");
	c = strtol(content, 0, 0);
	if (c == -1)
	    strcat(key, "m");
	else
	    strcat(key, content);
	strcat(key, ".");
	if (*definedprimary == 't')
	    strcat(key, "0");
	else 
	    strcat(key, "1");

	/* value */
	sprintf(v, "%s:%s:%s", hostname, port, datadir);

	config_set(gpma.cfg, key, v);
    }

    if (stop) {
	/* stop master QD */
	if ((e = gpcc_proc_pg_ctl_stop(&proc, host, master_port, dir, "immediate", 1, 1, p)) != 0) {
	    gpcc_log_error("stop master QD failure");
	    goto bail;
	}
    }

    config_set(gpma.cfg, CONFIG_DBNAME_KEY, gpma.array_name);


    bail:
    if (p) apr_pool_destroy(p);

    return e;

}

/** 
    check master QD host in /gp/$USER/$ARRAY/gpma/qd on paxos
    if file not exists then
        try to be a leader
    else 
        check master file /gp/$USER/$ARRAY/gpma/qd
	if same as master host then
	    try to be a leader
	else
	    be a slave
        endif
    endif
*/
apr_status_t a_lma_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int exist = 0;
    int is_localhost = 0;
    char path[1024];
    
    sprintf(path, "/gp/%s/%s/gpma/qd", gpma.user, gpma.array_name);

    if (a_lma_paxos_exists(ev, arg, path, &exist) != 0) {
	/* paxos failed.  retry in 10 second */
	gpcc_log_error("paxos failure. retry in 10 seconds");
	slot->countdown = LMA_RETRY_PERIOD;
	return 0;
    }

    if (! exist) {
	/* elect ourself to be a lead MA */
	evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    } else {
	if (a_lma_check_qd_is_localhost(ev, arg, path, &is_localhost) != 0) {
	    slot->countdown = LMA_RETRY_PERIOD;
	    return 0;
	}
	if (is_localhost) {
	    /* elect ourself to be a lead MA */
	    evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    /* be a slave */
	    evt = gpcc_event_alloc(LMA_STANDBY_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	}
    }

    return 0;
}

apr_status_t a_lma_sleep(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->countdown = LMA_RETRY_PERIOD;
    return 0;
}

apr_status_t a_lma_electself(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int e;
    char path[1024];
    char content[1024];

    if (slot->u.lma.zhandle && is_unrecoverable(slot->u.lma.zhandle)) {
	zookeeper_close(slot->u.lma.zhandle);
	slot->u.lma.zhandle = 0;
    }

    if (! slot->u.lma.zhandle) {
	slot->u.lma.zhandle = zookeeper_init(gpma.px_address, NULL, 0, NULL, NULL, 0);
	if (! slot->u.lma.zhandle) {
	    gpcc_log_error("can't connect to paxos with address %s", gpma.px_address);
	    return APR_EINVAL;
	}
    }

    if ((e = a_lma_leader_lease(ev, arg)) != 0) {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    /* TODO: write address of Lead MA to /gp/$USER/$ARRAY/gpma/leader */
    sprintf(path, "/gp/%s/%s/gpma/leader", gpma.user, gpma.array_name);
    sprintf(content, "%s:%d", gpma.hostname, gpma.ma_port);
    if ((e = a_lma_paxos_set(ev, arg, path, content, -1)) != 0) {
	gpcc_log_error("MA get the lease but can't write file %s to paxos", path);
	    
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);	    
	return 0;

    }

    slot->u.lma.leader = apr_psprintf(slot->pool, "%s:%d", gpma.hostname, gpma.ma_port);
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);	    
    return 0;
}

apr_status_t a_lma_leader(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;

    /* sync the gp_configuration */
    a_lma_sync_config(ev, arg);

    /* kick-off fts cmdslot */
    evt = gpcc_event_alloc(INIT_EVENT, gpma.fts_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    slot->countdown = LMA_RENEW_PERIOD;

    return 0;
}


apr_status_t a_lma_renew_sleep(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    slot->countdown = LMA_RENEW_PERIOD;
    return 0;
}


/**
 *  check lock
 *  if lock not exists then
 *      try to be leader
 *  else 
 *      wait for expireTime from the zookeeper_lease_expireTime
 *  fi
 */
apr_status_t a_lma_standby_check_leader(gpcc_event_t * ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    char path[1024], buf[1024];
    int e, len, exists = 0;
    time_t tm;
    struct Stat stat;

    sprintf(path, "/gp/%s/%s/gpma/lock", gpma.user, gpma.array_name);
    if ((e = a_lma_paxos_exists(ev, arg, path, &exists)) != 0) {
	slot->countdown = LMA_RETRY_PERIOD; /* PAXOS ERROR.  RETRY */
	return 0;
    }
    
    if (exists) {
	if ((e = a_lma_leader_lease_getExpireTime(ev, arg, &tm)) != 0) {
	    slot->countdown = LMA_RETRY_PERIOD;
	} else {
	    ;
	    /* slot->countdown = tm->sec + 1; */
	}

	/* TODO: read /gp/$USER/$ARRAY/gpma/leader to get the lead MA */
	sprintf(path, "/gp/%s/%s/gpma/leader", gpma.user, gpma.array_name);
	len = sizeof(buf);
	if ((e = a_lma_paxos_get(ev, arg, path, buf, &len, &stat)) != 0) {
	    gpcc_log_error("paxos: get file %s error. ignore it and come back later.");
	} else {
	    if (! slot->u.lma.leader || 
		(slot->u.lma.leader && strcmp(slot->u.lma.leader, buf) != 0)) {
		    slot->u.lma.leader = apr_pstrdup(slot->pool, buf);
	    }
	}
    } else {
	evt = gpcc_event_alloc(LMA_ELECTSELF_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
	     
    return 0;
}

apr_status_t a_lma_renew(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int e;
    
    /* renew the lease */
    if ((e = a_lma_leader_lease_renew(ev, arg)) == 0) {
	slot->countdown = LMA_RENEW_PERIOD;
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);	
    }
    
    return 0;

}

apr_status_t a_lma_leader_giveup(gpcc_event_t *ev, void *arg) 
{
    cmdslot_t *slot = arg;

    a_lma_leader_lease_giveup(ev, arg);
    slot->countdown = 35; /* make sure standby MA get the lease */
    return 0;
}

/**
 *  cleanup fts_cmdslot
 */
apr_status_t a_lma_leader_reset(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;


    slot->countdown = LMA_RETRY_PERIOD;
    return 0;
}
