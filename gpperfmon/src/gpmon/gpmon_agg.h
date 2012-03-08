#ifndef GPMON_AGG_H
#define GPMON_AGG_H

#include "apr_pools.h"
#include "gpmonlib.h"

typedef struct agg_t agg_t;
extern apr_status_t agg_create(agg_t** agg, apr_int64_t generation,
			       apr_pool_t* pool);
extern apr_status_t agg_dup(agg_t** agg, agg_t* oldagg, apr_pool_t* pool);
extern void agg_destroy(agg_t* agg);
extern apr_status_t agg_put(agg_t* agg, const gp_smon_to_mmon_packet_t* pkt);
extern apr_status_t agg_dump(agg_t* agg);

#endif
