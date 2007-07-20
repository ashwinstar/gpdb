#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_CONN_MAX	256
#define GPCC_NET_PROTO		"gpcc/3.1"
#define HTTP_NET_PROTO          "http/1.0"

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

/** callback when bytes arrives */
typedef void gpcc_net_recv_t(gpcc_net_conn_t conn, void* conn_arg);

/** callback every second */
typedef void gpcc_net_tick_t(apr_time_t now);



/**
 *  Start serving network connections. This function must be called first to 
 *  get things going.
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
					 gpcc_net_tick_t* tick,
					 int threaded);

/**
 *  Stop serving network
 */
APR_DECLARE(void) gpcc_net_stop(void);

APR_DECLARE(apr_status_t) gpcc_net_once(void);

/**
 *  Open a connection
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
 *  Read len bytes from the connection.
 *  @return APR_EAGAIN if less than len bytes are available, and none are read.
 */
APR_DECLARE(apr_status_t) gpcc_net_read(gpcc_net_conn_t conn, 
					char* buf, int len);

/**
 *  Read up to buflen bytes looking for \n.
 *  @param conn the Connection
 *  @param buf the buffer to return the line. It is terminated by 
 *       \n\0 if buffer is sufficiently large, or \0 only if buffer
 *       only fits a partial line.
 *  @param len size of buffer.
 *  @return APR_EAGAIN if there is not any \n terminated line in conn.
 */
APR_DECLARE(apr_status_t) gpcc_net_readln(gpcc_net_conn_t conn, 
					  char* buf, int len);


#if defined(_MSC_VER)
#pragma pack(push,1)
#define GPCC_PACKED
#else
#define GPCC_PACKED __attribute__ ((__packed__))
#endif


typedef struct gpcc_net_packet_t gpcc_net_packet_t;
struct gpcc_net_packet_t {
    struct {
	apr_uint16_t type;	/* packet type */
	apr_uint16_t len;	/* #bytes in packet body */
	apr_uint32_t sndidx;	/* sender cmdidx */
	apr_uint32_t rcvidx;	/* receiver cmdidx */
    } mh;
    char mb[1];
} GPCC_PACKED;

#ifdef _MSC_VER
#pragma pack(pop)
#endif
#undef GPCC_PACKED


APR_DECLARE(apr_status_t) gpcc_net_read_packet(gpcc_net_conn_t conn, 
					       gpcc_net_packet_t** packet);

APR_DECLARE(void) gpcc_net_free_packet(gpcc_net_packet_t* packet);

APR_DECLARE(apr_status_t) gpcc_net_send_packet(gpcc_net_conn_t conn,
					       int type, 
					       int sndidx, int rcvidx,
					       const char* buf, int len);



typedef struct gpcc_net_request_t gpcc_net_request_t;
struct gpcc_net_request_t {
    int argc;
    char** argv;
    int hc;
    char* hname[50];
    char* hvalue[50];
    int datalen;
    int offset;
    char *data;
};

/**
 *  Parse for a HTTP-style request
 *  @param buf the buffer
 *  @param len IN: readable bytes in buf[]. OUT: bytes consumed for request in buf[].
 *  @param pool where to allocate the gpcc_net_request_t return value
 *  @return NULL if a full request is still not available in the buffer
 */
APR_DECLARE(gpcc_net_request_t*) gpcc_net_parse_request(const char* buf, int* len, apr_pool_t* pool);

/**
 *  Get value of the header
 *  @param req the request
 *  @param header the header name
 *  @param def default value
 *  @return def if header not found
 */
APR_DECLARE(const char *) gpcc_net_request_get_header(gpcc_net_request_t *req, const char *header, const char *def);

/**
 *  Get integer value of the header
 *  @param req the request
 *  @param header the header name
 *  @param def default value
 *  @return def if header not found
 */
APR_DECLARE(int) gpcc_net_request_get_header_int(gpcc_net_request_t *req, const char *header, int def);

/**
 *  Get float value of the header
 *  @param req the request
 *  @param header the header name
 *  @param def default value
 *  @return def if header not found
 */
APR_DECLARE(float) gpcc_net_request_get_header_float(gpcc_net_request_t *req, const char *header, float def);



/**
 *  Parse for a HTTP-style request
 *  @param conn the connection
 *  @param req  OUT: return the request if successfully read
 *  @param pool where to allocate the gpcc_net_request_t return value
 *  @return APR_EAGAIN if a full request is still not available in the connection
 */
APR_DECLARE(apr_status_t) gpcc_net_read_request(gpcc_net_conn_t conn, 
						gpcc_net_request_t** req, 
						apr_pool_t* pool);
			  
			  
/**
 *  Read for HTTP-style request data
 *  @param conn the connection
 *  @param req the request
 *  @return APR_EAGAIN if a full request is still not avaliable in the connection
 */		     
			    
APR_DECLARE(apr_status_t) gpcc_net_read_request_data(gpcc_net_conn_t conn,
						     gpcc_net_request_t *req);

APR_DECLARE(apr_status_t) gpcc_net_send_request(gpcc_net_conn_t conn,
						const gpcc_net_request_t* req);



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



/** identifies this node - a random unique value */
typedef struct gpcc_net_node_id_t gpcc_net_node_id_t;
struct gpcc_net_node_id_t {
    unsigned char opaque[20];
};


/**
 *  Get the ID of this node
 *  @return the ID 
 */
APR_DECLARE(gpcc_net_node_id_t) gpcc_net_get_node_id(void);


#endif /* GPCC_NET_H */

