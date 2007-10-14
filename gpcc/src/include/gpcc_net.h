#ifndef GPCC_NET_H
#define GPCC_NET_H

#include <gpcc.h>
#include <apr_time.h>

#define GPCC_NET_PACKET_PROTO		"GPCC/3.1"

#define GPCC_NET_HEADER_ENTRYMAX  (20)

typedef struct gpcc_net_packet_t gpcc_net_packet_t;

#if defined(_MSC_VER)
#pragma pack(push,1)
#define GPCC_PACKED
#else
#define GPCC_PACKED __attribute__ ((__packed__))
#endif


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

typedef struct gpcc_net_header_t gpcc_net_header_t;
struct gpcc_net_header_t {
    int argc;
    char* argv[GPCC_NET_HEADER_ENTRYMAX+1];
    int hc;
    char* hname[GPCC_NET_HEADER_ENTRYMAX+1];
    char* hvalue[GPCC_NET_HEADER_ENTRYMAX+1];
};

typedef apr_uint32_t gpcc_net_conn_t;

typedef void gpcc_net_opened_t(gpcc_net_conn_t conn, 
			       const char* peer_addr,
			       gpcc_net_header_t* to_peer, 
			       gpcc_net_header_t* from_peer,
			       apr_uint64_t arg);

typedef void gpcc_net_accepted_t(gpcc_net_conn_t conn,
				 const char* peer_addr,
				 gpcc_net_header_t* from_peer);

typedef int gpcc_net_received_t(gpcc_net_conn_t conn, 
				const char* buf, int buflen, apr_uint64_t arg);

typedef void gpcc_net_received_packet_t(gpcc_net_conn_t conn, 
					gpcc_net_packet_t* pkt, apr_uint64_t arg);

typedef void gpcc_net_closed_t(gpcc_net_conn_t conn, apr_uint64_t arg);

typedef void gpcc_net_tick_t(apr_time_t now);


APR_DECLARE(apr_status_t) gpcc_net_init(int listen_port,
					gpcc_net_opened_t* opened,
					gpcc_net_accepted_t* accepted,
					gpcc_net_received_t* received,
					gpcc_net_received_packet_t* received_packet,
					gpcc_net_closed_t* closed,
					gpcc_net_tick_t*  tick);

APR_DECLARE(apr_status_t) gpcc_net_once(int blocking);

APR_DECLARE(apr_status_t) gpcc_net_open(gpcc_net_conn_t* conn,
					char* peer_addr,
					int argc, char** argv,
					int hc, char** hname, char** hvalue,
					apr_uint64_t arg);


APR_DECLARE(void) gpcc_net_close(gpcc_net_conn_t conn);

APR_DECLARE(apr_status_t) gpcc_net_tag(gpcc_net_conn_t conn, const char* tag);

APR_DECLARE(apr_status_t) gpcc_net_set_arg(gpcc_net_conn_t conn, apr_uint64_t arg);

APR_DECLARE(gpcc_net_conn_t) gpcc_net_lookup(const char* tag);

typedef struct gpcc_net_datum_t gpcc_net_datum_t;
struct gpcc_net_datum_t {
    const char* ptr;
    int   len;
};

APR_DECLARE(apr_status_t) gpcc_net_send_header(gpcc_net_conn_t conn,
					       gpcc_net_header_t *hdr);

APR_DECLARE(apr_status_t) gpcc_net_sendv(gpcc_net_conn_t conn,
					 gpcc_net_datum_t* datum, int dcnt);


static inline apr_status_t gpcc_net_send(gpcc_net_conn_t conn, 
					 const char* buf, int buflen)
{
    gpcc_net_datum_t d;
    d.ptr =buf;
    d.len = buflen;
    return gpcc_net_sendv(conn, &d, 1);
}

APR_DECLARE(apr_status_t) gpcc_net_send_packet(gpcc_net_conn_t conn, int type, 
                                               apr_uint32_t sndidx, apr_uint32_t rcvidx,
                                               const char* buf, int len);

APR_DECLARE(apr_status_t) gpcc_net_send_packetx(const char* tag, int type, 
                                               apr_uint32_t sndidx, apr_uint32_t rcvidx,
                                               const char* buf, int len);


#endif /* GPCC_NET_H */
