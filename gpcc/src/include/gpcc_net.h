#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_CONN_MAX	256
#define GPCC_NET_PROTO          "gpcc/3.1"


typedef struct gpcc_net_msghdr_t gpcc_net_msghdr_t;
struct gpcc_net_msghdr_t {
    apr_uint16_t mtype;		/* packet type */
    apr_uint16_t mlen;		/* #bytes in packet body */
    apr_uint32_t sndidx;		/* sender cmdidx */
    apr_uint32_t rcvidx;		/* receiver cmdidx */
};

#define GPCC_NET_MSGHDR_FILL(mh, t, l, s, r)	\
    (mh)->mtype = t, (mh)->mlen = l, (mh)->sndidx = s, (mh)->rcvidx = r

/** identifies this node - a random unique value */
typedef struct gpcc_net_node_id_t gpcc_net_node_id_t;
struct gpcc_net_node_id_t {
    unsigned char opaque[20];
};

/** identifies a connection, only non-zero values are valid */
typedef apr_uint32_t gpcc_net_conn_t;

/** 
 Callback when a connection is opened. For conn that is opened locally,
 the self_origin flag is set, and conn_arg contains the arg supplied to 
 gpcc_net_open(). For conn that is opened by remote node, both the
 self_origin flag and conn_arg are NULL. In any case, this routine 
 should return a newly allocated conn_org or echo the conn_arg that
 was passed in.
 */
typedef void* gpcc_net_opened_t(gpcc_net_conn_t conn, void* conn_arg, 
				int self_origin,
				apr_pool_t* pool);

/** callback when a connection is closed */
typedef void gpcc_net_closed_t(gpcc_net_conn_t conn, void* conn_arg);

/** callback when a packet arrives */
typedef void gpcc_net_recv_t(gpcc_net_conn_t conn, void* conn_arg,
			     const gpcc_net_msghdr_t* msghdr, const char* msgbody);

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
 *  @param conn_arg pointer to user supplied data structure. This will be passed
 *		to the gpcc_net_opened callback.
 */
APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* pconn, 
					const char* addr,
					void* conn_arg);

/**
 *  Close a connection
 *  @param conn the connection to close
 */
APR_DECLARE(apr_status_t) gpcc_net_close(gpcc_net_conn_t conn);




/**
 *  Send a packet 
 *  @param conn the connection receiving the packet
 *  @param msghdr the message header
 *  @param msgbody the message body
 */
APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					const gpcc_net_msghdr_t* msghdr, 
					const char* msgbody);


/**
 *  Set the user data pointer associated with a connection.
 *  @param conn the connection
 *  @param conn_arg pointer to user supplied data structure.
 */
APR_DECLARE(apr_status_t) gpcc_net_setarg(gpcc_net_conn_t conn, void* conn_arg);


/**
 *  Retrieve the user data pointer associated with a connection.
 *  @param conn the connection
 *  @return pointer to user supplied data structure currently associated with conn
 */
APR_DECLARE(void*) gpcc_net_getarg(gpcc_net_conn_t conn);




/**
 *  Get the ID of this node
 *  @return the ID 
 */
APR_DECLARE(gpcc_net_node_id_t) gpcc_net_my_node_id(void);


/**
 *  Get active connections.
 *  @param conns OUT: current active connections. This array must have capacity have 
 *          GPCC_NET_CONN_MAX elements.
 *  @return number of active connections returned in conns[]
 */
APR_DECLARE(int) gpcc_net_get_conns(gpcc_net_conn_t conns[GPCC_NET_CONN_MAX]);

#endif /* GPCC_NET_H */

