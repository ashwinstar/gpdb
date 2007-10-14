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


static apr_status_t http_response(gpcc_net_conn_t conn, int status, const char* msg, const char* json)
{
    gpcc_net_header_t hdr;
    char statusbuf[10];
    int len = strlen(json);
    char lenbuf[10];
    int hc;
    int e;

    sprintf(statusbuf, "%d", status);
    sprintf(lenbuf, "%d", len);

    hdr.ac = 3;
    hdr.argv[0] = statusbuf;
    hdr.argv[1] = msg;
    hdr.argv[2] = "HTTP/1.0";
    hc = 0;
    hdr.hname[hc] = "Content-type";
    hdr.hvalue[hc++] = "text/plain";
    hdr.hname[hc] = "Content-length";
    hdr.hvalue[hc++] = lenbuf;
    hdr.hc = hc;

    if (0 != (e = gpcc_net_send_header(conn, &hdr)))
	return e;

    return gpcc_net_send(conn, json, len);
}

