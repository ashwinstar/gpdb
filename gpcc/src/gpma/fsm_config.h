
static int keycmp(const void* a, const void* b)
{
    const char** aa = (const char**) a;
    const char** bb = (const char**) b;
    return strcmp(*aa, *bb);
}


static char *config_to_json(apr_pool_t *pool) 
{
    const char** keys;
    int count, i;
    char* buf;
    int buflen;
    char* p;
    char* q;
    const char *prev_value;

    keys = config_keys(gpma.cfg, pool);
    for (count = 0; keys[count]; count++);
    qsort(keys, count, sizeof(*keys), keycmp);

    buflen = 16 * 1024;
    buf = apr_palloc(pool, buflen);
    p = buf;
    q = buf + buflen;
    
    apr_snprintf(p, q - p, "{ \"status\": \"ok\", \"action\": \"lscfg\", \"a\": \"%s\", \"rset\": [", config_get(gpma.cfg, CONFIG_DBNAME_KEY));
    p += strlen(p);
    
    prev_value = 0;
    for (i = 0; i < count; i++) {
	const char* value = config_get(gpma.cfg, keys[i]);
	if (value) {
	    apr_snprintf(p, q - p, "%s [ \"%s\", \"%s\" ]", 
			 prev_value ? "," : "",
			 keys[i], value);
	    p += strlen(p);
	}
	prev_value = value;
    }
    apr_snprintf(p, q - p, " ] }");
    p += strlen(p);
    
    if (p >= q) 
	return 0;
    
    return buf;
}

/* config is updated in memory and we should save it on disk */
static apr_status_t a_config_updated(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    const char *master;
    char buf[1024];
    char path[1024];
    chubby_string_t *s = (chubby_string_t *) buf;
    int e;

    gpcc_log_info("a_config_updated...");
    if (config_get(gpma.cfg, CONFIG_DBNAME_KEY)) {
	/* put master file */
	master = config_get(gpma.cfg, "seg.m.0");

	gpcc_log_info("config: put file %s to chubby. content = '%s'", path, master);
	s->length = strlen(master)+1;
	strcpy(s->a, master);
	e = chubby_connection_asynchronous_put(gpma.lma_cmdslot->u.lma.chubby.lease,
					       chubby_put_cb,
					       (void *) &slot->cmdidx,
					       gpma.chubby_master_file,
					       s);
    } else {
	/* delete master file */
	gpcc_log_info("config: remove file %s from chubby", path);
	e = chubby_connection_asynchronous_remove(gpma.lma_cmdslot->u.lma.chubby.lease,
						  chubby_remove_cb,
						  (void *) &slot->cmdidx,
						  gpma.chubby_master_file);
    }

    config_gc(gpma.cfg);

    return 0;
}

static apr_status_t a_config_put_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;

    if (result->retval) {
	slot->countdown = 5;
	gpcc_log_error("config: put file failed. retry in 5 seconds");
	return 0;
    }
    
    gpcc_log_info("config: put file successfully.");
    return 0;
}

static apr_status_t a_config_remove_reply(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    chubby_result_t *result = (chubby_result_t *) ev->pdata;

    if (result->retval && result->retval != CHUBBY_E_NOENT) {
	gpcc_log_error("config: remove file failed. retry in 5 seconds");
	slot->countdown = 5;
	return 0;
    }

    gpcc_log_info("config: file removed successfully.");
    return 0;
}

static apr_status_t a_config_list(gpcc_event_t *ev, void *arg)
{
    cmdslot_t *slot = arg;
    char* buf;
    char *url;
    gpcc_net_conn_t conn;

    memcpy(&conn, ev->pdata, sizeof(gpcc_net_conn_t));
    url = ev->pdata + sizeof(gpcc_net_conn_t);

    buf = config_to_json(slot->pool);
    if (buf)
	gpcc_net_http_close(conn, 200, "OK", buf);
    else 
	gpcc_net_http_close(conn, 500, "Internal error", "");

    apr_pool_clear(slot->pool);

    return 0;
}
