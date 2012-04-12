/*
 * execGpmon.c
 *   Gpmon related functions inside executor.
 *
 * Copyright (c) 2012 - present, EMC/Greenplum
 */
#include "postgres.h"

#include "executor/executor.h"
#include "utils/lsyscache.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbgang.h"
#include "miscadmin.h"

char * GetScanRelNameGpmon(Oid relid, char schema_rel_name[SCAN_REL_NAME_BUF_SIZE])
{
	if (relid > 0)
	{
		char *relname = get_rel_name(relid);
		char *schemaname = get_namespace_name(get_rel_namespace(relid));
		snprintf(schema_rel_name, SCAN_REL_NAME_BUF_SIZE, "%s.%s", schemaname, relname);
		if (relname)
		{
			pfree(relname);
		}
		
		if (schemaname)
		{
			pfree(schemaname);
		}
	}
	return schema_rel_name;
}

/*
 * toSendGpmonExecutingPkt
 *   Return true if a perfmon executing packet needs to be sent for the given plan node.
 *
 * In most cases, this function will return true. The exception is for a Sort/Material
 * that is called by the SharedNode and is not in a driver slice.
 */
static bool
toSendGpmonExecutingPkt(Plan *plan, EState *estate)
{
	bool doSend = true;
	Assert(plan != NULL);
	if (IsA(plan, Material))
	{
		Material *ma = (Material *)plan;
		doSend = (ma->share_type == SHARE_NOTSHARED ||
				  ma->share_type == SHARE_MATERIAL ||
				  ma->driver_slice == LocallyExecutingSliceIndex(estate));
	}
	
	else if (IsA(plan, Sort))
	{
		Sort *sort = (Sort *)plan;
		doSend = (sort->share_type == SHARE_NOTSHARED ||
				  sort->share_type == SHARE_MATERIAL ||
				  sort->driver_slice == LocallyExecutingSliceIndex(estate));
		
	}
	
	return doSend;
}

void CheckSendPlanStateGpmonPkt(PlanState *ps)
{
	if (ps == NULL ||
		ps->state == NULL)
	{
		return;
	}

	if(gp_enable_gpperfmon)
	{
		/*
		 * When Sort/Material is called by the SharedNode that is not
		 * a driver slice, do not send gpmon packet.
		 */
		Assert(ps->plan != NULL);
		bool doSend = toSendGpmonExecutingPkt(ps->plan, ps->state);
		if (!doSend)
		{
			return;
		}
		
		if(!ps->fHadSentGpmon || ps->gpmon_plan_tick != gpmon_tick)
		{
			gpmon_send(&ps->gpmon_pkt);
			ps->fHadSentGpmon = true;
		}

		ps->gpmon_plan_tick = gpmon_tick;
	}
}

/*
 * NodeInExecutingSlice
 *   Return true if the given node is running in the same slice of the
 * executing QE.
 *
 * Each motion node has a sender and a receiver slice, so for the same
 * motion node, this function returns true in two slices.
 *
 * For all other nodes, the node's slice id is stored in
 * estate->currentExecutingSliceId.
 */
static bool
NodeInExecutingSlice(Plan *plan, EState *estate)
{
	Assert(plan != NULL && estate != NULL);
     
	/*
	 * For Motion node, check either the sender slice or the receiver
	 * slice is running on the same slice as this QE. For the Motion
	 * nodes in the top slice of an InitPlan, the executing slice
	 * id might be different from the receiver slice id. We need to
	 * check that as well.
	 */
	if (IsA(plan, Motion))
	{
		Motion *motion = (Motion *)plan;
 
		SliceTable *sliceTable = estate->es_sliceTable;
 
		Slice *sendSlice = (Slice *)list_nth(sliceTable->slices, motion->motionID);
		Assert(sendSlice->parentIndex >= 0);
		Slice *recvSlice = (Slice *)list_nth(sliceTable->slices, sendSlice->parentIndex);
		int localSliceId = LocallyExecutingSliceIndex(estate);
          
		return (localSliceId == recvSlice->sliceIndex ||
				localSliceId == sendSlice->sliceIndex ||
				localSliceId == estate->currentExecutingSliceId);
	}
 
	/* For other nodes except for Motion, simple check estate->currentExecutingSliceId. */
	return (LocallyExecutingSliceIndex(estate) == estate->currentExecutingSliceId);
}

void EndPlanStateGpmonPkt(PlanState *ps)
{
	if(ps == NULL ||
	   ps->state == NULL)
	{
		return;
	}

	/*
	 * If this operator is not running in this slice, do not
	 * send the finished packet.
	 */
	if (!NodeInExecutingSlice(ps->plan, ps->state))
	{
		return;
	}

	ps->gpmon_pkt.u.qexec.status = (uint8)PMNS_Finished;

	if(gp_enable_gpperfmon)
	{
		gpmon_send(&ps->gpmon_pkt);
	}
}

/*
 * InitPlanNodeGpmonPkt -- initialize the init gpmon package, and send it off.
 */
void InitPlanNodeGpmonPkt(Plan *plan, gpmon_packet_t *gpmon_pkt, EState *estate,
						  PerfmonNodeType type,
						  int64 rowsout_est,
						  char* relname)
{
	int rowsout_adjustment_factor = 0;

	if (plan == NULL ||
		estate == NULL)
	{
		return;
	}

	/*
	 * If this operator is not running in this slice, do not
	 * send the init packet.
	 */
	if (!NodeInExecutingSlice(plan, estate))
	{
		return;
	}
	
	/* The estimates are now global so we need to adjust by
	 * the number of segments in the array.
	 */
	rowsout_adjustment_factor = getgpsegmentCount();

	/* Make sure we don't div by zero below */
	if (rowsout_adjustment_factor < 1)
		rowsout_adjustment_factor = 1;

	Assert(rowsout_adjustment_factor >= 1);

	memset(gpmon_pkt, 0, sizeof(gpmon_packet_t));

	gpmon_pkt->magic = GPMON_MAGIC;
	gpmon_pkt->version = GPMON_PACKET_VERSION;
	gpmon_pkt->pkttype = GPMON_PKTTYPE_QEXEC;

	gpmon_gettmid(&gpmon_pkt->u.qexec.key.tmid);
	gpmon_pkt->u.qexec.key.ssid = gp_session_id;
	gpmon_pkt->u.qexec.key.ccnt = gp_command_count;
	gpmon_pkt->u.qexec.key.hash_key.segid = Gp_segment;
	gpmon_pkt->u.qexec.key.hash_key.pid = MyProcPid;
	gpmon_pkt->u.qexec.key.hash_key.nid = plan->plan_node_id;

	gpmon_pkt->u.qexec.pnid = plan->plan_parent_node_id;


	gpmon_pkt->u.qexec.nodeType = (apr_uint16_t)type;

	gpmon_pkt->u.qexec.rowsout = 0;
	gpmon_pkt->u.qexec.rowsout_est = rowsout_est / rowsout_adjustment_factor;

	if (relname)
	{
		snprintf(gpmon_pkt->u.qexec.relation_name, sizeof(gpmon_pkt->u.qexec.relation_name), "%s", relname);
	}

	gpmon_pkt->u.qexec.status = (uint8)PMNS_Initialize;

	if(gp_enable_gpperfmon)
	{
		gpmon_send(gpmon_pkt);
	}

	gpmon_pkt->u.qexec.status = (uint8)PMNS_Executing;
}
