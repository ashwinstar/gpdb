#include <stdio.h>
#include <stdlib.h>
#include <gpcc_ha.h>

#define GET_IDENTITY_MSG 1

struct gpcc_ha_t {
    int index;
    apr_pool_t *pool;
    gpcc_net_conn_t conn;
    gpcc_ha_opened_t *opened;
    gpcc_ha_closed_t *closed;
    gpcc_ha_recv_t *recv;

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
    ha->self.request.argv[0] = GPCC_NET_PROTO;
    return gpcc_net_send_request(ha->conn, &ha->self.request);
}

void *ha_net_opened(gpcc_net_conn_t conn, void *conn_arg,
		    int self_origin,
		    apr_pool_t *pool)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;
    int e;

    ha->pool = pool;
    /* send proto */
    if (0 != (e = send_gpcc_net_proto(ha))) {
	gpcc_warning(FLINE, "send gpcc net protocol failed");
	gpcc_ha_close(ha);
	return 0;
    }

    ha->opened(ha);
    return ha;
}

void ha_net_closed(gpcc_net_conn_t conn, void *conn_arg)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;

    ha->closed(ha);
}

void ha_net_recv(gpcc_net_conn_t conn, void *conn_arg)
{
    gpcc_ha_t *ha = (gpcc_ha_t *) conn_arg;
    int e;

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

	e = gpcc_net_send_packet(ha->conn, GET_IDENTITY_MSG,
				 ha->index, 0, 0, 0);
	if (e) {
	    gpcc_ha_close(ha);
	    return;
	}
    } else {
	gpcc_net_packet_t *packet = 0;
	/* get packet */
	e = gpcc_net_read_packet(ha->conn, &packet);
	if (e) {
	    goto bail;
	}

	ha->recv(ha, packet);

	if (packet)
	    free(packet);
    }

    bail:

    if (e == APR_EAGAIN)
	return;

    gpcc_ha_close(ha);
}

APR_DECLARE(gpcc_ha_t*) gpcc_ha_open(const char *addr,
				     gpcc_ha_opened_t *opened,
				     gpcc_ha_closed_t *closed,
				     gpcc_ha_recv_t *recv) 
{
			       
    int e;
    gpcc_ha_t *ha;

    ha = malloc(sizeof(gpcc_ha_t));
    if (!ha) {
	gpcc_warning(FLINE, "not enough memory");
	return 0;
    }
    memset(ha, 0, sizeof(*ha));

    if (0 != (e = gpcc_net_open(&ha->conn, addr, ha_net_opened, ha_net_closed, ha_net_recv, ha))) {
	gpcc_warning(FLINE, "gpcc_net_open failed");
	free(ha);
	return 0;
    }

    return ha;
}
										 
    
APR_DECLARE(apr_status_t) gpcc_ha_close(gpcc_ha_t *ha) {
    int e = 0;

    if (ha) {
	e = gpcc_net_close(ha->conn);
	free(ha);
    }
    return e;
}

APR_DECLARE(apr_status_t) gpcc_ha_send(gpcc_ha_t *ha,
				       int type,
				       int sndidx, int rcvidx,
				       const char *buf, int len)
{
    
    return gpcc_net_send_packet(ha->conn,
				type,
				sndidx, rcvidx,
				buf, len);
    
}
