#include <stdio.h>
#include <stdlib.h>
#include "enum.h"
#include <gpcc_ha.h>

struct gpcc_ha_t {
    int sndidx;
    int rcvidx;
    apr_pool_t *pool;
    gpcc_net_conn_t conn;
    void *arg;
    
    struct {
	gpcc_ha_opened_t *opened;
	gpcc_ha_closed_t *closed;
	gpcc_ha_recv_t *recv;
    } cb;

    struct {
	gpcc_net_request_t *request;
    } peer;

    struct {
	gpcc_net_request_t request;
    } self;

};


static apr_status_t send_gpcc_net_proto(gpcc_ha_t *ha)
{
    if (ha->self.request.argc)
	return APR_EINVAL;

    ha->self.request.argc = 1;
    ha->self.request.argv = apr_pcalloc(ha->pool, sizeof(char *) * (ha->self.request.argc + 1));
    ha->self.request.argv[0] = GPCC_NET_PROTO;
    return gpcc_net_send_request(ha->conn, &ha->self.request);
}

void *ha_net_opened(gpcc_net_conn_t conn, void *conn_arg,
		    int self_origin,
		    apr_pool_t *pool)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;
    int e;

    gpcc_log_info("ha_net_opened...");
    ha->pool = pool;
    /* send proto */
    
    if (0 != (e = send_gpcc_net_proto(ha))) {
	gpcc_warning(FLINE, "send gpcc net protocol failed");
	gpcc_ha_close(ha);
	return 0;
    }
    
    return ha;
}

void ha_net_closed(gpcc_net_conn_t conn, void *conn_arg)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;

    gpcc_log_info("ha_net_closed...");
    if (ha) {
	ha->cb.closed(ha, ha->arg);
	free(ha);
    }
}

void ha_net_recv(gpcc_net_conn_t conn, void *conn_arg)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;
    int e;
    gpcc_net_packet_t *packet = 0;

    gpcc_log_info("ha_net_recv...");
    if (!ha->peer.request) {
	/* receive proto msg */
	e = gpcc_net_read_request(ha->conn, &ha->peer.request, ha->pool);
	if (e) {
	    goto bail;
	}
	if (ha->peer.request->argc == 0 ||
	    0 != strcmp(ha->peer.request->argv[0], GPCC_NET_PROTO)) {
	    gpcc_ha_close(ha);
	    return;
	}
	gpcc_log_info("receive request and send GET_IDENTITY_MSG");
	/*
	e = gpcc_net_send_packet(ha->conn, GET_IDENTITY_MSG,
				 ha->sndidx, 0, 0, 0);
	if (e) {
	    gpcc_ha_close(ha);
	    return;
	}
	*/
    } 

    /* get packet */
    for (;;) {
	e = gpcc_net_read_packet(ha->conn, &packet);
	if (e) {
	    goto bail;
	}
	
	gpcc_log_info("receive packet...");
	if (ha->rcvidx == 0) {
	    /* if IDENTITY_MSG, update rcvidx and sndidx else invalid message*/
	    if (packet->mh.type == IDENTITY_MSG) {
		/* session ready */
		gpcc_log_info("recv IDENTITY_MSG snd= %d, rcv=%d", packet->mh.sndidx, packet->mh.rcvidx);
		ha->rcvidx = packet->mh.sndidx;
		ha->arg = ha->cb.opened(ha, ha->arg);
	    } else if (packet->mh.type == GET_IDENTITY_MSG) {
		gpcc_log_info("recv GET_IDENTITY_MSG snd= %d, rcv=%d", packet->mh.sndidx, packet->mh.rcvidx);
		gpcc_net_send_packet(ha->conn, IDENTITY_MSG,
				     ha->sndidx, packet->mh.sndidx, 0, 0);
	    } else {
		gpcc_log_info("invalid packet %s %d %d", event_str(packet->mh.type), packet->mh.sndidx, packet->mh.rcvidx);
		if (packet) {
		    free(packet);
		    packet = 0;
		}
		return;
	    }
	    
	} else {
	    if (packet->mh.type == GET_IDENTITY_MSG) {
		gpcc_log_info("recv GET_IDENTITY_MSG snd= %d, rcv=%d", packet->mh.sndidx, packet->mh.rcvidx);
		ha->rcvidx = packet->mh.sndidx;
		gpcc_net_send_packet(ha->conn, IDENTITY_MSG,
				     ha->sndidx, ha->rcvidx, 0, 0);
	    } else {
		ha->rcvidx = packet->mh.sndidx;
		ha->cb.recv(ha, packet, ha->arg);
	    }
	}
	
	if (packet) {
	    free(packet);
	    packet = 0;
	}
    }

    return;
    
    bail:
    
    if (e == APR_EAGAIN)
	return;
    
    gpcc_ha_close(ha);
}

APR_DECLARE(gpcc_ha_t*) gpcc_ha_open(int cmdidx,
				     const char *addr,
				     gpcc_ha_opened_t *opened,
				     gpcc_ha_closed_t *closed,
				     gpcc_ha_recv_t *recv,
				     void *event_base,
				     void *arg)
{
			       
    int e;
    gpcc_ha_t *ha;

    ha = malloc(sizeof(gpcc_ha_t));
    if (!ha) {
	gpcc_warning(FLINE, "not enough memory");
	return 0;
    }
    memset(ha, 0, sizeof(*ha));

    if (0 != (e = gpcc_net_open(&ha->conn, addr, 
				ha_net_opened, ha_net_closed, ha_net_recv, event_base, ha))) {
	gpcc_warning(FLINE, "gpcc_net_open failed");
	free(ha);
	return 0;
    }

    ha->cb.opened = opened;
    ha->cb.closed = closed;
    ha->cb.recv = recv;
    ha->arg = arg;
    ha->sndidx = cmdidx;
    return ha;
}
										 
    
APR_DECLARE(apr_status_t) gpcc_ha_close(gpcc_ha_t *ha) {
    int e = 0;

    if (ha) {
	if (ha->conn) {
	    e = gpcc_net_close(ha->conn);
	}
    }
    return e;
}

APR_DECLARE(apr_status_t) gpcc_ha_send(gpcc_ha_t *ha,
				       int type,
				       const char *buf, int len)
{
    
    return gpcc_net_send_packet(ha->conn,
				type,
				ha->sndidx, ha->rcvidx,
				buf, len);
    
}
