#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_CONN_MAX	256


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
			     const char* buf, 
			     int* bufsz); /* IN: bytes in buf to consume, OUT: bytes consumed */

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
 *  @param buf the buffer
 *  @param bufsz bytes to send
 */
APR_DECLARE(apr_status_t) gpcc_net_send(gpcc_net_conn_t conn, 
					const char* buf, int bufsz);


/**
 *  Send a packet
 *  @param conn the connection receiving the packet
 *  @param nbuf number of buffers
 *  @param buf the array of buffers containing packet to send
 *  @param bufsz size of elements in buf[]
 */
APR_DECLARE(apr_status_t) gpcc_net_sendv(gpcc_net_conn_t conn, 
					 int nbuf,
					 const char* buf[], int bufsz[]);


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
 *  Get active connections.
 *  @param conns OUT: current active connections. This array must have capacity have 
 *          GPCC_NET_CONN_MAX elements.
 *  @return number of active connections returned in conns[]
 */
APR_DECLARE(int) gpcc_net_get_conns(gpcc_net_conn_t conns[GPCC_NET_CONN_MAX]);


typedef struct gpcc_net_headers_t gpcc_net_headers_t;
struct gpcc_net_headers_t {
    int   hcount;
    char* hname[];
    char* hvalue[];
};

APR_DECLARE(gpcc_net_headers_t*) gpcc_net_parse_headers(const char* buf, int* len, apr_pool_t* pool);

#endif /* GPCC_NET_H */

