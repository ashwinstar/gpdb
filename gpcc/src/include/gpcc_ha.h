#ifndef GPCC_HA_H
#define GPCC_HA_H

#include <gpcc_net.h>

typedef struct gpcc_ha_t gpcc_ha_t;
typedef void gpcc_ha_opened_t(gpcc_ha_t *ha);
typedef void gpcc_ha_closed_t(gpcc_ha_t *ha);
typedef void gpcc_ha_recv_t(gpcc_ha_t *ha, gpcc_net_packet_t *packet);

APR_DECLARE(gpcc_ha_t *) gpcc_ha_open(const char *addr,
				      gpcc_ha_opened_t *opened,
				      gpcc_ha_closed_t *closed,
				      gpcc_ha_recv_t *recv);

APR_DECLARE(apr_status_t) gpcc_ha_send(gpcc_ha_t *ha,
				       int type,
				       int sndidx, int rcvidx,
				       const char *buf, int len);

APR_DECLARE(apr_status_t) gpcc_ha_close(gpcc_ha_t *ha);

#endif
