#ifndef NET_H
#define NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_PROTO		"gpcc/3.1"

#define GPCC_NET_HEADER_ENTRYMAX  20
typedef struct gpcc_net_header_t gpcc_net_header_t;
struct gpcc_net_header_t {
    int argc;
    char* argv[GPCC_NET_HEADER_ENTRYMAX];
    int hc;
    char* hname[GPCC_NET_HEADER_ENTRYMAX];
    char* hvalue[GPCC_NET_HEADER_ENTRYMAX];
};

typedef apr_uint32_t gpcc_net_conn_t;

typedef void gpcc_net_opened_t(gpcc_net_conn_t conn, 
			       gpcc_net_header_t* to_peer, 
			       gpcc_net_header_t* from_peer);

typedef void gpcc_net_accepted_t(gpcc_net_conn_t conn,
				 gpcc_net_header_t* from_peer);

typedef int gpcc_net_received_t(gpcc_net_conn_t conn, 
				const char* buf, int buflen);

typedef void gpcc_net_closed_t(gpcc_net_conn_t conn);

APR_DECLARE(apr_status_t) gpcc_net_init(int listen_port,
					gpcc_net_opened_t* opened,
					gpcc_net_accepted_t* accepted,
					gpcc_net_received_t* received,
					gpcc_net_closed_t* closed);
APR_DECLARE(apr_status_t) gpcc_net_once();

APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* conn,
					char* peer_addr,
					int argc, char** argv,
					int hc, char** hname, char** hvalue);


APR_DECLARE(apr_status_t) gpcc_net_close(gpcc_net_conn_t conn);

APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn,
					const char* buf, int buflen);


#endif /* NET_H */
