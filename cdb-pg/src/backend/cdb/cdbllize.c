/*-------------------------------------------------------------------------
 *
 * cdbllize.c
 *	  Parallelize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004, Metapa
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbllize.h"

Plan *
cdbparallelize(	Plan* plan, 
				Query *query, 
				bool isCursor, 
				int cursorOptions, 
				ParamListInfo boundParams
			)
			
	/* Can be made chatty... */{
#if 0
	/* CDB Hook Candidate */
	PlanInventory inventory;
	
	if ( plan_inventory(plan, &inventory) )
	{	
		ereport(NOTICE,  (
			errmsg("CDB Notice: planner returns. (%d scans, %d vars.)",
				inventory.scans,
				inventory.vars )
			));
	}
	else
	{
		elog(NOTICE, "CDB Notice: planner returns. (Inventory failed.)");
	}
#endif

	/* ... but deep down, today, it's a NOP. */
	return plan;
}