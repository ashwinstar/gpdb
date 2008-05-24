/* gp delete */
static apr_status_t a_fts_array_delete(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    char *url, *mode = "immediate";
    gpcc_net_conn_t conn;
    cmdslot_t *p;
    gpcc_event_t *evt;
    struct json_object *obj;
    gpcc_net_uri_t *uri;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);
    
    uri = gpcc_net_uri_alloc(url);

    p = cmdslot_alloc("ARRAY_DELETE", FTS_ARRAY_DELETE_CMDSTATE);
    p->u.array_delete.parent_cmdidx = cmdslot->cmdidx;
    p->u.array_delete.mode = apr_pstrdup(p->pool, mode);
    cmdslot->u.fts.active_cmdidx = p->cmdidx;
    
    evt = gpcc_event_alloc(INIT_EVENT, p->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    obj = gpcc_json_http_reply(config_get(gpma.cfg, CONFIG_DBNAME_KEY), 0,
			       "delete", OK_STATUS, ARRAY_DELETING_STATUS, 0);
    gpcc_net_http_close(conn, 200, "ok", json_object_to_json_string(obj));
    json_object_put(obj);
    if (uri)
	gpcc_net_uri_free(uri);

    return 0;
}

static apr_status_t a_fts_array_delete_reply(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_net_packet_t *pkt = (gpcc_net_packet_t *) ev->pdata;
    struct json_object *obj;
    gpcc_event_t *evt;
    char *status;

    obj = json_tokener_parse(pkt->payload);

    status = gpcc_json_object_get_string(obj, "status", FAIL_STATUS);
    if (strcmp(status, OK_STATUS) == 0) {
	evt = gpcc_event_alloc(SUCCESS_EVENT, cmdslot->cmdidx, 0);
    } else {
	evt = gpcc_event_alloc(FAILURE_EVENT, cmdslot->cmdidx, 0);	
    }
    gpcc_evqueue_put(gpma.evq, evt);

    return 0;
}


static apr_status_t a_fts_deleted(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    const char **keys;
    int i;
    gpcc_event_t *evt;

    cmdslot->u.fts.active_cmdidx = 0;

    keys = config_keys(gpma.cfg, cmdslot->pool);
    for (i = 0 ; keys[i] ; i++) {
	if (strstr(keys[i], "seg.") == keys[i] || 
	    strstr(keys[i], "node.") == keys[i] ||
	    strcmp(keys[i], CONFIG_DBNAME_KEY) == 0) {
	    config_set(gpma.cfg, keys[i], 0);
	}
    }

    evt = gpcc_event_alloc(CFG_UPDATED_EVENT, gpma.cfg_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    setup_cleanup_seginfo(ev, _cmdslot);
    apr_pool_clear(cmdslot->pool);
    return 0;
}

static apr_status_t a_fts_array_delete_shutdown_nodes(gpcc_event_t *ev, void *_cmdslot)
{
    cmdslot_t *cmdslot = _cmdslot;
    gpcc_event_t *evt;
    int i;

    /* shutdown nodes */
    for (i = 0 ; i < gpma.node_max ; i++) {
	evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.node_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    /* shutdown unit */
    for (i = 0 ; i < gpma.unit_max ; i++) {
	evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.unit_cmdtab[i]->cmdidx, 0);
	gpcc_evqueue_put(gpma.evq, evt);
    }

    /* shutdown haset */
    evt = gpcc_event_alloc(SHUTDOWN_EVENT, gpma.haset_cmdslot->cmdidx, 0);
    gpcc_evqueue_put(gpma.evq, evt);

    gpma.node_max = 0;
    gpma.haset_cmdslot = 0;
    gpma.node_cmdtab = 0;
    gpma.unit_max = 0;
    gpma.unit_cmdtab = 0;
    cmdslot->u.fts.nnode = 0;
    cmdslot->u.fts.nopened = 0;

    return 0;
}
