
static apr_status_t a_fts_waiting(gpcc_event_t *ev, void *_cmdslot)
{
    char *url, *cmd = "";
    gpcc_net_conn_t conn;
    struct json_object *obj;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    switch (ev->etype)	{ 
    case ARRAY_INIT_EVENT:
	url = "/gpcc/init";
	cmd = "init";
	break;
    case ARRAY_STOP_EVENT:
	cmd = "stop";
	break;
    case ARRAY_START_EVENT:
	cmd = "start";
	break;
    case ARRAY_DELETE_EVENT:
	cmd = "delete";
	break;
    default:
	break;
    }
	
    if (IS_NO_LEADMA) {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   cmd, FAIL_STATUS, 0, "Lead MA is not determined");
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    } else if (!IS_LEADMA) {
	a_fts_http_redirect_leadma(conn, url);
    } else {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
				   cmd, FAIL_STATUS, 0, SYSTEM_INITIALIZING_STATUS);
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    }
    return 0;
}

static apr_status_t a_fts_array_units_status(gpcc_event_t *ev, void *_cmdslot, struct json_object *obj)
{
    char *status;
    struct json_object *new_obj, *array;
    int i;

    if (!gpma.unit_cmdtab)
	return 0;

    new_obj = json_object_new_object();
    for (i = 0 ; i < gpma.unit_max ; i++) {
	cmdslot_t *slot = gpma.unit_cmdtab[i];

	switch (slot->cmdstate) {
	case UNIT_CMDSTATE:
	    status= UNIT_SETUP_STATUS;
	    break;
	case UNIT_STOPPING_CMDSTATE:
	    status = UNIT_STOPPING_STATUS;
	    break;
	case UNIT_STOPPED_CMDSTATE:
	    status = UNIT_STOPPED_STATUS;
	    break;
	case UNIT_STARTING_CMDSTATE:
	    status = UNIT_STARTING_STATUS;
	    break;
	case UNIT_RUNNING_CMDSTATE:
	    status = UNIT_RUNNING_STATUS;
	    break;
	default:
	    status = UNIT_INVALID_STATUS;
	    break;
	}
	
	json_object_object_add(new_obj, slot->cmdname, json_object_new_string(status));
    }

    array = json_object_object_get(obj, "rset");
    json_object_array_add(array, new_obj);

    return 0;
}


static apr_status_t a_fts_array_status_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *url;
    char *state;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    if (IS_NO_LEADMA) {
	obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
				   "status", OK_STATUS, SYSTEM_INITIALIZING_STATUS, 0);
	gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
	json_object_put(obj);
    } else if (!IS_LEADMA) {
	a_fts_http_redirect_leadma(conn, url);
	return 0;
    }

    /* send the current state */
    switch (cmdslot->cmdstate) {
    case FTS_CMDSTATE:
    case FTS_WAIT_CMDSTATE:
	state = SYSTEM_INITIALIZING_STATUS;
	break;
    case FTS_EMPTY_CMDSTATE:
	state = ARRAY_NOT_INITIALIZED_STATUS;
	break;
    case FTS_INITIALIZING_CMDSTATE:
    case FTS_INITIALIZING0_CMDSTATE:
	state = ARRAY_INITIALIZING_STATUS;
	break;
    case FTS_STOPPED_CMDSTATE:
	state = ARRAY_STOPPED_STATUS;
	break;
    case FTS_STARTING_CMDSTATE:
	state = ARRAY_STARTING_STATUS;
	break;
    case FTS_RUNNING_CMDSTATE:
	state = ARRAY_RUNNING_STATUS;
	break;
    case FTS_STOPPING_CMDSTATE:
	state = ARRAY_STOPPING_STATUS;
	break;
    case FTS_DELETING_CMDSTATE:
	state = ARRAY_DELETING_STATUS;
	break;
    default:
	state = ARRAY_INVALID_STATUS;
	break;
    }

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
			       "status", OK_STATUS, state, 0);

    a_fts_array_units_status(ev, _cmdslot, obj);

    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_busy_ok_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *msg;

    if (cmdslot->cmdstate ==  FTS_CMDSTATE) {
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {	
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_EMPTY_CMDSTATE) {	
	msg = ARRAY_NOT_INITIALIZED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_INITIALIZING_CMDSTATE) {	
	msg = ARRAY_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = ARRAY_STOPPED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = ARRAY_STARTING_STATUS;
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = ARRAY_RUNNING_STATUS;
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = ARRAY_STOPPING_STATUS;
    } else if (cmdslot->cmdstate == FTS_DELETING_CMDSTATE) {
	msg = ARRAY_DELETING_STATUS;
    } else {
	msg = ARRAY_INVALID_STATUS;
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "", OK_STATUS, msg, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_busy_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url;
    gpcc_net_conn_t conn;
    struct json_object *obj = 0;
    char *msg;

    if (cmdslot->cmdstate ==  FTS_CMDSTATE) {
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_WAIT_CMDSTATE) {	
	msg = SYSTEM_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_EMPTY_CMDSTATE) {	
	msg = ARRAY_NOT_INITIALIZED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_INITIALIZING_CMDSTATE) {	
	msg = ARRAY_INITIALIZING_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STOPPED_CMDSTATE) {
	msg = ARRAY_STOPPED_STATUS;
    } else if (cmdslot->cmdstate ==  FTS_STARTING_CMDSTATE) {
	msg = ARRAY_STARTING_STATUS;
    } else if (cmdslot->cmdstate == FTS_RUNNING_CMDSTATE) {
	msg = ARRAY_RUNNING_STATUS;
    } else if (cmdslot->cmdstate == FTS_STOPPING_CMDSTATE) {
	msg = ARRAY_STOPPING_STATUS;
    } else if (cmdslot->cmdstate == FTS_DELETING_CMDSTATE) {
	msg = ARRAY_DELETING_STATUS;
    } else {
	msg = ARRAY_INVALID_STATUS;
    }

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0, 
			       "", FAIL_STATUS, 0, msg);
    gpcc_net_http_close(conn, 200, "failure", json_object_to_json_string(obj));
    json_object_put(obj);
    return 0;
}

static apr_status_t a_fts_running(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;

    return 0;
}


static apr_status_t a_fts_failure(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    cmdslot->u.fts.active_cmdidx = 0;
    return 0;
}
