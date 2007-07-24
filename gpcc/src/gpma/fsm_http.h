static apr_status_t http_request(gpcc_event_t *ev, void *cmdslot_)
{
    cmdslot_t *p = cmdslot_;
    conninfo_t* conninfo = p->conninfo;
    int i;
    gpcc_http_request_t *req = (gpcc_http_request_t *) conninfo->peer.request;

    gpcc_log_info("http request...");
    gpcc_log_info("METHOD=%s", gpcc_http_request_get_method(req));
    gpcc_log_info("HEADER");
    for (i = 0 ; i < conninfo->peer.request->hc ; i++){
	gpcc_log_info("%s : %s\n", conninfo->peer.request->hname[i], conninfo->peer.request->hvalue[i]);
    }
    gpcc_log_info("URL=%s", gpcc_http_request_get_context_path(req));
    for (i = 0 ; i < req->pc ; i++) {
	gpcc_log_info("PARAMETER %s = %s", req->pname[i], req->pvalue[i]);
    }
	
    gpcc_log_info("data len = %d", conninfo->peer.request->datalen);
    for (i = 0 ; i < conninfo->peer.request->datalen ; i++) {
	gpcc_log_info("data[%d] = %c", i, conninfo->peer.request->data[i]);
    }

    return 0;
}
