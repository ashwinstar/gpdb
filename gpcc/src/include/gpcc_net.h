#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_PACKET_PROTO		"GPCC/3.1"



#if defined(_MSC_VER)
#pragma pack(push,1)
#define GPCC_PACKED
#else
#define GPCC_PACKED __attribute__ ((__packed__))
#endif

/**
 *  Packet structure.
 */
typedef struct gpcc_net_packet_t gpcc_net_packet_t;
struct gpcc_net_packet_t {
    struct {
	apr_uint32_t _reserved;	/* internal use */
	apr_uint16_t type;      /* packet type */
	apr_uint16_t len;       /* #bytes in packet body */
	apr_uint32_t sndidx;    /* sender cmdidx */
	apr_uint32_t rcvidx;    /* receiver cmdidx */
    } mh;
    char        payload[1];
} GPCC_PACKED;

#ifdef _MSC_VER
#pragma pack(pop)
#endif
#undef GPCC_PACKED


/**
 *  Current tick, updated once every second
 */
extern apr_int64_t gpcc_net_tick;


/**
 *  HTTP or GPCC_NET_PACKET_PROTO header
 */
typedef struct gpcc_net_header_t gpcc_net_header_t;
struct gpcc_net_header_t {
#define GPCC_NET_HEADER_ENTRYMAX  (20)
    int argc;
    const char* argv[GPCC_NET_HEADER_ENTRYMAX+1];
    int hc;
    const char* hname[GPCC_NET_HEADER_ENTRYMAX+1];
    const char* hvalue[GPCC_NET_HEADER_ENTRYMAX+1];
    const char* post;
};


/**
 * A parsed URI 
 */
typedef struct gpcc_net_uri_t gpcc_net_uri_t;
struct gpcc_net_uri_t {
    /* uri: a/b/c?d=e&f=g#k 
       -> path = /a/b/c
       -> pc = 2
       -> pname[0], pvalue[0] = d, e
       -> pname[1], pvalue[1] = f, g
       -> fragment = k
    */
    char* uri;
    char* path;
    int   pc;
    char* pname[20];
    char* pvalue[20];
    char* fragment;
};


/**
 *  Address a connection.
 */
typedef apr_uint32_t gpcc_net_conn_t;


/**
 *  Callback on successful TCP + header connect to remote peer.
 *  @param conn The connection.
 *  @param peer_addr Peer address in the format IP:port.
 *  @param to_peer The net header that was sent to peer. Sourced from prior gpcc_net_open().
 *  @param from_peer The net header that the peer sent back.
 *  @param arg The argument associated with conn (from prior gpcc_net_open() call).
 */
typedef void gpcc_net_opened_t(gpcc_net_conn_t conn, 
			       const char* peer_addr,
			       gpcc_net_header_t* to_peer, 
			       gpcc_net_header_t* from_peer,
			       apr_uint64_t arg);

/**
 *  Callback on successful TCP + header accept from remote host.
 *  @param conn The connection.
 *  @param peer_addr Peer address in the format IP:port.
 *  @param from_peer The net header that the peer sent to initiate connection.
 */
typedef void gpcc_net_accepted_t(gpcc_net_conn_t conn,
				 const char* peer_addr,
				 gpcc_net_header_t* from_peer);

/**
 *  Callback when some bytes arrived from peer.
 *  @param conn The connection.
 *  @param buf Where the bytes reside.
 *  @param buflen How many bytes arrived.
 *  @param arg The argument associated with conn.
 */
typedef int gpcc_net_received_t(gpcc_net_conn_t conn, 
				const char* buf, int buflen, apr_uint64_t arg);

/**
 *  Callback when a packet arrived from peer.
 *  @param conn The connection.
 *  @param pkt The new packet.
 *  @param arg The argument associated with conn.
 */
typedef void gpcc_net_received_packet_t(gpcc_net_conn_t conn, 
					gpcc_net_packet_t* pkt, apr_uint64_t arg);

/**
 *  Callback when the connection closed. Can be initiated locally or remotely.
 *  @param conn The connection.
 *  @param arg The argument associated with conn.
 */
typedef void gpcc_net_closed_t(gpcc_net_conn_t conn, apr_uint64_t arg);


/**
 *  Callback from time to time (approx 1 sec).
 *  @param now The current time in microsec since epoch.
 */
typedef void gpcc_net_tick_t(apr_time_t now);


/**
 *  Initialize the gpcc_net module.
 *  @param listen_port Which port to listen for incoming connection. 0 if not active.
 *  @param opened Callback routine when connection opened.
 *  @param accepted Callback routine when connection accepted.
 *  @param received Callback routine when bytes arrived.
 *  @param received_packet Callback routine when packets arrived.
 *  @param closed Callback routine when connection closed.
 *  @param tick Callback routine for time tick.
 */
APR_DECLARE(apr_status_t) 
    gpcc_net_init(int listen_port,
		  gpcc_net_opened_t* opened,
		  gpcc_net_accepted_t* accepted,
		  gpcc_net_received_t* received,
		  gpcc_net_received_packet_t* received_packet,
		  gpcc_net_closed_t* closed,
		  gpcc_net_tick_t*  tick);

/**
 *  Wait for a network or tick event.
 *  @param blocking flag: don't return before network or tick event occurs.
 */
APR_DECLARE(apr_status_t) 
    gpcc_net_once(int blocking);

/**
 *  Open a connection.
 *  @param conn return the new connection.
 *  @param peer_addr Where to connect to, in format IP:port.
 *  @param argc argv[0..argc).
 *  @param argv strings for the first line of header (e.g. "GET" "/uri" "HTTP/1.0").
 *  @param hc hname[0..hc).
 *  @param hname name of header lines, e.g. "Content-type" "Content-length".
 *  @param hvalue value of header liens, e.g. "text/plain" "1024"
 *  @param arg The argument associated with the new connection.
 */
APR_DECLARE(apr_status_t) 
    gpcc_net_open(gpcc_net_conn_t* conn,
		  const char* peer_addr,
		  int argc, const char** argv,
		  int hc, const char** hname, const char** hvalue,
		  apr_uint64_t arg);


/**
 *  Close the connection.
 *  @param conn The connection.
 *  @param flush If set, then flush prior to close. Otherwise, close immediately.
 */
APR_DECLARE(void) 
    gpcc_net_close(gpcc_net_conn_t conn, int flush);

    
/**
 *  Set a tag for the connection.
 *  @param conn The connection.
 *  @param tag The tag name. A copy will be made and stored with conn.
 */
APR_DECLARE(apr_status_t)
    gpcc_net_tag(gpcc_net_conn_t conn, const char* tag);

/**
 *  Find a connection by tag name.
 *  @param tag The tag name.
 *  @return A connection with that tag name, or 0 if not found.
 */
APR_DECLARE(gpcc_net_conn_t)
    gpcc_net_lookup(const char* tag);

/**
 *  Set the argument for the connection.
 *  @param conn The connection.
 *  @param arg The argument.
 */
APR_DECLARE(apr_status_t)
    gpcc_net_set_arg(gpcc_net_conn_t conn, apr_uint64_t arg);


/**
 *  A datum is a pointer, length pair.
 */
typedef struct gpcc_net_datum_t gpcc_net_datum_t;
struct gpcc_net_datum_t {
    const char* ptr;
    int   len;
};

/**
 *  Send a net header. Usually used in accepted callback to send a
 *  reply net header to the peer that initiated the connection.
 *  @param conn The connection.
 *  @param hdr The header to send.
 */
APR_DECLARE(apr_status_t)
    gpcc_net_send_header(gpcc_net_conn_t conn,
			 gpcc_net_header_t *hdr);

/**
 *  Send data in some datum to remote peer.
 *  @param conn The connection.
 *  @param datum The datums to send.
 *  @param dcnt How many datums were there.
 */
APR_DECLARE(apr_status_t)
    gpcc_net_sendv(gpcc_net_conn_t conn,
		   gpcc_net_datum_t* datum, int dcnt);


/**
 *  Send some bytes to remote peer.
 *  @param conn The connection.
 *  @param buf The bytes to send.
 *  @param buflen How many bytes were there.
 */
static inline apr_status_t gpcc_net_send(gpcc_net_conn_t conn, 
					 const char* buf, int buflen)
{
    gpcc_net_datum_t d;
    d.ptr =buf;
    d.len = buflen;
    return gpcc_net_sendv(conn, &d, 1);
}


/**
 *  Send a packet to remote peer.
 *  @param conn The connection
 *  @param type The packet type.
 *  @param sndidx The sender index.
 *  @param rcvidx The receiver index.
 *  @param buf The body of the packet.
 *  @param len How many bytes were in buf.
 */
APR_DECLARE(apr_status_t) 
    gpcc_net_send_packet(gpcc_net_conn_t conn, int type, 
			 apr_uint32_t sndidx, apr_uint32_t rcvidx,
			 const char* buf, int len);

/**
 *  Send a packet to remote peer.
 *  @param tag The tag of the connection.
 *  @param type The packet type.
 *  @param sndidx The sender index.
 *  @param rcvidx The receiver index.
 *  @param buf The body of the packet.
 *  @param len How many bytes were in buf.
 */
APR_DECLARE(apr_status_t)
    gpcc_net_send_packetx(const char* tag, int type, 
			  apr_uint32_t sndidx, apr_uint32_t rcvidx,
			  const char* buf, int len);


/**
 *  Allocate a URI by parsing uristr. Must call gpcc_net_uri_free() when done.
 *  @param uristr The URI in string form, e.g. /the/path?a=x&b=y#fragment
 *  @return A newly allocated URI if successful, 0 if bad URI.
 */
APR_DECLARE(gpcc_net_uri_t*) 
    gpcc_net_uri_alloc(const char* uristr);

/**
 *  Free a URI structure returned by gpcc_net_uri_alloc.
 *  @param uri The uri to free.
 */
APR_DECLARE(void) 
    gpcc_net_uri_free(gpcc_net_uri_t* uri);


/**
 *  Find name in parameters of a URI.
 *  @param uri The URI.
 *  @param name The parameter to look for.
 *  @return The value of a parameter matching name, or 0 if not found.
 */
static inline char* gpcc_net_uri_param(gpcc_net_uri_t* uri, const char* name)
{
    int i;
    for (i = 0; i < uri->pc; i++) 
	if (0 == strcmp(uri->pname[i], name))
	    return uri->pvalue[i];
    return 0;
}


/**
 *  Send a http response and close the connection.
 */
APR_DECLARE(void)
    gpcc_net_http_close(gpcc_net_conn_t conn, int status, const char* msg, const char* json);

#endif /* GPCC_NET_H */
