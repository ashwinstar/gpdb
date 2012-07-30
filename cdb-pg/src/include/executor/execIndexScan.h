/*
 * execIndexScan.h
 *
 * Copyright (c) 2012 - present, EMC/Greenplum
 */
#ifndef EXECINDEXSCAN_H
#define EXECINDEXSCAN_H

#include "nodes/execnodes.h"

extern void ExecIndexBuildScanKeys(PlanState *planstate, Relation index,
					   List *quals, List *strategies, List *subtypes,
					   ScanKey *scanKeys, int *numScanKeys,
					   IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys,
					   IndexArrayKeyInfo **arrayKeys, int *numArrayKeys);
extern void ExecIndexEvalRuntimeKeys(ExprContext *econtext,
					   IndexRuntimeKeyInfo *runtimeKeys, int numRuntimeKeys);
extern bool ExecIndexEvalArrayKeys(ExprContext *econtext,
					   IndexArrayKeyInfo *arrayKeys, int numArrayKeys);
extern bool ExecIndexAdvanceArrayKeys(IndexArrayKeyInfo *arrayKeys, int numArrayKeys);

extern void InitCommonIndexScanState(IndexScanState *indexstate, IndexScan *node, EState *estate, int eflags);
extern void FreeCommonIndexScanState(IndexScanState *node);
extern Relation OpenIndexRelation(EState *estate, Oid indexOid, Index tableRtIndex);
extern void InitRuntimeKeysContext(IndexScanState *indexstate);
extern void ReEvalRuntimeKeys(IndexScanState *node, ExprContext *exprCtxt);

/*
 * getScanDirection
 *    Return estate->es_direction by default. If requestedDirection is backward,
 * flip estate->es_direction value.
 */
static inline ScanDirection
GetScanDirection(EState *estate, ScanDirection requestedDirection)
{
	ScanDirection direction = estate->es_direction;
	
	if (ScanDirectionIsBackward(requestedDirection))
	{
		if (ScanDirectionIsForward(direction))
		{
			direction = BackwardScanDirection;
		}
		
		else if (ScanDirectionIsBackward(direction))
		{
			direction = ForwardScanDirection;
		}
	}

	return direction;	
}

#endif
