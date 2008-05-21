
static apr_status_t a_user_check_expire(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    apr_hash_index_t *hi;
    void *val;
    const void *key;
    apr_ssize_t  klen;
    apr_time_t now = apr_time_now();

    /* non-thread safe apr_hash_first is used but should be safe.  apr_pool_t is NULL */
    for (hi = apr_hash_first(0, slot->u.user.usertb); hi ;  hi = apr_hash_next(hi)) {
	user_t *user;

	apr_hash_this(hi, &key, &klen, &val);
	user = (user_t *) val;
	if (user->expire_time > now) {
	    /* expired */
	    apr_hash_set(slot->u.user.usertb, key, klen, NULL);
	    gpcc_log_info("user %s login expired", (const char *) key);
	    free(user);
	}
    }
    

    return 0;
}

static apr_status_t a_user_login(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *login_slot;
    gpcc_event_t *evt;

    login_slot = cmdslot_alloc("LOGIN", LOGIN_CMDSTATE);
    evt = gpcc_event_alloc(ev->etype, login_slot->cmdidx, ev->edatasz);
    memcpy(evt->pdata, ev->pdata, ev->edatasz);
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}

static apr_status_t a_user_logout(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri;
    const char *user, *host;
    char key[1024];
    struct json_object *obj;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_alloc(url);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    host = gpcc_net_uri_param(uri, "host");
    
    if (! user || ! host) {
	goto bail;
    }

    sprintf(key, "%s@%s", user, host);
    apr_hash_set(slot->u.user.usertb, key, APR_HASH_KEY_STRING, NULL);

    obj = gpcc_json_http_reply(gpma.array_name, 0, "logout", OK_STATUS, 0, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    
    return 0;

    bail:

    obj = gpcc_json_http_reply(gpma.array_name, 0, "logout", FAIL_STATUS, 0, "missing parameter");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;

}

static apr_status_t a_login_get_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri;
    struct json_object *obj;
    gpcc_event_t *evt;
    const char *user, * password, *host;
    char path[1024];

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    uri = gpcc_net_uri_alloc(url);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    password = gpcc_net_uri_param(uri, "password");
    host = gpcc_net_uri_param(uri, "host");
    
    if (! user || ! password || ! host) {
	goto bail;
    }

    slot->u.login.conn = conn;
    slot->u.login.username = apr_pstrdup(slot->pool, user);
    slot->u.login.password = apr_pstrdup(slot->pool, password);
    slot->u.login.host = apr_pstrdup(slot->pool, host);


    /* get password file from chubby */				       
    sprintf(path, "/gp/%s/%s/gpma/passwd", gpma.user, gpma.array_name);
    slot->u.login.chubby.conn = chubby_makeConnection(gpma.chubby);
    chubby_connection_asynchronous_get(slot->u.login.chubby.conn, 
				       chubby_get_cb, 
				       (void *) &slot->cmdidx, 
				       path, 
				       -1);

    if (uri) 
	gpcc_net_uri_free(uri);

    return 0;

    bail:


    if (uri) 
	gpcc_net_uri_free(uri);

    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    obj = gpcc_json_http_reply(gpma.array_name, 0, "login", FAIL_STATUS, 0, "parameter missing");
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

/* got the password file and logging onto gpcc */
static apr_status_t a_login_logging_on(gpcc_event_t *ev, void *arg) 
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;

    if (result->retval) {
	/* fail to get password file */
	evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
	obj = gpcc_json_http_reply(gpma.array_name, 0, "login", FAIL_STATUS, 0, "No password file found");
	gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
	return 0;
    } 

    /* compare the login user and password */
    
    evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);


    return 0;
}

static apr_status_t a_login_logged_on(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    user_t *user;
    int len;
    apr_md5_ctx_t context;
    struct json_object *obj;
    char sid_md5[APR_MD5_DIGESTSIZE+1];

    len = strlen(slot->u.login.username) + strlen(slot->u.login.host) + 2;
    user = (user_t *) malloc(sizeof(user_t) + len);
    user->datasz = sizeof(user_t) + len;
    user->login_time = apr_time_now();
    user->expire_time = apr_time_now() + 2 * 3600 * APR_USEC_PER_SEC;
    sprintf(user->name, "%s@%s", slot->u.login.username, slot->u.login.host);
    
    apr_md5_init(&context);
    apr_md5_update(&context, user->name, strlen(user->name));
    apr_md5_update(&context, &user->login_time, sizeof(user->login_time));
    apr_md5_final(user->sid, &context);

    apr_hash_set(gpma.user_cmdslot->u.user.usertb, user->name, APR_HASH_KEY_STRING, (const void *) user);
    
    memcpy(sid_md5, user->sid, APR_MD5_DIGESTSIZE);
    sid_md5[APR_MD5_DIGESTSIZE] = 0;
    obj = gpcc_json_http_reply(gpma.array_name, 0, "login", OK_STATUS, sid_md5, 0);
    gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));    

    return 0;
}

static apr_status_t a_login_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj;
    
    obj = gpcc_json_http_reply(gpma.array_name, 0, "login", OK_STATUS, 0, "login failed.");
    gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));    
    return 0;
}

static apr_status_t a_login_finish(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    if (slot->u.login.chubby.conn)
	chubby_connection_close(slot->u.login.chubby.conn);

    return 0;
}
