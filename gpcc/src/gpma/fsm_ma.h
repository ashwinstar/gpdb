#define LMA_EXPIRE_PERIOD 30 /* 30 seconds */
#define LMA_RENEW_PERIOD  20 /* 20 seconds */
#define LMA_RETRY_PERIOD  10 /* 10 seconds */

static apr_status_t a_lma_sync_config(gpcc_event_t *ev, void *arg, char *master);

/* CHUBBY */
static apr_status_t a_lma_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    slot->u.lma.chubby.conn = chubby_makeConnection(gpma.chubby);
    gpcc_log_info("MA make a chubby connection.");
    return 0;
}

/* mkdir directory */
static apr_status_t a_lma_mkdir(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;
    gpcc_event_t *evt;
    struct chubby_stat stat;

    gpcc_log_info("mkdir %s", gpma.chubby_array_dir);
    e = chubby_connection_nonAtomicMakeManyDirs(slot->u.lma.chubby.conn,
						gpma.chubby_array_dir);
    if (e && e != CHUBBY_E_EXIST) {
	gpcc_log_error("chubby_connection_nonAtomMakeManyDirs %s failed", gpma.chubby_array_dir);
	slot->countdown = 5;
	return 0;
    }
    gpcc_log_info("mkdir %s. ret=%d", gpma.chubby_array_dir, e);

    gpcc_log_info("mkdir %s", gpma.chubby_etc_dir);

    e = chubby_connection_mkdir(slot->u.lma.chubby.conn,
				gpma.chubby_etc_dir);
    if (e && e != CHUBBY_E_EXIST) {
	gpcc_log_error("chubby_connection_nonAtomMakeManyDirs %s failed", gpma.chubby_etc_dir);
	slot->countdown = 5;
	return 0;
    }
    gpcc_log_info("mkdir %s. ret=%d", gpma.chubby_etc_dir, e);

    e = chubby_connection_stat(slot->u.lma.chubby.conn,
			       &stat,
			       gpma.chubby_passwd_file);

    gpcc_log_info("stat %s %d. ret =%d", gpma.chubby_passwd_file, stat.decreeNumber, e);
    if (e == CHUBBY_E_NOENT || (!e && stat.decreeNumber == 0)) {
	/* empty /gpcc/etc/ directory.  create default password file */
	const char *user = "gpadmin";
	char encoded[ADMIN_PASSWORD_ENCODED_LENGTH];
	char buf[1024];
	chubby_string_t *content = (chubby_string_t *) buf;

	apr_md5_encode(user, user, encoded, sizeof(encoded));
	sprintf(content->a, "%s:%s\n", user, encoded);
	content->length = strlen(content->a) + 1;
	
	gpcc_log_info("create default password file %s. content = '%s'", gpma.chubby_passwd_file, content->a);
	e = chubby_connection_put(slot->u.lma.chubby.conn,
				  gpma.chubby_passwd_file,
				  content);
	
	if (e) {
	    gpcc_log_error("chubby_connection_put %s failed", gpma.chubby_passwd_file);
	    slot->countdown = 5;
	    return 0;
	}
    }

    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;

}

/* get the lease */
static apr_status_t a_lma_get_lease(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    gpcc_log_info("get lease %s", gpma.chubby_leader_lease);
    e = chubby_connection_asynchronous_getLease(slot->u.lma.chubby.conn,
						chubby_get_lease_cb,
						(void *) &slot->cmdidx,
						gpma.chubby_leader_lease,
						LMA_EXPIRE_PERIOD+5, 
						LMA_EXPIRE_PERIOD);

    if (e) {
	gpcc_log_error("chubby_connection_asynchronous_getLease failure. ret=%d", e);
    }
					    
    return 0;
}

/* get lease reply */
static apr_status_t a_lma_get_lease_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    gpcc_event_t *evt;

    if (result->retval) {
	gpcc_log_info("MA failed to get the lease. ret=%d", result->retval);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    gpcc_log_info("MA get the lease and become a Lead");
    slot->u.lma.chubby.lease = result->u.lease.conn;
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

/* being a leader. schedule a renew time and put a leader file to chubby */
static apr_status_t a_lma_leader(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char buf[1024];
    chubby_string_t *c = (chubby_string_t *) buf;
    int e;

    slot->countdown = LMA_RENEW_PERIOD;
    
    sprintf(c->a, "%s:%d", gpma.hostname, gpma.ma_port);
    c->length = strlen(c->a) + 1;
    
    gpcc_log_info("MA is putting %s file to paxos. content =%s", gpma.chubby_leader_file, c->a);
    e = chubby_connection_asynchronous_put(slot->u.lma.chubby.lease,
					   chubby_put_cb,
					   (void *) &slot->cmdidx,
					   gpma.chubby_leader_file,
					   c);

    return 0;
}

/* format of master file.  host:port:datadir */
static apr_status_t a_lma_leader_get_master(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    /* get the leader file */
    gpcc_log_info("MA is getting %s file", gpma.chubby_master_file);
    e = chubby_connection_asynchronous_get(slot->u.lma.chubby.lease,
					   chubby_get_cb,
					   (void *) &slot->cmdidx,
					   gpma.chubby_master_file,
					   -1);	

    return 0;
}

/* leader got put reply from chubby */
static apr_status_t a_lma_leader_put_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    gpcc_event_t *evt;
    char buf[1024];

    if (result->retval) {
	gpcc_log_error("Lead MA failed to put the leader file. ret=%d", result->retval);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;	
    }

    /* update slot->u.lma.leader */
    gpcc_log_info("Lead MA put the leader file successfully");
    sprintf(buf, "%s:%d", gpma.hostname, gpma.ma_port);
    if (slot->u.lma.leader)
	free(slot->u.lma.leader);
    slot->u.lma.leader = strdup(buf);


    /* get master file */
    a_lma_leader_get_master(ev, arg);

    return 0;
}


static apr_status_t a_lma_leader_get_master_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    gpcc_event_t *evt;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	gpcc_log_error("Lead MA failed to get master file. ret=%d", result->retval);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;	
    }
    
    if (result->retval == 0) {
	/* array found  and sync the gp_configuration */	
	gpcc_log_info("Lead MA get leader file successfully.");
	gpcc_log_info("len = %d, content = %s", result->u.get.content.length, result->u.get.content.a);
	a_lma_sync_config(ev, arg, result->u.get.content.a);
    }

    /* kick-off fts cmdslot */
    evt = gpcc_event_alloc(INIT_EVENT, gpma.fts_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    return 0;
}

/**
 *  leader failed to do anything via chubby 
 *
 *  Lead MA should not give up the lease or fail to renew the lease.  If this happens,  
 *  we will shutdown the Lead MA and let the standby MA take over.
 *
 **/
static apr_status_t a_lma_leader_failure(gpcc_event_t *ev, void *arg)
{

    gpcc_fatal(FLINE, "Lead MA failed to connect to chubby.  Chubby probably crashed.  Lead MA shutdown immediately");
    
#if 0
    cmdslot_t *slot = arg;

    if (slot->u.lma.leader) {
	free(slot->u.lma.leader);
	slot->u.lma.leader = 0;
    }
    if (slot->u.lma.chubby.lease) {
	chubby_connection_close(slot->u.lma.chubby.lease);
	slot->u.lma.chubby.lease = 0;
    }

    /* become leadless and schedule a retry */
    slot->countdown = LMA_RETRY_PERIOD;

    /* TODO: cleanup fts cmdslot */
#endif

    return 0;
}

/* leader renew the lease */
static apr_status_t a_lma_leader_renew(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    gpcc_log_info("start renewing the lease");
    e = chubby_connection_asynchronous_renew(slot->u.lma.chubby.lease,
					     chubby_renew_cb,
					     (void *) &slot->cmdidx,
					     LMA_EXPIRE_PERIOD);
    return 0;
}

/* leader renew reply from chubby */
static apr_status_t a_lma_leader_renew_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    gpcc_event_t *evt;

    if (result->retval) {
	gpcc_log_error("Lead MA failed to renew lease. ret=%d", result->retval);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	return 0;
    }

    gpcc_log_info("Lead MA renewed the lease successfully.");
    slot->countdown = LMA_RENEW_PERIOD;
    return 0;
}


/* standby -- schedule the time for getting the lease */
static apr_status_t a_lma_standby(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    /* schedule time for getting the lease */
    slot->countdown = LMA_RETRY_PERIOD;
    return 0;
}



/* standby get lease */
static apr_status_t a_lma_standby_get_lease(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int e;

    e = chubby_connection_asynchronous_getLease(slot->u.lma.chubby.conn,
						chubby_get_lease_cb,
						(void *) &slot->cmdidx,
						gpma.chubby_leader_lease,
						LMA_EXPIRE_PERIOD+5, 
						LMA_EXPIRE_PERIOD);
    
    return 0;
}

/* standby get lease reply */
static apr_status_t a_lma_standby_get_lease_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    gpcc_event_t *evt;
    int e;

    if (result->retval) {
	/* get the leader file */
	gpcc_log_info("standby MA failed to get lease");
	e = chubby_connection_asynchronous_get(slot->u.lma.chubby.conn,
					       chubby_get_cb,
					       (void *) &slot->cmdidx,
					       gpma.chubby_leader_file,
					       -1);

	return 0;
    }

    gpcc_log_info("standby MA got the lease and become a Lead MA");
    slot->u.lma.chubby.lease = result->u.lease.conn;
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

/* standby get reply from chubby */
static apr_status_t a_lma_standby_get_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    
    slot->countdown = LMA_RETRY_PERIOD;
    if (result->retval) {
	gpcc_log_info("standby MA failed to get leader file.");
	return 0;
    }

    gpcc_log_info("standby MA got the leader file");
    gpcc_log_info("len = %d, content = %s", result->u.get.content.length, result->u.get.content.a);
    if (!slot->u.lma.leader) {
	slot->u.lma.leader = strdup(result->u.get.content.a);
    } else if (strcmp(slot->u.lma.leader, result->u.get.content.a) != 0) {
	free(slot->u.lma.leader);
	slot->u.lma.leader = strdup(result->u.get.content.a);
    }
    return 0;
}


/* sync the gp_configuration and gpcc config */
static apr_status_t a_lma_sync_config(gpcc_event_t *ev, void *arg, char *master)
{
    int i, e = 0, stop = 0;
    apr_pool_t *p = 0;
    apr_proc_t proc;
    char key[128], line[1024];
    char master_host[1024];
    char *master_port_str;
    char master_datadir[1024];
    int master_port;
    char *last, *s;

    strcpy(line, master);
    s = apr_strtok(line, ":", &last);
    strcpy(master_host, s);
    master_port_str = apr_strtok(0, ":", &last);
    s = apr_strtok(0, ":", &last);
    strcpy(master_datadir, s);
    master_port = strtol(master_port_str, 0, 0);

    if (apr_pool_create(&p, 0))
	gpcc_fatal(FLINE, "apr_pool_create: not enough memory\n");
	
    /* check if QD is running */
    /* get gp_configuration and reset config */
    if ((e = gpcc_proc_psql(&proc, master_host, master_port, "template1",
			    "select * from gp_configuration order by content",
			    1, 1, p)) != 0) {

	/* start master QD in admin mode */
	if ((e = gpcc_proc_pg_ctl_start_master(&proc, master_host, master_port, master_datadir, 1, 1, 1, p)) != 0) {
	    gpcc_log_error("start master QD failure");
	    goto bail;
	}
	
	if ((e = gpcc_proc_psql(&proc, master_host, master_port, "template1",
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
	sprintf(v, "%s:%s:%s:%s:%s", hostname, port, datadir, isprimary, valid);

	config_set(gpma.cfg, key, v);
    }

    if (stop) {
	/* stop master QD */
	if ((e = gpcc_proc_pg_ctl_stop(&proc, master_host, master_port, master_datadir, "immediate", 1, 1, p)) != 0) {
	    gpcc_log_error("stop master QD failure");
	    goto bail;
	}
    }

    config_set(gpma.cfg, CONFIG_DBNAME_KEY, gpma.array_name);


    bail:
    if (p) apr_pool_destroy(p);

    return e;

}



#if 0
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
#endif
