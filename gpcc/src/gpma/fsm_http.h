static apr_status_t http_request(gpcc_event_t *ev, void *cmdslot_)
{
    cmdslot_t *p = cmdslot_;
    conninfo_t* conninfo = p->conninfo;
    int i;

    gpcc_log_info("http request...");
    for (i = 0 ; i < conninfo->peer.request->argc ; i++){
	gpcc_log_info("%s ", conninfo->peer.request->argv[i]);
    }
    for (i = 0 ; i < conninfo->peer.request->hc ; i++){
	gpcc_log_info("%s : %s\n", conninfo->peer.request->hname[i], conninfo->peer.request->hvalue[i]);
    }
	
    gpcc_log_info("data len = %d", conninfo->peer.request->datalen);
    for (i = 0 ; i < conninfo->peer.request->datalen ; i++) {
	gpcc_log_info("data[%d] = %c", i, conninfo->peer.request->data[i]);
    }

    return 0;
}
