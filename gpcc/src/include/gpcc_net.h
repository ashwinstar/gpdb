#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

/** identifies this node - a random unique value */
typedef struct gpcc_net_node_id_t gpcc_net_node_id_t;
struct gpcc_net_node_id_t {
    unsigned char opaque[20];
};

/** identifies a connection */
typedef apr_uint32_t gpcc_net_conn_t;

/** callback when a connection is opened */
typedef void gpcc_net_opened_t(gpcc_net_conn_t conn);

/** callback when a connection is closed */
typedef void gpcc_net_closed_t(gpcc_net_conn_t conn);

/** callback when a packet arrives */
typedef void gpcc_net_recv_t(gpcc_net_conn_t conn, 
			     int pkttype, int reqid, const char* pkt, int pktlen);

/** callback every second */
typedef void gpcc_net_tick_t(apr_time_t now);



/**
 *  Start serving network connections. This function must be called first to 
 *  start things going.
 *  @param port the port to listen
 *  @param opened callback when a connection is open actively or passively
 *  @param closed callback when a connection is closed
 *  @param recv callback when a packet arrives at this node
 *  @param tick callback every second 
 */
APR_DECLARE(apr_status_t) gpcc_net_start(int port, 
					 gpcc_net_opened_t* opened,
					 gpcc_net_closed_t* closed,
					 gpcc_net_recv_t* recv,
					 gpcc_net_tick_t* tick);

/**
 *  Stop serving network
 */
APR_DECLARE(void) gpcc_net_stop(void);

/**
 *  Open a connction
 *  @param pconn OUT: the connection which is technically not opened until 
 *               the gpcc_net_opened_t callback function (see gpcc_net_start) 
 *               is called
 *  @param addr a network address in the form host:port
 */
APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* pconn, const char* addr);

/**
 *  Close a connection
 *  @param conn the connection to close
 */
APR_DECLARE(apr_status_t) gpcc_net_close(gpcc_net_conn_t conn);

/**
 *  Send a packet 
 *  @param conn the connection receiving the packet
 *  @param pkttype packet type
 *  @param reqid request identifier
 *  @param pkt points to the packet buffer
 *  @param pktlen how many bytes to send
 */
APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					int pkttype, int reqid, const char* pkt, int pktlen);


/**
 *  Get the ID of this node
 *  @return the ID 
 */
APR_DECLARE(gpcc_net_node_id_t) gpcc_net_my_node_id(void);

#endif /* GPCC_NET_H */
