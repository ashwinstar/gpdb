


static apr_status_t a_ssh_gethostname(gpcc_event_t *ev, void *arg)
{
#define MAX_THREAD_NUM 20

    cmdslot_t *slot = arg;
    int i,e, nthread;
    char *argv[MAX_THREAD_NUM+1];

    nthread = 0;

    slot->u.gethostname.start = slot->u.gethostname.offset;
    if (slot->u.gethostname.start + MAX_THREAD_NUM > slot->u.gethostname.nhost ) {
	slot->u.gethostname.offset = slot->u.gethostname.nhost;
    } else {
	slot->u.gethostname.offset = slot->u.gethostname.start + MAX_THREAD_NUM;	
    }

    for (i = slot->u.gethostname.start ; i < slot->u.gethostname.offset ; i++) {
	argv[nthread] = (char *) slot->u.gethostname.hosts[i];
	nthread++;
    }
    argv[nthread] = 0;
    slot->u.gethostname.nthread = nthread;
    slot->u.gethostname.nsuccess = 0;
    slot->u.gethostname.nfailure = 0;
    if ((e = gpcc_proc_gpssh(&slot->u.gethostname.proc,
			     (const char *const *) argv,
			     "hostname",
			     0,
			     slot->pool)) != 0) {
	gpcc_log_error("gpcc_pg_gethostname_by_hosts: gpssh failed.");
	return APR_EINVAL;
    }

    slot->countdown = 3;
    return 0;
}

static apr_status_t a_ssh_gethostname_wait(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int i, e, nsuccess, nfailure;
    apr_proc_t *p;
    int ret;
    apr_exit_why_e exitwhy;
    gpcc_event_t* evt;
    char buf[1024];
    char *ptr;
    apr_size_t nbytes = sizeof(buf);
    apr_size_t nread = 0;

    nsuccess = nfailure = 0;
    for (i = 0, p = slot->u.gethostname.proc ; 
	 i < slot->u.gethostname.nthread ; 
	 i++, p++) {

	e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
	if (e == APR_CHILD_DONE) {
	    if (ret == 0) {
		nsuccess++;
		gpcc_log_info("gethostname[%d] succeeded", i);
	    } else {
		nfailure++;
		gpcc_log_info("gethostname[%d] failed", i);
	    }
	}
    }
    
    slot->u.gethostname.nsuccess += nsuccess;
    slot->u.gethostname.nfailure += nfailure;
    gpcc_log_info("gethostname:  success = %d, failure = %d, total = %d", 
		  slot->u.gethostname.nsuccess, 
		  slot->u.gethostname.nfailure, 
		  slot->u.gethostname.nthread);

    if (slot->u.gethostname.nsuccess + slot->u.gethostname.nfailure == slot->u.gethostname.nthread) {

	if (slot->u.gethostname.nfailure > 0) {
	    /* failure */

	    for (i = 0, p = slot->u.gethostname.proc ; 
		 i < slot->u.gethostname.nthread ; 
		 i++, p++) {
		
		e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
		if (e == APR_CHILD_DONE) {
		    if (ret) {
			gpcc_log_error("ssh to host %s failed", slot->u.gethostname.hosts[slot->u.gethostname.start+i]);
		    }
		}
	    }	    

	    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);	    
	    return 0;
	}
	
	for (i = 0, p = slot->u.gethostname.proc ;
	     i < slot->u.gethostname.nthread ;
	     i++, p++) {
	    
	    if ((e = apr_file_read_full(p->out, buf, nbytes, &nread)) != 0 &&
		e != APR_EOF) {
		gpcc_log_error("ssh hostname: get string from hostname failure\n");
		evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);	    		    
		return 0;
	    }
	    buf[nread] = 0;
	    ptr = gpcc_string_trim(buf);
	    slot->u.gethostname.hostname[slot->u.gethostname.start+i] = apr_pstrdup(slot->pool, ptr);
	    gpcc_log_info("hostname of %s -> %s", 
			  slot->u.gethostname.hosts[slot->u.gethostname.start+i],
			  slot->u.gethostname.hostname[slot->u.gethostname.start+i]);
			  
	}

	if (slot->u.gethostname.offset < slot->u.gethostname.nhost) {
	    /* continue */
	    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    /* success */
	    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);	    
	}
    } else {
	slot->countdown = 3;
    }

    slot->u.gethostname.timeout--;
    if (slot->u.gethostname.timeout < 0) {
	gpcc_log_error("ssh hostname: timed out");
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

static apr_status_t a_ssh_gethostname_success(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int i;
    struct json_object *obj;
    struct json_object *rset;
    struct json_object *tuple;
    char *objstr;
    
    obj = gpcc_json_http_reply(gpma.array_name, 0, "gethostname", "ok", 0, 0);

    rset = json_object_new_array();
    for (i = 0 ; i < slot->u.gethostname.nhost ; i++) {
	tuple = json_object_new_array();
	json_object_array_add(tuple, json_object_new_string(slot->u.gethostname.hosts[i]));
	json_object_array_add(tuple, json_object_new_string(slot->u.gethostname.hostname[i]));	
	json_object_array_add(rset, tuple);
    }
    json_object_object_add(obj, "rset", rset);

    objstr = json_object_to_json_string(obj);
    gpcc_log_info("gethosname succeeded.  JSON reply= %s", objstr);
    evt = gpcc_event_alloc(SSH_HOSTNAME_REPLY_EVENT, slot->u.gethostname.parent_cmdidx, strlen(objstr)+1);
    strcpy(evt->pdata, objstr);
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_ssh_gethostname_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    struct json_object *obj;
    char *objstr;
    int i;
    apr_proc_t *p;

    for (i = 0, p = slot->u.gethostname.proc ; i < slot->u.gethostname.nthread ; i++, p++) {
	int e, ret;
	apr_exit_why_e exitwhy;
	e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
	if (e != APR_CHILD_DONE) {
	    apr_proc_kill(p, SIGTERM);
	}
    }

	
    obj = gpcc_json_http_reply(gpma.array_name, 0, "gethostname", "error", 0, "ssh hostname failed");
    objstr = json_object_to_json_string(obj);
    gpcc_log_info("gethostname failed. JSON reply = %s", objstr);
    evt = gpcc_event_alloc(SSH_HOSTNAME_REPLY_EVENT, slot->u.gethostname.parent_cmdidx, strlen(objstr)+1);
    strcpy(evt->pdata, objstr);
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}
    

static apr_status_t a_ssh_getaddress(gpcc_event_t *ev, void *arg)
{
#define MAX_THREAD_NUM 20

    cmdslot_t *slot = arg;
    int i,e, n, nthread;
    char *argv[MAX_THREAD_NUM+1];
    char out[1024], cmd[1024];

#ifdef SOLARIS2
    char *ifconfig_txt = "-a inet";
#else
    char *ifconfig_txt = "";
#endif
    
    n = sizeof(out);
    if (gpcc_proc_findCmdInPath("ifconfig", out, &n)) {
	gpcc_fatal(FLINE, "ifconfig command not found");
    }
    sprintf(cmd, "%s %s | grep inet | grep -v 127.0.0 | grep -v inet6", out, ifconfig_txt);  

    nthread = 0;

    slot->u.getaddress.start = slot->u.getaddress.offset;
    if (slot->u.getaddress.start + MAX_THREAD_NUM > slot->u.getaddress.nhost ) {
	slot->u.getaddress.offset = slot->u.getaddress.nhost;
    } else {
	slot->u.getaddress.offset = slot->u.getaddress.start + MAX_THREAD_NUM;	
    }

    for (i = slot->u.getaddress.start ; i < slot->u.getaddress.offset ; i++) {
	argv[nthread] = (char *) slot->u.getaddress.hosts[i];
	nthread++;
    }
    argv[nthread] = 0;
    slot->u.getaddress.nthread = nthread;
    slot->u.getaddress.nsuccess = 0;
    slot->u.getaddress.nfailure = 0;
    if ((e = gpcc_proc_gpssh(&slot->u.getaddress.proc,
			     (const char *const *) argv,
			     cmd,
			     0,
			     slot->pool)) != 0) {
	gpcc_log_error("gpcc_pg_getaddress_by_hosts: gpssh failed.");
	return APR_EINVAL;
    }

    slot->countdown = 3;
    
    return 0;

}

static apr_status_t a_ssh_getaddress_wait(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    int i, e, n, nn, nsuccess, nfailure;
    apr_proc_t *p;
    int ret;
    apr_exit_why_e exitwhy;
    gpcc_event_t* evt;
    char out[1024], addr[1024], *s;
    char *ptr, *tok, *last;

    nsuccess = nfailure = 0;
    for (i = 0, p = slot->u.getaddress.proc ; 
	 i < slot->u.getaddress.nthread ; 
	 i++, p++) {

	e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
	if (e == APR_CHILD_DONE) {
	    if (ret == 0) {
		nsuccess++;
	    } else {
		nfailure++;
	    }
	}
    }

    slot->u.getaddress.nsuccess += nsuccess;
    slot->u.getaddress.nfailure += nfailure;
    gpcc_log_info("getaddress:  success = %d, failure = %d, total = %d", 
		  slot->u.getaddress.nsuccess, 
		  slot->u.getaddress.nfailure, 
		  slot->u.getaddress.nthread);

    if (slot->u.getaddress.nsuccess + slot->u.getaddress.nfailure == slot->u.getaddress.nthread) {

	if (slot->u.getaddress.nfailure > 0) {
	    /* failure */
	    for (i = 0, p = slot->u.getaddress.proc ; 
		 i < slot->u.getaddress.nthread ; 
		 i++, p++) {
		
		e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
		if (e == APR_CHILD_DONE) {
		    if (ret) {
			gpcc_log_error("ssh to host %s failed", slot->u.getaddress.hosts[slot->u.getaddress.start+i]);
		    }
		}
	    }	    
	    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);	    
	    return 0;
	}
	
	for (i = 0, p = slot->u.getaddress.proc ;
	     i < slot->u.getaddress.nthread ;
	     i++, p++) {
	    
	    /* get the first line */
	    *addr = 0;
	    n = sizeof(addr);
	    while (apr_file_gets(out, sizeof(out), p->out) == 0) {
		tok = apr_strtok(out, " ", &last);
		if (!tok)
		    return APR_EINVAL;
		
		tok = apr_strtok(0, " ", &last);
		if (!tok || *tok == 0)
		    return APR_EINVAL;
		
		if ((ptr = strchr(tok, ':')) == 0) {
		    nn = strlen(tok) + 1;
		    if (n < nn)
			return APR_EINVAL;
		    
		    strcat(addr, tok);
		    strcat(addr, " ");
		    n -= nn;
		    
		} else {
		    nn = strlen(ptr+1) + 1;
		    if (n < nn)
			return APR_EINVAL;
		    
		    strcat(addr, ptr+1);
		    strcat(addr, " ");
		    
		    n -= nn;
		}
	    }	

	    s = gpcc_string_trim(addr);
	    slot->u.getaddress.address[slot->u.getaddress.start+i] = apr_pstrdup(slot->pool, s);
	    gpcc_log_info("address of %s -> %s", 
			  slot->u.getaddress.hosts[slot->u.getaddress.start+i],
			  slot->u.getaddress.address[slot->u.getaddress.start+i]);
	}

	if (slot->u.getaddress.offset < slot->u.getaddress.nhost) {
	    /* continue */
	    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);
	} else {
	    /* success */
	    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
	    gpcc_evqueue_put(gpma.evq, evt);	    
	}
    } else {
	slot->countdown = 3;
    }

    slot->u.getaddress.timeout--;
    if (slot->u.getaddress.timeout < 0) {
	gpcc_log_error("ssh hostname: timed out");
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }
    return 0;
}

static apr_status_t a_ssh_getaddress_success(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    int i;
    struct json_object *obj;
    struct json_object *rset, *tuple;
    char *objstr;

    obj = gpcc_json_http_reply(gpma.array_name, 0, "getaddress", "ok", 0, 0);

    rset = json_object_new_array();
    for (i = 0 ; i < slot->u.getaddress.nhost ; i++) {
	tuple = json_object_new_array();
	json_object_array_add(tuple, json_object_new_string(slot->u.getaddress.hosts[i]));
	json_object_array_add(tuple, json_object_new_string(slot->u.getaddress.address[i]));	
	json_object_array_add(rset, tuple);
    }
    json_object_object_add(obj, "rset", rset);

    objstr = json_object_to_json_string(obj);
    gpcc_log_info("getaddress succeeded. JSON reply = %s", objstr);
    evt = gpcc_event_alloc(SSH_ADDRESS_REPLY_EVENT, slot->u.getaddress.parent_cmdidx, strlen(objstr)+1);
    strcpy(evt->pdata, objstr);
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}

static apr_status_t a_ssh_getaddress_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    gpcc_event_t *evt;
    struct json_object *obj;
    char *objstr;
    int i;
    apr_proc_t *p;

    for (i = 0, p = slot->u.getaddress.proc ; i < slot->u.getaddress.nthread ; i++, p++) {
	int e, ret;
	apr_exit_why_e exitwhy;
	e = apr_proc_wait(p, &ret, &exitwhy, APR_NOWAIT);
	if (e != APR_CHILD_DONE) {
	    apr_proc_kill(p, SIGTERM);
	}
    }

    obj = gpcc_json_http_reply(gpma.array_name, 0, "getaddress", "error", 0, "get IP address failed");
    objstr = json_object_to_json_string(obj);
    gpcc_log_info("getaddress failed. JSON reply = %s", objstr);
    evt = gpcc_event_alloc(SSH_ADDRESS_REPLY_EVENT, slot->u.getaddress.parent_cmdidx, strlen(objstr)+1);
    strcpy(evt->pdata, objstr);
    gpcc_evqueue_put(gpma.evq, evt);
    json_object_put(obj);
    return 0;
}
    
