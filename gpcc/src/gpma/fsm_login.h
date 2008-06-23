
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
	if (user->expire_time < now) {
	    /* expired */
	    apr_hash_set(slot->u.user.usertb, key, klen, NULL);
	    gpcc_log_info("user %s login expired", (const char *) key);
	    free(user);
	}
    }
    
    slot->countdown  = 30;
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
    char *url, *p;
    gpcc_net_conn_t conn;
    const char *key;
    int len;
    struct json_object *obj;
    user_t *userinfo;

    p = ev->pdata;
    memcpy(&conn, p, sizeof(gpcc_net_conn_t));
    p += sizeof(gpcc_net_conn_t);
    memcpy(&len, p, sizeof(int));
    p += sizeof(int);
    url = p;
    p += len;
    memcpy(&len, p, sizeof(int));
    p += sizeof(int);
    key = p;

    gpcc_log_info("logout: sessionid = '%s'", key);

    userinfo = (user_t *) apr_hash_get(slot->u.user.usertb, key, APR_HASH_KEY_STRING);
    if (userinfo) {
	gpcc_log_info("remove the session with sessionid %s", key);
	apr_hash_set(slot->u.user.usertb, key, APR_HASH_KEY_STRING, NULL);
	free(userinfo);
	userinfo = 0;
    } else {
	gpcc_log_info("session %s is not there.", key);
    }


    obj = gpcc_json_http_reply(gpma.array_name, 0, "logout", OK_STATUS, 0, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    
    return 0;
}

static apr_status_t a_login_get_password_file(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char *url, *p;
    gpcc_net_conn_t conn;
    gpcc_net_uri_t *uri = 0;
    struct json_object *obj;
    gpcc_event_t *evt;
    const char *user, * password, *host;
    int datalen, len;
    char data[1024];

    p = ev->pdata;
    memcpy(&conn, p, sizeof(gpcc_net_conn_t));
    p += sizeof(gpcc_net_conn_t);
    memcpy(&datalen, p, sizeof(int));
    p += sizeof(int);
    url = p;
    p += datalen;
    memcpy(&len, p, sizeof(int));
    p += sizeof(int);
    host = p;
    
    if (datalen >= sizeof(data)) {
	gpcc_log_error("data length exceed limit %d >= %d", datalen, sizeof(data));
	goto bail;
    }

    memcpy(data, url, datalen);
    data[datalen] = 0;
    gpcc_log_info("host = %s", host);
    gpcc_log_info("data = %s", data);

    uri = gpcc_net_uri_param_alloc(data);
    if (!uri) {
	goto bail;
    }

    user = gpcc_net_uri_param(uri, "user");
    password = gpcc_net_uri_param(uri, "password");
    
    if (! user || ! password) {
	goto bail;
    }

    gpcc_log_info("gp login:  user = '%s', host = '%s', password = '%s'", user, host, password);

    slot->u.login.conn = conn;
    slot->u.login.username = apr_pstrdup(slot->pool, user);
    slot->u.login.password = apr_pstrdup(slot->pool, password);
    slot->u.login.host = apr_pstrdup(slot->pool, host);


    /* get password file from chubby */				       
    slot->u.login.chubby.conn = chubby_makeConnection(gpma.chubby);
    chubby_connection_asynchronous_get(slot->u.login.chubby.conn, 
				       chubby_get_cb, 
				       (void *) &slot->cmdidx, 
				       gpma.chubby_passwd_file, 
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
    char *user, *encoded_passwd, *tok, *last, *lastlast;

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
    for (tok = apr_strtok(result->u.get.content.a,  "\r\n", &last) ; 
	 tok ; 
	 tok = apr_strtok(0, "\r\n", &last)) {

	tok = gpcc_string_trim(tok);
	if (*tok == 0)
	    continue;
	user = apr_strtok(tok, ":", &lastlast);
	encoded_passwd = apr_strtok(0, "\r\n", &lastlast);
	user = gpcc_string_trim(user);
	encoded_passwd = gpcc_string_trim(encoded_passwd);
	if (strcmp(user, slot->u.login.username) == 0) {
	    if (apr_password_validate(slot->u.login.password, encoded_passwd) == 0) {
		/* username and password correct */
		gpcc_log_info("correct password for user %s", slot->u.login.username);
		evt = gpcc_event_alloc(SUCCESS_EVENT, slot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		return 0;
	    } else {
		/* wrong password */
		gpcc_log_info("wrong password for user %s", slot->u.login.username);
		evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
		gpcc_evqueue_put(gpma.evq, evt);
		obj = gpcc_json_http_reply(gpma.array_name, 0, "login", FAIL_STATUS, 0, "wrong password");
		gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));
		json_object_put(obj);		
		return 0;
	    }
	}
    }

    
    evt = gpcc_event_alloc(FAILURE_EVENT, slot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);
    obj = gpcc_json_http_reply(gpma.array_name, 0, "login", FAIL_STATUS, 0, "No such user found");
    gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_login_logged_on(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    user_t *user;
    int len;
    apr_md5_ctx_t context;
    struct json_object *obj;
    unsigned char sid[APR_MD5_DIGESTSIZE];
    int *sid_int;
    char sid_str[36];
    int randint;

    len = strlen(slot->u.login.username) + strlen(slot->u.login.host) + strlen(gpma.array_name) + sizeof(sid_str) + 4;
    user = (user_t *) malloc(sizeof(user_t) + len);
    user->datasz = sizeof(user_t) + len;
    user->login_time = apr_time_now();
    user->expire_time = apr_time_now() + 2 * 3600 * APR_USEC_PER_SEC;

    randint = random();   
    apr_md5_init(&context);
    apr_md5_update(&context, slot->u.login.username, strlen(slot->u.login.username));
    apr_md5_update(&context, &randint, sizeof(int));
    apr_md5_update(&context, &user->login_time, sizeof(user->login_time));
    apr_md5_final(sid, &context);

    sid_int = (int *) sid;
    sprintf(sid_str, "%08x-%08x-%08x-%08x", sid_int[0], sid_int[1], sid_int[2], sid_int[3]);

    strcpy(user->key, slot->u.login.username);
    strcat(user->key, "@");
    strcat(user->key, slot->u.login.host);
    strcat(user->key, ":");
    strcat(user->key, gpma.array_name);
    strcat(user->key, ":");
    strcat(user->key, sid_str);

    gpcc_log_info("new login session with id = '%s'", user->key);
    apr_hash_set(gpma.user_cmdslot->u.user.usertb, user->key, APR_HASH_KEY_STRING, (const void *) user);

    obj = gpcc_json_http_reply(gpma.array_name, 0, "login", OK_STATUS, user->key, 0);
    gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));    

    return 0;
}

static apr_status_t a_login_failure(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    struct json_object *obj;

    if (slot->u.login.conn) {
	obj = gpcc_json_http_reply(gpma.array_name, 0, "login", OK_STATUS, 0, "login failed.");
	gpcc_net_http_close(slot->u.login.conn, 200, "ok", json_object_to_json_string(obj));    
    }

    return 0;
}

static apr_status_t a_login_finish(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;

    if (slot->u.login.chubby.conn)
	chubby_connection_close(slot->u.login.chubby.conn);

    return 0;
}
