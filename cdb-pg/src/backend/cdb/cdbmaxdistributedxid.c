/*-------------------------------------------------------------------------
 *
 * cdbmaxdistributedxid.c
 *		Function to return maximum distributed transaction id.
 *
 * IDENTIFICATION
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/heapam.h"
#include "access/cdbdistributedxidmap.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbtm.h"

Datum		gp_max_distributed_xid(PG_FUNCTION_ARGS);

Datum
gp_max_distributed_xid(PG_FUNCTION_ARGS)
{
	DistributedTransactionId max = GetMaxDistributedXid();

	elog(LOG,"gp_max_distributed_xid returning %u",max);
	
	PG_RETURN_XID(max);

}
