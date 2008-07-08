
static apr_status_t a_admin_forward(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *admin_slot;
    gpcc_event_t *evt;

    switch (ev->etype) {
    case ADMIN_ADD_EVENT:
	admin_slot = cmdslot_alloc("ADMIN_ADD_CMD", ADMIN_ADD_CMDSTATE);
	break;
    case ADMIN_DELETE_EVENT:
	admin_slot = cmdslot_alloc("ADMIN_DELETE_CMD", ADMIN_DELETE_CMDSTATE);
	break;
    case ADMIN_LIST_EVENT:
	admin_slot = cmdslot_alloc("ADMIN_LIST_CMD", ADMIN_LIST_CMDSTATE);
	break;
    case ADMIN_PASSWORD_EVENT:
	admin_slot = cmdslot_alloc("ADMIN_PASSWORD_CMD", ADMIN_PASSWORD_CMDSTATE);
	break;
    default:
	gpcc_log_error("invalid admin event type = %d", ev->etype);
	return 0;
    }
	
    evt = gpcc_event_alloc(ev->etype, admin_slot->cmdidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

/* general function to access password file */
static apr_status_t a_admin_get_password_file_lease(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    gpcc_log_info("get password lease %s", gpma.chubby_passwd_lease);
    slot->u.admin.chubby.conn = chubby_makeConnection(gpma.chubby);
    chubby_connection_asynchronous_getLease(slot->u.admin.chubby.conn,
					    chubby_get_lease_cb,
					    (void *) &slot->cmdidx,
					    gpma.chubby_passwd_lease,
					    15, 10);

    return 0; 
}

static apr_status_t a_admin_get_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;

    if (result->retval) {
	/* fail to get password file lease */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin", FAIL_STATUS, 0, "No password file found");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    slot->u.admin.chubby.lease = result->u.lease.conn;
    chubby_connection_asynchronous_get(slot->u.admin.chubby.lease,
				       chubby_get_cb,
				       (void *) &slot->cmdidx,
				       gpma.chubby_passwd_file,
				       -1);
				       
    return 0; 
}

static apr_status_t a_admin_giveup_lease(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    
    if (result->retval) {
	/* fail to set password file */
	gpcc_log_error("put password file %s failed", gpma.chubby_passwd_file);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin", FAIL_STATUS, 0, "failed to save password file");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    /* give up lease anyway */
    gpcc_log_info("give up the password lease %s", gpma.chubby_passwd_lease);
    chubby_connection_asynchronous_giveUpLease(slot->u.admin.chubby.lease,
					       chubby_giveup_lease_cb,
					       (void *) &slot->cmdidx);
				       
    
    return 0; 
}

/* admin add */
static apr_status_t a_admin_add_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri;
    gpcc_event_t *evt;
    struct json_object *obj;
    const char *user, *password;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_param_alloc(url);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    password = gpcc_net_uri_param(uri, "password");
    
    if (! user || ! password) {
	goto bail;
    }

    slot->u.admin.conn = conn;
    slot->u.admin.username = apr_pstrdup(slot->pool, user);
    slot->u.admin.password = apr_pstrdup(slot->pool, password);
    
    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    if (uri)
	gpcc_net_uri_free(uri);

    return 0;

    bail:
    
    if (uri)
	gpcc_net_uri_free(uri);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin add", FAIL_STATUS, 0, "missing parameter");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    return 0;
}

static apr_status_t a_admin_add_set_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    chubby_string_t *content = 0;
    char line[256];
    char encoded[ADMIN_PASSWORD_ENCODED_LENGTH];
    int len;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	/* fail to get password file */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin add", FAIL_STATUS, 0, "No password file found");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    apr_md5_encode(slot->u.admin.password,
		   slot->u.admin.username,
		   encoded,
		   sizeof(encoded));

    sprintf(line, "%s:%s\n", slot->u.admin.username, encoded);
    gpcc_log_info("add user '%s', password = '%s', encoded = '%s'", 
		  slot->u.admin.username,
		  slot->u.admin.password,
		  encoded);

    if (result->retval == CHUBBY_E_NOENT) {
	/* file not exists */
	len = strlen(line) + 1;
	content = malloc(sizeof(*content) + len);
	content->length = len;
	strcpy(content->a, line);
	
    } else {
	/* add new entry to the file */
	len = result->u.get.content.length + strlen(line) + 1;
	content = malloc(sizeof(*content) + len);
	content->length = len;
	*content->a = 0;
	strcat(content->a, result->u.get.content.a);
	strcat(content->a, line);
    }

    /* save the password file */
    chubby_connection_asynchronous_put(slot->u.admin.chubby.lease,
				       chubby_put_cb,
				       (void *) &slot->cmdidx,
				       gpma.chubby_passwd_file,
				       content);

    if (content) 
	free(content);

    return 0;
}

static apr_status_t a_admin_add_done(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;

    if (result->retval) {
	/* fail to give up the lease */
	gpcc_log_error("admin add: failed to give up password lease %s. ret=%d", gpma.chubby_passwd_lease, result->retval);
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin add", FAIL_STATUS, 0, "Failed to give up lease");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;	
    }

    gpcc_log_info("admin add: password lease gave up successfully.");
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin add", OK_STATUS, 0, "");
    gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0; 
}

static apr_status_t a_admin_cleanup(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    if (slot->u.admin.chubby.lease)
	chubby_connection_close(slot->u.admin.chubby.conn);	

    if (slot->u.admin.chubby.conn)
	chubby_connection_close(slot->u.admin.chubby.conn);
    return 0;
}

static apr_status_t a_admin_add_success(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

static apr_status_t a_admin_add_failure(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

/* admin delete */
static apr_status_t a_admin_delete_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri;
    gpcc_event_t *evt;
    struct json_object *obj;
    const char *user;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_alloc(url);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    
    if (! user) {
	goto bail;
    }

    slot->u.admin.conn = conn;
    slot->u.admin.username = apr_pstrdup(slot->pool, user);
    
    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    if (uri)
	gpcc_net_uri_free(uri);

    return 0;

    bail:
    
    if (uri)
	gpcc_net_uri_free(uri);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin delete", FAIL_STATUS, 0, "missing parameter");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    return 0;
}

static apr_status_t a_admin_delete_set_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    chubby_string_t *content = 0;
    char *buf, *tok, *last, *lastlast;
    char *user, *passwd;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	/* fail to get password file */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin delete", FAIL_STATUS, 0, "Failed to get password file");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 
    
    if (result->retval == CHUBBY_E_NOENT) {
	/* file not exists */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin delete", FAIL_STATUS, 0, "Error: no such user.");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    /* add new entry to the file */
    buf = malloc(sizeof(chubby_string_t) + result->u.get.content.length);
    content = (chubby_string_t *) buf;
    *content->a = 0;

    for (tok = apr_strtok(result->u.get.content.a, "\r\n", &last) ; 
	 tok ; 
	 tok = apr_strtok(0, "\r\n", &last)) {

	if (*tok == 0) 
	    continue;

	user = apr_strtok(tok, ":", &lastlast);
	passwd = apr_strtok(0, "\r\n", &lastlast);

	user = gpcc_string_trim(user);
	passwd = gpcc_string_trim(passwd);

	if (strcmp(user, slot->u.admin.username) != 0) {
	    strcat(content->a, user);
	    strcat(content->a, ":");
	    strcat(content->a, passwd);
	    strcat(content->a, "\n");
	}
    }

    content->length = strlen(content->a) + 1;

    /* save the password file */
    chubby_connection_asynchronous_put(slot->u.admin.chubby.lease,
				       chubby_put_cb,
				       (void *) &slot->cmdidx,
				       gpma.chubby_passwd_file,
				       content);

    if (buf) free(buf);

    return 0; 
}


static apr_status_t a_admin_delete_done(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;

    if (result->retval) {
	/* fail to give up the lease */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin delete", FAIL_STATUS, 0, "Failed to give up lease");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;	
    }

    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin delete", OK_STATUS, 0, "");
    gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0; 
}

static apr_status_t a_admin_delete_success(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

static apr_status_t a_admin_delete_failure(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

/* admin list */
static apr_status_t a_admin_list_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_event_t *evt;
    
    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    slot->u.admin.conn = conn;
    
    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    
    return 0;
}

static apr_status_t a_admin_get_password_file_direct(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    
    slot->u.admin.chubby.conn = chubby_makeConnection(gpma.chubby);
    chubby_connection_asynchronous_get(slot->u.admin.chubby.conn,
				       chubby_get_cb,
				       (void *) &slot->cmdidx,
				       gpma.chubby_passwd_file,
				       -1);
    return 0;
}

static apr_status_t a_admin_list_done(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj, *arrobj;
    gpcc_event_t *evt;
    char *tok, *last, *lastlast;
    char *user;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	/* fail to get password file */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin list", FAIL_STATUS, 0, "Failed to get password file");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 
    
    if (result->retval == CHUBBY_E_NOENT) {
	/* file not exists */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin list", OK_STATUS, 0, "");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    /* add new entry to the file */
    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin list", OK_STATUS, 0, "");
    arrobj = json_object_new_array();

    for (tok = apr_strtok(result->u.get.content.a, "\r\n", &last) ; 
	 tok ; 
	 tok = apr_strtok(0, "\r\n", &last)) {

	if (*tok == 0) 
	    continue;

	user = apr_strtok(tok, ":", &lastlast);
	user = gpcc_string_trim(user);

	json_object_array_add(arrobj, json_object_new_string(user));
	
    }

    json_object_object_add(obj, "rset", arrobj);
    gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));

    return 0; 
}

static apr_status_t a_admin_list_success(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

static apr_status_t a_admin_list_failure(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

/* admin password */
static apr_status_t a_admin_password_init(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri;
    gpcc_event_t *evt;
    struct json_object *obj;
    const char *user, *password, *new_password;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_param_alloc(url);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    password = gpcc_net_uri_param(uri, "old_password");
    new_password = gpcc_net_uri_param(uri, "new_password");
    
    
    if (! user || ! password || ! new_password) {
	goto bail;
    }

    slot->u.admin.conn = conn;
    slot->u.admin.username = apr_pstrdup(slot->pool, user);
    slot->u.admin.password = apr_pstrdup(slot->pool, password);
    slot->u.admin.new_password = apr_pstrdup(slot->pool, new_password);

    evt = gpcc_event_alloc(CONTINUE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    if (uri)
	gpcc_net_uri_free(uri);

    return 0;

    bail:
    
    if (uri)
	gpcc_net_uri_free(uri);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin password", FAIL_STATUS, 0, "missing parameter");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);

    return 0;
}

static apr_status_t a_admin_password_set_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    chubby_string_t *content = 0;
    char *buf, *tok, *last, *lastlast;
    char *user, *passwd;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	/* fail to get password file */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin passwd", FAIL_STATUS, 0, "Failed to get password file");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 
    
    if (result->retval == CHUBBY_E_NOENT) {
	/* file not exists */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin passwd", FAIL_STATUS, 0, "Error: no such user.");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    /* add new entry to the file */
    buf = malloc(sizeof(chubby_string_t) + result->u.get.content.length);
    content = (chubby_string_t *) buf;
    *content->a = 0;

    for (tok = apr_strtok(result->u.get.content.a, "\r\n", &last) ; 
	 tok ; 
	 tok = apr_strtok(0, "\r\n", &last)) {

	if (*tok == 0) 
	    continue;

	user = apr_strtok(tok, ":", &lastlast);
	passwd = apr_strtok(0, "\r\n", &lastlast);

	user = gpcc_string_trim(user);
	passwd = gpcc_string_trim(passwd);

	if (strcmp(user, slot->u.admin.username) != 0) {
	    strcat(content->a, user);
	    strcat(content->a, ":");
	    strcat(content->a, passwd);
	    strcat(content->a, "\n");
	} else {
	    if (apr_password_validate(slot->u.admin.password, passwd) != 0) {
		evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		obj = gpcc_json_http_reply(gpma.array_name, 0, "admin passwd", FAIL_STATUS, 0, "wrong password");
		gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
		json_object_put(obj);	
		if (buf) free(buf);
		return 0;
	    }

	    {
		char encoded[ADMIN_PASSWORD_ENCODED_LENGTH];
		apr_md5_encode(slot->u.admin.new_password, 
			       slot->u.admin.username, 
			       encoded,
			       sizeof(encoded));
		
		strcat(content->a, user);
		strcat(content->a, ":");
		strcat(content->a, encoded);
		strcat(content->a, "\n");
	    }
	}
	    
    }

    content->length = strlen(content->a) + 1;

    /* save the password file */
    chubby_connection_asynchronous_put(slot->u.admin.chubby.lease,
				       chubby_put_cb,
				       (void *) &slot->cmdidx,
				       gpma.chubby_passwd_file,
				       content);

    if (buf) free(buf);
    return 0; 
}


static apr_status_t a_admin_password_done(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;

    if (result->retval) {
	/* fail to give up the lease */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "admin passwd", FAIL_STATUS, 0, "Failed to give up lease");
	gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;	
    }

    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);    
    obj = gpcc_json_http_reply(gpma.array_name, 0, "admin passwd", OK_STATUS, 0, "");
    gpcc_net_http_close(slot->u.admin.conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0; 
}

static apr_status_t a_admin_password_success(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

static apr_status_t a_admin_password_failure(gpcc_event_t *ev, void *arg)
{
    return a_admin_cleanup(ev, arg);
}

