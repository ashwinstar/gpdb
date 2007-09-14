#ifndef GPCC_HA_H
#define GPCC_HA_H

#include <gpcc_net.h>

typedef struct gpcc_ha_t gpcc_ha_t;
typedef void *gpcc_ha_opened_t(gpcc_ha_t *ha, void *arg);
typedef void gpcc_ha_closed_t(gpcc_ha_t *ha, void *arg);
typedef void gpcc_ha_recv_t(gpcc_ha_t *ha, gpcc_net_packet_t *packet, void *arg);

/**
 * gpcc_ha_open  open socket connection 
 *
 * @param addr  in format "hostname:port"
 * @param opened  gpcc_ha_opened_t callback function
 * @param closed  gpcc_ha_closed_t callback function
 * @param recv gpcc_ha_recv_t callback function
 * @return APR_SUCCESS if success
 */
APR_DECLARE(gpcc_ha_t *) gpcc_ha_open(const char *addr,
				      gpcc_ha_opened_t *opened,
				      gpcc_ha_closed_t *closed,
				      gpcc_ha_recv_t *recv,
				      void *arg);

/**
 * gpcc_ha_send send packet
 *
 * @param ha  ha handle
 * @param type message type
 * @param sndidx sender idx
 * @param rcvidx receiver idx
 * @param buf   content
 * @param len length of the content
 * @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_ha_send(gpcc_ha_t *ha,
				       int type,
				       const char *buf, int len);

/**
 *  gpcc_ha_close close socket
 *
 *  @param ha ha handle
 *  @return APR_SUCCESS if success
 */
			     
APR_DECLARE(apr_status_t) gpcc_ha_close(gpcc_ha_t *ha);



#endif
