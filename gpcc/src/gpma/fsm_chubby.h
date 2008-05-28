
void chubby_get_cb(void *data, int e, struct chubby_stat *stat, chubby_string_t *answer)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;

    gpcc_log_info("chubby_get: ret=%d", e);
    if (e) {
	evt = gpcc_event_alloc(CHUBBY_GET_REPLY_EVENT, cmdidx, sizeof(*result));	
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;	
    } else {
	gpcc_log_info("chubby_get: answer = %s", answer);
	evt = gpcc_event_alloc(CHUBBY_GET_REPLY_EVENT, cmdidx, sizeof(*result) + answer->length);
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;
	result->u.get.content.length = answer->length;
	memcpy(result->u.get.content.a, answer->a, answer->length);
	memcpy(&result->u.get.stat, stat, sizeof(*stat));
    }
    gpcc_evqueue_put(gpma.evq, evt);

}

void chubby_put_cb(void *data, int e)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;

    gpcc_log_info("chubby_put: ret=%d", e);
    evt = gpcc_event_alloc(CHUBBY_PUT_REPLY_EVENT, cmdidx, sizeof(*result));
    result = (chubby_result_t *) evt->pdata;
    result->retval = e;
    gpcc_evqueue_put(gpma.evq, evt);

}

void chubby_renew_cb(void *data, int e)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;
    gpcc_log_info("chubby_renew: ret=%d", e);
    evt = gpcc_event_alloc(CHUBBY_RENEW_REPLY_EVENT, cmdidx, sizeof(*result));
    result = (chubby_result_t *) evt->pdata;
    result->retval = e;
    gpcc_evqueue_put(gpma.evq, evt);

}    

void chubby_giveup_lease_cb(void *data, int e)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;

    evt = gpcc_event_alloc(CHUBBY_GIVEUP_LEASE_REPLY_EVENT, cmdidx, sizeof(*result));
    result = (chubby_result_t *) evt->pdata;
    result->retval = e;
    gpcc_evqueue_put(gpma.evq, evt);

}


void chubby_stat_cb(void *data, int e, struct chubby_stat *stat)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;

    if (e) {
	evt = gpcc_event_alloc(CHUBBY_STAT_REPLY_EVENT, cmdidx, sizeof(*result));	
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;	
    } else {
	evt = gpcc_event_alloc(CHUBBY_STAT_REPLY_EVENT, cmdidx, sizeof(*result));
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;
	memcpy(&result->u.stat.stat, stat, sizeof(*stat));
    }
    gpcc_evqueue_put(gpma.evq, evt);

}

void chubby_get_lease_cb(void *data, int e, chubby_connection_t *conn)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;

    gpcc_log_info("chubby_get_lease: ret=%d", e);
    if (e) {
	evt = gpcc_event_alloc(CHUBBY_GET_LEASE_REPLY_EVENT, cmdidx, sizeof(*result));	
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;	
    } else {    
	evt = gpcc_event_alloc(CHUBBY_GET_LEASE_REPLY_EVENT, cmdidx, sizeof(*result));
	result = (chubby_result_t *) evt->pdata;
	result->retval = e;
	result->u.lease.conn = conn;
    }
    gpcc_evqueue_put(gpma.evq, evt);

}

void chubby_mkdir_cb(void *data, int e)
{
    apr_uint32_t cmdidx = *((apr_uint32_t *) data);
    chubby_result_t *result;
    gpcc_event_t *evt;
    gpcc_log_info("chubby_mkdir: ret=%d", e);
    evt = gpcc_event_alloc(CHUBBY_MKDIR_REPLY_EVENT, cmdidx, sizeof(*result));
    result = (chubby_result_t *) evt->pdata;
    result->retval = e;
    gpcc_evqueue_put(gpma.evq, evt);

}    
