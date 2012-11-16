//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2010 Greenplum, Inc.
//
//	@filename:
//		CTranslatorDXLToPlStmt.cpp
//
//	@doc:
//		Implementation of the methods for translating from DXL tree to GPDB
//		PlannedStmt.
//
//	@owner:
//		antovl
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "postgres.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "gpos/base.h"

#include "dxl/operators/CDXLNode.h"

#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/translate/CTranslatorDXLToPlStmt.h"
#include "gpopt/translate/CTranslatorUtils.h"

#include "md/IMDFunction.h"
#include "md/IMDScalarOp.h"
#include "md/IMDAggregate.h"
#include "md/IMDType.h"
#include "md/IMDTypeBool.h"
#include "md/IMDIndex.h"

#include "gpopt/gpdbwrappers.h"

using namespace gpdxl;
using namespace gpos;
using namespace gpopt;
using namespace gpmd;

#define GPDXL_ROOT_PLAN_ID -1
#define GPDXL_PLAN_ID_START 1
#define GPDXL_MOTION_ID_START 1
#define GPDXL_PARAM_ID_START 0

//---------------------------------------------------------------------------
//	@function:
//		ShtypeFromSpoolInfo
//
//	@doc:
//		Helper function for extracting the GPDB share type from a DXL spool info object
//
//---------------------------------------------------------------------------
ShareType
ShtypeFromSpoolInfo(const CDXLSpoolInfo *pspoolinfo)
{
	ShareType shtype = SHARE_NOTSHARED;

	GPOS_ASSERT(NULL != pspoolinfo);

	if (EdxlspoolMaterialize == pspoolinfo->Edxlsptype())
	{
		shtype = SHARE_MATERIAL;
		if (pspoolinfo->FMultiSlice())
		{
			shtype = SHARE_MATERIAL_XSLICE;
		}
	}
	else if (EdxlspoolSort == pspoolinfo->Edxlsptype())
	{
		shtype = SHARE_SORT;
		if (pspoolinfo->FMultiSlice())
		{
			shtype = SHARE_SORT_XSLICE;
		}
	}

	return shtype;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CTranslatorDXLToPlStmt
//
//	@doc:
//		Constructor
//
//---------------------------------------------------------------------------
CTranslatorDXLToPlStmt::CTranslatorDXLToPlStmt
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CContextDXLToPlStmt* pctxdxltoplstmt
	)
	:
	m_pmp(pmp),
	m_pmda(pmda),
	m_pctxdxltoplstmt(pctxdxltoplstmt),
	m_cmdtype(CMD_SELECT),
	m_fTargetTableDistributed(false),
	m_plResultRelations(NULL)
{
	m_pdxlsctranslator = New (m_pmp) CTranslatorDXLToScalar
										(
										m_pmp,
										m_pmda
										);

	InitTranslators();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::~CTranslatorDXLToPlStmt
//
//	@doc:
//		Destructor
//
//---------------------------------------------------------------------------
CTranslatorDXLToPlStmt::~CTranslatorDXLToPlStmt()
{
	delete m_pdxlsctranslator;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::InitTranslators
//
//	@doc:
//		Initialize index of translators
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::InitTranslators()
{
	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(m_rgpfTranslators); ul++)
	{
		m_rgpfTranslators[ul] = NULL;
	}

	// array mapping operator type to translator function
	STranslatorMapping rgTranslators[] =
	{
			{EdxlopPhysicalTableScan,				&gpopt::CTranslatorDXLToPlStmt::PssFromDXLTblScan},
			{EdxlopPhysicalIndexOnlyScan,			&gpopt::CTranslatorDXLToPlStmt::PiosFromDXLIndexOnlyScan},
			{EdxlopPhysicalIndexScan,				&gpopt::CTranslatorDXLToPlStmt::PisFromDXLIndexScan},
			{EdxlopPhysicalHashJoin, 				&gpopt::CTranslatorDXLToPlStmt::PhjFromDXLHJ},
			{EdxlopPhysicalNLJoin, 					&gpopt::CTranslatorDXLToPlStmt::PnljFromDXLNLJ},
			{EdxlopPhysicalMergeJoin,				&gpopt::CTranslatorDXLToPlStmt::PmjFromDXLMJ},
			{EdxlopPhysicalMotionGather,			&gpopt::CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion},
			{EdxlopPhysicalMotionBroadcast,			&gpopt::CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion},
			{EdxlopPhysicalMotionRedistribute,		&gpopt::CTranslatorDXLToPlStmt::PplanTranslateDXLMotion},
			{EdxlopPhysicalMotionRandom,			&gpopt::CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion},
			{EdxlopPhysicalMotionRoutedDistribute,	&gpopt::CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion},
			{EdxlopPhysicalLimit, 					&gpopt::CTranslatorDXLToPlStmt::PlimitFromDXLLimit},
			{EdxlopPhysicalAgg, 					&gpopt::CTranslatorDXLToPlStmt::PaggFromDXLAgg},
			{EdxlopPhysicalWindow, 					&gpopt::CTranslatorDXLToPlStmt::PwindowFromDXLWindow},
			{EdxlopPhysicalSort,					&gpopt::CTranslatorDXLToPlStmt::PsortFromDXLSort},
			{EdxlopPhysicalSubqueryScan,			&gpopt::CTranslatorDXLToPlStmt::PsubqscanFromDXLSubqScan},
			{EdxlopPhysicalResult, 					&gpopt::CTranslatorDXLToPlStmt::PresultFromDXLResult},
			{EdxlopPhysicalAppend, 					&gpopt::CTranslatorDXLToPlStmt::PappendFromDXLAppend},
			{EdxlopPhysicalMaterialize, 			&gpopt::CTranslatorDXLToPlStmt::PmatFromDXLMaterialize},
			{EdxlopPhysicalSharedScan, 				&gpopt::CTranslatorDXLToPlStmt::PshscanFromDXLSharedScan},
			{EdxlopPhysicalSequence, 				&gpopt::CTranslatorDXLToPlStmt::PplanSequence},
			{EdxlopPhysicalDynamicTableScan,		&gpopt::CTranslatorDXLToPlStmt::PplanDTS},
			{EdxlopPhysicalTVF,						&gpopt::CTranslatorDXLToPlStmt::PplanFunctionScanFromDXLTVF},
			{EdxlopPhysicalDML,						&gpopt::CTranslatorDXLToPlStmt::PplanDML},
			{EdxlopPhysicalSplit,					&gpopt::CTranslatorDXLToPlStmt::PplanSplit},
			{EdxlopPhysicalRowTrigger,				&gpopt::CTranslatorDXLToPlStmt::PplanRowTrigger},
			{EdxlopPhysicalAssert,					&gpopt::CTranslatorDXLToPlStmt::PplanAssert},
	};

	const ULONG ulTranslators = GPOS_ARRAY_SIZE(rgTranslators);

	for (ULONG ul = 0; ul < ulTranslators; ul++)
	{
		STranslatorMapping elem = rgTranslators[ul];
		m_rgpfTranslators[elem.edxlopid] = elem.pf;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplstmtFromDXL
//
//	@doc:
//		Translate DXL node into a PlannedStmt
//
//---------------------------------------------------------------------------
PlannedStmt *
CTranslatorDXLToPlStmt::PplstmtFromDXL
	(
	const CDXLNode *pdxln
	)
{
	GPOS_ASSERT(NULL != pdxln);

	CDXLTranslateContext dxltrctx(m_pmp, false);

	Plan *pplan = PplFromDXL(pdxln, &dxltrctx, NULL);

	GPOS_ASSERT(NULL != pplan);

	// collect oids from rtable
	List *plOids = NIL;

	ListCell *plcRTE = NULL;
	ForEach (plcRTE, m_pctxdxltoplstmt->PlPrte())
	{
		RangeTblEntry *pRTE = (RangeTblEntry *) lfirst(plcRTE);

		if (pRTE->rtekind == RTE_RELATION)
		{
			plOids = gpdb::PlAppendOid(plOids, pRTE->relid);
		}
	}

	// assemble planned stmt
	PlannedStmt *pplstmt = MakeNode(PlannedStmt);
	pplstmt->planGen = PLANGEN_OPTIMIZER;
	
	pplstmt->rtable = m_pctxdxltoplstmt->PlPrte();
	pplstmt->subplans = m_pctxdxltoplstmt->PlPplanSubplan();
	pplstmt->planTree = pplan;

	// store partitioned table indexes in planned stmt
	pplstmt->queryPartOids = m_pctxdxltoplstmt->PlPartitionedTables();
	pplstmt->canSetTag = true;
	pplstmt->relationOids = plOids;

	pplan->nMotionNodes  = m_pctxdxltoplstmt->UlCurrentMotionId()-1;
	pplstmt->nMotionNodes =  m_pctxdxltoplstmt->UlCurrentMotionId()-1;

	pplstmt->commandType = m_cmdtype;
	
	GPOS_ASSERT(pplan->nMotionNodes >= 0);
	if (0 == pplan->nMotionNodes && !m_fTargetTableDistributed)
	{
		// no motion nodes and not a DML on a distributed table
		pplan->dispatch = DISPATCH_SEQUENTIAL;
	}
	else
	{
		pplan->dispatch = DISPATCH_PARALLEL;
	}
	
	pplstmt->resultRelations = m_plResultRelations;
	
	SetInitPlanVariables(pplstmt);
	return pplstmt;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplFromDXL
//
//	@doc:
//		Translates a DXL tree into a Plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplFromDXL
	(
	const CDXLNode *pdxln,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	GPOS_ASSERT(NULL != pdxln);

	CDXLOperator *pdxlop = pdxln->Pdxlop();
	ULONG ulOpId =  (ULONG) pdxlop->Edxlop();

	PfPplan pf = m_rgpfTranslators[ulOpId];

	if (NULL == pf)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiDXL2PlStmtConversion, pdxln->Pdxlop()->PstrOpName()->Wsz());
	}

	return (this->* pf)(pdxln, pdxltrctxOut, pplanParent);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::IPlanId
//
//	@doc:
//		If pplParent == NULL returns -1 else eeturns the plan id.
//
//---------------------------------------------------------------------------
INT
CTranslatorDXLToPlStmt::IPlanId(Plan* pplParent)
{
	if (NULL == pplParent)
	{
		return GPDXL_ROOT_PLAN_ID;
	}
	else
	{
		return pplParent->plan_node_id;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetInitPlanVariables
//
//	@doc:
//		Iterates over the plan to set the qDispSliceId that is found in the plan
//		as well as its subplans. Set the number of parameters used in the plan.
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetInitPlanVariables(PlannedStmt* pplstmt)
{
	if(1 != m_pctxdxltoplstmt->UlCurrentMotionId()) // For Distributed Tables m_ulMotionId > 1
	{
		pplstmt->nInitPlans = m_pctxdxltoplstmt->UlCurrentParamId();
		pplstmt->planTree->nInitPlans = m_pctxdxltoplstmt->UlCurrentParamId();
	}

	pplstmt->planTree->nParamExec = m_pctxdxltoplstmt->UlCurrentParamId();
	pplstmt->nCrossLevelParams = m_pctxdxltoplstmt->UlCurrentParamId();

	// Extract all subplans defined in the planTree
	List *plSubPlans = gpdb::PlExtractNodesPlan(pplstmt->planTree, T_SubPlan, true);

	ListCell *plc = NULL;

	ForEach (plc, plSubPlans)
	{
		SubPlan *psubplan = (SubPlan*) lfirst(plc);
		if (psubplan->is_initplan)
		{
			SetInitPlanSliceInformation(psubplan);
		}
	}

	// InitPlans can also be defined in subplans. We therefore have to iterate
	// over all the subplans referred to in the planned statement.

	List *plInitPlans = pplstmt->subplans;

	ForEach (plc,plInitPlans)
	{
		plSubPlans = gpdb::PlExtractNodesPlan((Plan*) lfirst(plc), T_SubPlan, true);
		ListCell *plc2;

		ForEach (plc2, plSubPlans)
		{
			SubPlan *psubplan = (SubPlan*) lfirst(plc2);
			if (psubplan->is_initplan)
			{
				SetInitPlanSliceInformation(psubplan);
			}
		}
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetInitPlanSliceInformation
//
//	@doc:
//		Set the qDispSliceId for a given subplans. In GPDB once all motion node
// 		have been assigned a slice, each initplan is assigned a slice number.
//		The initplan are traversed in an postorder fashion. Since in CTranslatorDXLToPlStmt
//		we assign the plan_id to each initplan in a postorder fashion, we take
//		advantage of this.
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetInitPlanSliceInformation(SubPlan * psubplan)
{
	GPOS_ASSERT(psubplan->is_initplan && "This is processed for initplans only");

	if (psubplan->is_initplan)
	{
		GPOS_ASSERT(0 < m_pctxdxltoplstmt->UlCurrentMotionId());

		if(1 < m_pctxdxltoplstmt->UlCurrentMotionId())
		{
			psubplan->qDispSliceId =  m_pctxdxltoplstmt->UlCurrentMotionId() + psubplan->plan_id-1;
		}
		else
		{
			psubplan->qDispSliceId = 0;
		}
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::SetParamIds
//
//	@doc:
//		Set the bitmapset with the param_ids defined in the plan
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::SetParamIds(Plan* pplan)
{
	List *plParams = gpdb::PlExtractNodesPlan(pplan, T_Param, true);

	ListCell *plc = NULL;

	Bitmapset  *pbitmapset = NULL;

	ForEach (plc, plParams)
	{
		Param *pparam = (Param*) lfirst(plc);
		pbitmapset = gpdb::PbmsAddMember(pbitmapset, pparam->paramid);
	}

	pplan->extParam = pbitmapset;
	pplan->allParam = pbitmapset;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PssFromDXLTblScan
//
//	@doc:
//		Translates a DXL table scan node into a SeqScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PssFromDXLTblScan
	(
	const CDXLNode *pdxlnTblScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalTableScan *pdxlopTS = CDXLPhysicalTableScan::PdxlopConvert(pdxlnTblScan->Pdxlop());

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	// we will add the new range table entry as the last element of the range table
	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;

	RangeTblEntry *prte = PrteFromTblDescr(pdxlopTS->Pdxltabdesc(), NULL /*pdxlid*/, iRel, &dxltrctxbt);
	GPOS_ASSERT(NULL != prte);

	m_pctxdxltoplstmt->AddRTE(prte);
	//(* (m_pctxdxltoplstmt->m_pplRTable)) = lappend_wrapper((* (m_pctxdxltoplstmt->m_pplRTable)), prte);

	// create sequential scan node
	SeqScan *pss = MakeNode(SeqScan);

	pss->scanrelid = iRel;

	Plan *pplan = &(pss->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);
	pplan->nMotionNodes = 0;

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopTS->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// a table scan node must have 2 children: projection list and filter
	GPOS_ASSERT(2 == pdxlnTblScan->UlArity());

	// translate proj list and filter
	CDXLNode *pdxlnPrL = (*pdxlnTblScan)[EdxltsIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnTblScan)[EdxltsIndexFilter];

	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		&dxltrctxbt,	// translate context for the base table
		NULL,			// pdxltrctxLeft and pdxltrctxRight,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	SetParamIds(pplan);

	return (Plan *) pss;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PisFromDXLIndexScan
//
//	@doc:
//		Translates a DXL index scan node into a IndexScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PisFromDXLIndexScan
	(
	const CDXLNode *pdxlnIndexScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalIndexScan *pdxlopIndexScan = CDXLPhysicalIndexScan::PdxlopConvert(pdxlnIndexScan->Pdxlop());

	return PisFromDXLIndexScan(pdxlnIndexScan, pdxlopIndexScan, pdxltrctxOut, pplanParent, false /*fIndexOnlyScan*/);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PiosFromDXLIndexOnlyScan
//
//	@doc:
//		Translates a DXL index only scan node into a IndexOnlyScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PiosFromDXLIndexOnlyScan
	(
	const CDXLNode *pdxlnIndexOnlyScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalIndexOnlyScan *pdxlopIndexOnlyScan = CDXLPhysicalIndexOnlyScan::PdxlopConvert(pdxlnIndexOnlyScan->Pdxlop());

	return PisFromDXLIndexScan(pdxlnIndexOnlyScan, pdxlopIndexOnlyScan, pdxltrctxOut, pplanParent, true /*fIndexOnlyScan*/);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PisFromDXLIndexScan
//
//	@doc:
//		Translates a DXL index scan node into a IndexScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PisFromDXLIndexScan
	(
	const CDXLNode *pdxlnIndexScan,
	CDXLPhysicalIndexScan *pdxlopIndexScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent,
	BOOL fIndexOnlyScan
	)
{
	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;

	const CDXLIndexDescr *pdxlid = NULL;
	if (fIndexOnlyScan)
	{
		pdxlid = pdxlopIndexScan->Pdxlid();
	}

	RangeTblEntry *prte = PrteFromTblDescr(pdxlopIndexScan->Pdxltabdesc(), pdxlid, iRel, &dxltrctxbt);
	GPOS_ASSERT(NULL != prte);
	m_pctxdxltoplstmt->AddRTE(prte);

	IndexScan *pis = NULL;
	if (fIndexOnlyScan)
	{
		pis = MakeNode(IndexOnlyScan);
	}
	else
	{
		pis = MakeNode(IndexScan);
	}

	pis->scan.scanrelid = iRel;

	CMDIdGPDB *pmdidIndex = CMDIdGPDB::PmdidConvert(pdxlopIndexScan->Pdxlid()->Pmdid());
	const IMDIndex *pmdindex = m_pmda->Pmdindex(pmdidIndex);
	Oid oidIndex = pmdidIndex->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidIndex);
	pis->indexid = oidIndex;

	Plan *pplan = &(pis->scan.plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);
	pplan->nMotionNodes = 0;

	// translate operator costs
	TranslatePlanCosts(pdxlopIndexScan->Pdxlprop()->Pdxlopcost(), &(pplan->startup_cost), &(pplan->total_cost), &(pplan->plan_rows), &(pplan->plan_width));

	// an index scan node must have 3 children: projection list, filter and index condition list
	GPOS_ASSERT(3 == pdxlnIndexScan->UlArity());

	// translate proj list and filter
	CDXLNode *pdxlnPrL = (*pdxlnIndexScan)[EdxlisIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnIndexScan)[EdxlisIndexFilter];
	CDXLNode *pdxlnIndexCondList = (*pdxlnIndexScan)[EdxlisIndexCondition];

	TranslateProjListAndFilter(pdxlnPrL, pdxlnFilter, &dxltrctxbt, NULL, &pplan->targetlist, &pplan->qual, pdxltrctxOut, pplan);

	pis->indexorderdir = CTranslatorUtils::Scandirection(pdxlopIndexScan->EdxlScanDirection());

	CMappingColIdVarPlStmt mapcidvarplstmt(m_pmp, &dxltrctxbt, NULL, pdxltrctxOut, m_pctxdxltoplstmt, pplan);

	// translate index condition list
	List *plIndexConditions = NIL;
	List *plIndexStratgey = NIL;
	List *plIndexSubtype = NIL;

	const ULONG ulArity = pdxlnIndexCondList->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnIndexCond = (*pdxlnIndexCondList)[ul];

		// we special case index quals as the scalar identifiers must be
		// translated into vars whose varattno refers to the index column rather
		// than the base table column
		GPOS_ASSERT(2 == pdxlnIndexCond->UlArity());

		CDXLNode *pdxlnLeftScChild = (*pdxlnIndexCond)[0];
		CDXLNode *pdxlnRightScChild = (*pdxlnIndexCond)[1];

		// translate the left scalar child
		CDXLScalarIdent *pdxlopIdent = CDXLScalarIdent::PdxlopConvert(pdxlnLeftScChild->Pdxlop());

		INT iAttno = dxltrctxbt.IAttnoForColId(pdxlopIdent->Pdxlcr()->UlID());
		INT iAttnoOld = iAttno;
		// if this is an indexonlyscan, then we already have the attno referring
		// to the index. Otherwise, we need to perform this mapping here
		if (!fIndexOnlyScan)
		{
			iAttno = 1 + pmdindex->UlPosInKey((ULONG) iAttno - 1);
		}

		Index idxVarno = 1;
		Var *pvarLeft = gpdb::PvarMakeVar
					(
					idxVarno,
					(AttrNumber) iAttno,
					CMDIdGPDB::PmdidConvert(pdxlopIdent->PmdidType())->OidObjectId(),
					-1,	// vartypmod
					0	// varlevelsup
					);
		pvarLeft->varnoold = 1;
		pvarLeft->varoattno =  (AttrNumber) iAttnoOld;

		// translate right scalar child
		Expr *pexprRight = m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnRightScChild, &mapcidvarplstmt);

		// construct the GPDB operator and add to the list of index conditions
		CDXLScalarComp *pdxlopCmp = CDXLScalarComp::PdxlopConvert(pdxlnIndexCond->Pdxlop());
		OpExpr *popexpr = MakeNode(OpExpr);
		popexpr->opno = CMDIdGPDB::PmdidConvert(pdxlopCmp->Pmdid())->OidObjectId();

		const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pdxlopCmp->Pmdid());
		popexpr->opfuncid = CMDIdGPDB::PmdidConvert(pmdscop->PmdidFunc())->OidObjectId();
		popexpr->opresulttype = CMDIdGPDB::PmdidConvert(m_pmda->PtMDType<IMDTypeBool>()->Pmdid())->OidObjectId();
		popexpr->opretset = false;
		popexpr->args = ListMake2(pvarLeft, pexprRight);

		plIndexConditions = gpdb::PlAppendElement(plIndexConditions, popexpr);

		// get the index strategy
		StrategyNumber sn = CTranslatorUtils::Strategynumber(m_pmp, m_pmda, (Expr *) popexpr);
		plIndexStratgey = gpdb::PlAppendInt(plIndexStratgey, sn);

		// TODO: raghav 9th March 2012, the default is 0. Need to investigate if this cannot be 0.
		plIndexSubtype = gpdb::PlAppendOid(plIndexSubtype, OID(0));
	}

	pis->indexqual = plIndexConditions;
	pis->indexqualorig = gpdb::PlCopy(plIndexConditions);
	pis->indexstrategy = plIndexStratgey;
	pis->indexsubtype = plIndexSubtype;
	SetParamIds(pplan);

	return (Plan *) pis;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlimitFromDXLLimit
//
//	@doc:
//		Translates a DXL Limit node into a Limit node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PlimitFromDXLLimit
	(
	const CDXLNode *pdxlnLimit,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalLimit *pdxlopLimit = CDXLPhysicalLimit::PdxlopConvert(pdxlnLimit->Pdxlop());

	// create limit node
	Limit *plimit = MakeNode(Limit);

	Plan *pplan = &(plimit->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopLimit->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	GPOS_ASSERT(4 == pdxlnLimit->UlArity());

	CDXLTranslateContext dxltrctxLeft(m_pmp, false, pdxltrctxOut->PhmColParam());

	// translate proj list
	CDXLNode *pdxlnPrL = (*pdxlnLimit)[EdxllimitIndexProjList];
	CDXLNode *pdxlnChildPlan = (*pdxlnLimit)[EdxllimitIndexChildPlan];
	CDXLNode *pdxlnLimitCount = (*pdxlnLimit)[EdxllimitIndexLimitCount];
	CDXLNode *pdxlnLimitOffset = (*pdxlnLimit)[EdxllimitIndexLimitOffset];

	// NOTE: Limit node has only the left plan while the right plan is left empty
	Plan *pplanLeft = PplFromDXL(pdxlnChildPlan, &dxltrctxLeft, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(&dxltrctxLeft);

	pplan->targetlist = PlTargetListFromProjList
								(
								pdxlnPrL,
								NULL,		// base table translation context
								pdrgpdxltrctx,
								pdxltrctxOut,
								pplan
								);

	pplan->lefttree = pplanLeft;

	if(NULL != pdxlnLimitCount && pdxlnLimitCount->UlArity() >0)
	{
		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt(m_pmp, NULL, pdrgpdxltrctx, pdxltrctxOut, m_pctxdxltoplstmt, pplan);
		Node *pnodeLimitCount = (Node *) m_pdxlsctranslator->PexprFromDXLNodeScalar((*pdxlnLimitCount)[0], &mapcidvarplstmt);
		plimit->limitCount = pnodeLimitCount;
	}

	if(NULL != pdxlnLimitOffset && pdxlnLimitOffset->UlArity() >0)
	{
		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt(m_pmp, NULL, pdrgpdxltrctx, pdxltrctxOut, m_pctxdxltoplstmt, pplan);
		Node *pexprLimitOffset = (Node *) m_pdxlsctranslator->PexprFromDXLNodeScalar((*pdxlnLimitOffset)[0], &mapcidvarplstmt);
		plimit->limitOffset = pexprLimitOffset;
	}

	pplan->nMotionNodes = pplanLeft->nMotionNodes;
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return  (Plan *) plimit;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PhjFromDXLHJ
//
//	@doc:
//		Translates a DXL hash join node into a HashJoin node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PhjFromDXLHJ
	(
	const CDXLNode *pdxlnHJ,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	GPOS_ASSERT(pdxlnHJ->Pdxlop()->Edxlop() == EdxlopPhysicalHashJoin);
	GPOS_ASSERT(pdxlnHJ->UlArity() == EdxlhjIndexSentinel);

	// create hash join node
	HashJoin *phj = MakeNode(HashJoin);

	Join *pj = &(phj->join);
	Plan *pplan = &(pj->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalHashJoin *pdxlopHashJoin = CDXLPhysicalHashJoin::PdxlopConvert(pdxlnHJ->Pdxlop());

	// set join type
	pj->jointype = JtFromEdxljt(pdxlopHashJoin->Edxltype());
	pj->prefetch_inner = true;

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopHashJoin->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate join children
	CDXLNode *pdxlnLeft = (*pdxlnHJ)[EdxlhjIndexHashLeft];
	CDXLNode *pdxlnRight = (*pdxlnHJ)[EdxlhjIndexHashRight];
	CDXLNode *pdxlnPrL = (*pdxlnHJ)[EdxlhjIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnHJ)[EdxlhjIndexFilter];
	CDXLNode *pdxlnJoinFilter = (*pdxlnHJ)[EdxlhjIndexJoinFilter];
	CDXLNode *pdxlnHashCondList = (*pdxlnHJ)[EdxlhjIndexHashCondList];

	CDXLTranslateContext dxltrctxLeft(m_pmp, false, pdxltrctxOut->PhmColParam());
	CDXLTranslateContext dxltrctxRight(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanLeft = PplFromDXL(pdxlnLeft, &dxltrctxLeft, pplan);

	// the right side of the join is the one where the hash phase is done
	Plan *pplanRight = (Plan*) PhhashFromDXL(pdxlnRight, &dxltrctxRight, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxLeft));
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxRight));
	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate join filter
	pj->joinqual = PlQualFromFilter
					(
					pdxlnJoinFilter,
					NULL,			// translate context for the base table
					pdrgpdxltrctx,
					pdxltrctxOut,
					pplan
					);

	// translate hash cond
	List *plHashConditions = NIL;

	BOOL fHasINDFCond = false;

	const ULONG ulArity = pdxlnHashCondList->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnHashCond = (*pdxlnHashCondList)[ul];

		List *plHashCond = PlQualFromScalarCondNode
				(
				pdxlnHashCond,
				NULL,			// base table translation context
				pdrgpdxltrctx,
				pdxltrctxOut,
				pplan
				);

		GPOS_ASSERT(1 == gpdb::UlListLength(plHashCond));

		Expr *pexpr = (Expr *) LInitial(plHashCond);
		if (IsA(pexpr, BoolExpr) && ((BoolExpr *) pexpr)->boolop == NOT_EXPR)
		{
			// INDF test
			GPOS_ASSERT(gpdb::UlListLength(((BoolExpr *) pexpr)->args) == 1 &&
						(IsA((Expr *) LInitial(((BoolExpr *) pexpr)->args), DistinctExpr)));
			fHasINDFCond = true;
		}
		plHashConditions = gpdb::PlConcat(plHashConditions, plHashCond);
	}

	if (!fHasINDFCond)
	{
		// no INDF conditions in the hash condition list
		phj->hashclauses = plHashConditions;
	}
	else
	{
		// hash conditions contain INDF clauses -> extract equality conditions to
		// construct the hash clauses list
		List *plHashClauses = NIL;

		for (ULONG ul = 0; ul < ulArity; ul++)
		{
			CDXLNode *pdxlnHashCond = (*pdxlnHashCondList)[ul];

			// condition can be either a scalar comparison or a NOT DISTINCT FROM expression
			GPOS_ASSERT(EdxlopScalarCmp == pdxlnHashCond->Pdxlop()->Edxlop() ||
						EdxlopScalarBoolExpr == pdxlnHashCond->Pdxlop()->Edxlop());

			if (EdxlopScalarBoolExpr == pdxlnHashCond->Pdxlop()->Edxlop())
			{
				// clause is a NOT DISTINCT FROM check -> extract the distinct comparison node
				GPOS_ASSERT(Edxlnot == CDXLScalarBoolExpr::PdxlopConvert(pdxlnHashCond->Pdxlop())->EdxlBoolType());
				pdxlnHashCond = (*pdxlnHashCond)[0];
				GPOS_ASSERT(EdxlopScalarDistinct == pdxlnHashCond->Pdxlop()->Edxlop());
			}

			CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
														(
														m_pmp,
														NULL,
														pdrgpdxltrctx,
														pdxltrctxOut,
														m_pctxdxltoplstmt,
														pplan
														);

			// translate the DXL scalar or scalar distinct comparison into an equality comparison
			// to store in the hash clauses
			Expr *pexpr2 = (Expr *) m_pdxlsctranslator->PopexprFromDXLNodeScCmp
									(
									pdxlnHashCond,
									&mapcidvarplstmt
									);

			plHashClauses = gpdb::PlAppendElement(plHashClauses, pexpr2);
		}

		phj->hashclauses = plHashClauses;
		phj->hashqualclauses = plHashConditions;
	}

	GPOS_ASSERT(NIL != phj->hashclauses);

	pplan->lefttree = pplanLeft;
	pplan->righttree = pplanRight;
	pplan->nMotionNodes = pplanLeft->nMotionNodes + pplanRight->nMotionNodes;
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return  (Plan *) phj;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanFunctionScanFromDXLTVF
//
//	@doc:
//		Translates a DXL TVF node into a GPDB Function scan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanFunctionScanFromDXLTVF
	(
	const CDXLNode *pdxlnTVF,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	CDXLPhysicalTVF *pdxlopTVF = CDXLPhysicalTVF::PdxlopConvert(pdxlnTVF->Pdxlop());

	// translation context for column mappings
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	// we will add the new range table entry as the last element of the range table
	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;

	dxltrctxbt.SetIdx(iRel);

	// create function scan node
	FunctionScan *pfuncscan = MakeNode(FunctionScan);
	pfuncscan->scan.scanrelid = iRel;
	Plan *pplan = &(pfuncscan->scan.plan);

	RangeTblEntry *prte = PrteFromDXLTVF(pdxlnTVF, pdxltrctxOut, &dxltrctxbt, pplan);
	GPOS_ASSERT(NULL != prte);

	m_pctxdxltoplstmt->AddRTE(prte);

	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);
	pplan->nMotionNodes = 0;

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopTVF->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// a table scan node must have at least 1 child: projection list
	GPOS_ASSERT(1 <= pdxlnTVF->UlArity());

	CDXLNode *pdxlnPrL = (*pdxlnTVF)[EdxltsIndexProjList];

	// translate proj list
	List *plTargetList = PlTargetListFromProjList
						(
						pdxlnPrL,
						&dxltrctxbt,
						NULL,
						pdxltrctxOut,
						pplan
						);

	pplan->targetlist = plTargetList;

	ListCell *plcTe = NULL;

	ForEach (plcTe, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTe);
		OID oidType = gpdb::OidExprType((Node*) pte->expr);
		GPOS_ASSERT(InvalidOid != oidType);

		INT typMod = gpdb::IExprTypeMod((Node*) pte->expr);

		prte->funccoltypes = gpdb::PlAppendOid(prte->funccoltypes, oidType);
		prte->funccoltypmods = gpdb::PlAppendInt(prte->funccoltypmods, typMod);
	}

	SetParamIds(pplan);

	return (Plan *) pfuncscan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PrteFromDXLTVF
//
//	@doc:
//		Create a range table entry from a CDXLPhysicalTVF node
//
//---------------------------------------------------------------------------
RangeTblEntry *
CTranslatorDXLToPlStmt::PrteFromDXLTVF
	(
	const CDXLNode *pdxlnTVF,
	CDXLTranslateContext *pdxltrctxOut,
	CDXLTranslateContextBaseTable *pdxltrctxbt,
	Plan *pplanParent
	)
{
	CDXLPhysicalTVF *pdxlop = CDXLPhysicalTVF::PdxlopConvert(pdxlnTVF->Pdxlop());

	RangeTblEntry *prte = MakeNode(RangeTblEntry);
	prte->rtekind = RTE_FUNCTION;

	FuncExpr *pfuncexpr = MakeNode(FuncExpr);

	pfuncexpr->funcid = CMDIdGPDB::PmdidConvert(pdxlop->Pmdid())->OidObjectId();
	pfuncexpr->funcretset = true;
	// this is a function call, as opposed to a cast
	pfuncexpr->funcformat = COERCE_EXPLICIT_CALL;

	CMDIdGPDB *pmdidFunc = New(m_pmp) CMDIdGPDB(pfuncexpr->funcid);
	const IMDFunction *pmdfunc = m_pmda->Pmdfunc(pmdidFunc);
	pmdidFunc->Release();

	pfuncexpr->funcresulttype = CMDIdGPDB::PmdidConvert(pmdfunc->PmdidTypeResult())->OidObjectId();

	Alias *palias = MakeNode(Alias);
	palias->colnames = NIL;

	// get function alias
	palias->aliasname = CTranslatorUtils::SzFromWsz(pdxlop->Pstr()->Wsz());

	// project list
	CDXLNode *pdxlnPrL = (*pdxlnTVF)[EdxltsIndexProjList];

	// get column names
	const ULONG ulCols = pdxlnPrL->UlArity();
	for (ULONG ul = 0; ul < ulCols; ul++)
	{
		CDXLNode *pdxlnPrElem = (*pdxlnPrL)[ul];
		CDXLScalarProjElem *pdxlopPrEl = CDXLScalarProjElem::PdxlopConvert(pdxlnPrElem->Pdxlop());

		CHAR *szColName = CTranslatorUtils::SzFromWsz(pdxlopPrEl->PmdnameAlias()->Pstr()->Wsz());

		Value *pvalColName = gpdb::PvalMakeString(szColName);
		palias->colnames = gpdb::PlAppendElement(palias->colnames, pvalColName);

		// save mapping col id -> index in translate context
		(void) pdxltrctxbt->FInsertMapping(pdxlopPrEl->UlId(), ul+1 /*iAttno*/);
	}

	// function arguments
	const ULONG ulChildren = pdxlnTVF->UlArity();
	for (ULONG ul = 1; ul < ulChildren; ++ul)
	{
		CDXLNode *pdxlnFuncArg = (*pdxlnTVF)[ul];

		CMappingColIdVarPlStmt mapcidvarplstmt
									(
									m_pmp,
									pdxltrctxbt,
									NULL,
									pdxltrctxOut,
									m_pctxdxltoplstmt,
									pplanParent
									);

		Expr *pexprFuncArg = m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnFuncArg, &mapcidvarplstmt);
		pfuncexpr->args = gpdb::PlAppendElement(pfuncexpr->args, pexprFuncArg);
	}

	prte->funcexpr = (Node *)pfuncexpr;
	prte->inFromCl = true;
	prte->eref = palias;

	return prte;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PnljFromDXLNLJ
//
//	@doc:
//		Translates a DXL nested loop join node into a NestLoop plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PnljFromDXLNLJ
	(
	const CDXLNode *pdxlnNLJ,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	GPOS_ASSERT(pdxlnNLJ->Pdxlop()->Edxlop() == EdxlopPhysicalNLJoin);
	GPOS_ASSERT(pdxlnNLJ->UlArity() == EdxlnljIndexSentinel);

	// create hash join node
	NestLoop *pnlj = MakeNode(NestLoop);

	Join *pj = &(pnlj->join);
	Plan *pplan = &(pj->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalNLJoin *pdxlnlj = CDXLPhysicalNLJoin::PdxlConvert(pdxlnNLJ->Pdxlop());

	// set join type
	pj->jointype = JtFromEdxljt(pdxlnlj->Edxltype());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlnlj->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate join children
	CDXLNode *pdxlnLeft = (*pdxlnNLJ)[EdxlnljIndexLeftChild];
	CDXLNode *pdxlnRight = (*pdxlnNLJ)[EdxlnljIndexRightChild];

	CDXLNode *pdxlnPrL = (*pdxlnNLJ)[EdxlnljIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnNLJ)[EdxlnljIndexFilter];
	CDXLNode *pdxlnJoinFilter = (*pdxlnNLJ)[EdxlnljIndexJoinFilter];

	CDXLTranslateContext dxltrctxLeft(m_pmp, false, pdxltrctxOut->PhmColParam());
	CDXLTranslateContext dxltrctxRight(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanLeft = PplFromDXL(pdxlnLeft, &dxltrctxLeft, pplan);
	Plan *pplanRight = PplFromDXL(pdxlnRight, &dxltrctxRight, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(&dxltrctxLeft);
	pdrgpdxltrctx->Append(&dxltrctxRight);

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate join condition
	pj->joinqual = PlQualFromFilter
					(
					pdxlnJoinFilter,
					NULL,			// translate context for the base table
					pdrgpdxltrctx,
					pdxltrctxOut,
					pplan
					);

	pplan->lefttree = pplanLeft;
	pplan->righttree = pplanRight;
	pplan->nMotionNodes = pplanLeft->nMotionNodes + pplanRight->nMotionNodes;
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return  (Plan *) pnlj;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PmjFromDXLMJ
//
//	@doc:
//		Translates a DXL merge join node into a MergeJoin node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PmjFromDXLMJ
	(
	const CDXLNode *pdxlnMJ,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	GPOS_ASSERT(pdxlnMJ->Pdxlop()->Edxlop() == EdxlopPhysicalMergeJoin);
	GPOS_ASSERT(pdxlnMJ->UlArity() == EdxlmjIndexSentinel);

	// create merge join node
	MergeJoin *pmj = MakeNode(MergeJoin);

	Join *pj = &(pmj->join);
	Plan *pplan = &(pj->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalMergeJoin *pdxlopMergeJoin = CDXLPhysicalMergeJoin::PdxlopConvert(pdxlnMJ->Pdxlop());

	// set join type
	pj->jointype = JtFromEdxljt(pdxlopMergeJoin->Edxltype());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopMergeJoin->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate join children
	CDXLNode *pdxlnLeft = (*pdxlnMJ)[EdxlmjIndexLeftChild];
	CDXLNode *pdxlnRight = (*pdxlnMJ)[EdxlmjIndexRightChild];

	CDXLNode *pdxlnPrL = (*pdxlnMJ)[EdxlmjIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnMJ)[EdxlmjIndexFilter];
	CDXLNode *pdxlnJoinFilter = (*pdxlnMJ)[EdxlmjIndexJoinFilter];
	CDXLNode *pdxlnMergeCondList = (*pdxlnMJ)[EdxlmjIndexMergeCondList];

	CDXLTranslateContext dxltrctxLeft(m_pmp, false, pdxltrctxOut->PhmColParam());
	CDXLTranslateContext dxltrctxRight(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanLeft = PplFromDXL(pdxlnLeft, &dxltrctxLeft, pplan);
	Plan *pplanRight = PplFromDXL(pdxlnRight, &dxltrctxRight, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxLeft));
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxRight));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate join filter
	pj->joinqual = PlQualFromFilter
					(
					pdxlnJoinFilter,
					NULL,			// translate context for the base table
					pdrgpdxltrctx,
					pdxltrctxOut,
					pplan
					);

	// translate merge cond
	List *plMergeConditions = NIL;

	const ULONG ulArity = pdxlnMergeCondList->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnMergeCond = (*pdxlnMergeCondList)[ul];
		List *plMergeCond = PlQualFromScalarCondNode
				(
				pdxlnMergeCond,
				NULL,			// base table translation context
				pdrgpdxltrctx,
				pdxltrctxOut,
				pplan
				);

		GPOS_ASSERT(1 == gpdb::UlListLength(plMergeCond));
		plMergeConditions = gpdb::PlConcat(plMergeConditions, plMergeCond);
	}

	GPOS_ASSERT(NIL != plMergeConditions);

	pmj->mergeclauses = plMergeConditions;

	pplan->lefttree = pplanLeft;
	pplan->righttree = pplanRight;
	pplan->nMotionNodes = pplanLeft->nMotionNodes + pplanRight->nMotionNodes;
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return  (Plan *) pmj;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PhhashFromDXL
//
//	@doc:
//		Translates a DXL physical operator node into a Hash node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PhhashFromDXL
	(
	const CDXLNode *pdxln,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	Hash *ph = MakeNode(Hash);

	Plan *pplan = &(ph->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	// translate dxl node
	CDXLTranslateContext dxltrctx(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanLeft = PplFromDXL(pdxln, &dxltrctx, pplan);

	GPOS_ASSERT(0 < pdxln->UlArity());

	// create a reference to each entry in the child project list to create the target list of
	// the hash node
	CDXLNode *pdxlnPrL = (*pdxln)[0];
	List *plTargetList = PlTargetListForHashNode(pdxlnPrL, &dxltrctx, pdxltrctxOut);

	// copy costs from child node; the startup cost for the hash node is the total cost
	// of the child plan, see make_hash in createplan.c
	pplan->startup_cost = pplanLeft->total_cost;
	pplan->total_cost = pplanLeft->total_cost;
	pplan->plan_rows = pplanLeft->plan_rows;
	pplan->plan_width = pplanLeft->plan_width;

	pplan->targetlist = plTargetList;
	pplan->lefttree = pplanLeft;
	pplan->righttree = NULL;
	pplan->nMotionNodes = pplanLeft->nMotionNodes;
	pplan->qual = NIL;
	ph->rescannable = false;

	SetParamIds(pplan);

	return (Plan *) ph;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanTranslateDXLMotion
//
//	@doc:
//		Translate DXL motion node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanTranslateDXLMotion
	(
	const CDXLNode *pdxlnMotion,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	CDXLPhysicalRedistributeMotion *pdxlopMotion = CDXLPhysicalRedistributeMotion::PdxlopConvert(pdxlnMotion->Pdxlop());
	if (CDXLPhysicalRedistributeMotion::PdxlopConvert(pdxlopMotion)->FDuplicateSensitive())
	{
		return PplanResultHashFilters(pdxlnMotion, pdxltrctxOut, pplanParent);
	}
	
	return PplanMotionFromDXLMotion(pdxlnMotion, pdxltrctxOut, pplanParent);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion
//
//	@doc:
//		Translate DXL motion node into GPDB Motion plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanMotionFromDXLMotion
	(
	const CDXLNode *pdxlnMotion,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	CDXLPhysicalMotion *pdxlopMotion = CDXLPhysicalMotion::PdxlopConvert(pdxlnMotion->Pdxlop());

	// create motion node
	Motion *pmotion = MakeNode(Motion);

	Plan *pplan = &(pmotion->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopMotion->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	CDXLNode *pdxlnPrL = (*pdxlnMotion)[EdxlgmIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnMotion)[EdxlgmIndexFilter];
	CDXLNode *pdxlnSortColList = (*pdxlnMotion)[EdxlgmIndexSortColList];

	// translate motion child
	// child node is in the same position in broadcast and gather motion nodes
	// but different in redistribute motion nodes

	ULONG ulChildIndex = (pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRedistribute) ||
						(pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRoutedDistribute)?
								(ULONG) EdxlrmIndexChild : (ULONG) EdxlgmIndexChild;

	CDXLNode *pdxlnChild = (*pdxlnMotion)[ulChildIndex];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate sorting info
	ULONG ulNumSortCols = pdxlnSortColList->UlArity();
	if (0 < ulNumSortCols)
	{
		pmotion->sendSorted = true;
		pmotion->numSortCols = ulNumSortCols;
		pmotion->sortColIdx = (AttrNumber *) gpdb::GPDBAlloc(ulNumSortCols * sizeof(AttrNumber));
		pmotion->sortOperators = (Oid *) gpdb::GPDBAlloc(ulNumSortCols * sizeof(Oid));

		TranslateSortCols(pdxlnSortColList, pdxltrctxOut, pmotion->sortColIdx, pmotion->sortOperators);
	}
	else
	{
		// not a sorting motion
		pmotion->sendSorted = false;
		pmotion->numSortCols = 0;
		pmotion->sortColIdx = NULL;
		pmotion->sortOperators = NULL;
	}

	if (pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRedistribute ||
		pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRoutedDistribute ||
		pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRandom)
	{
		// translate hash expr list
		CDXLNode *pdxlnHashExprList = (*pdxlnMotion)[EdxlrmIndexHashExprList];
		List *plHashExpr = NIL;
		List *plHashExprTypes = NIL;

		if (pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRedistribute ||
			pdxlopMotion->Edxlop() == EdxlopPhysicalMotionRoutedDistribute)
		{
			TranslateHashExprList
				(
				pdxlnHashExprList,
				&dxltrctxChild,
				&plHashExpr,
				&plHashExprTypes,
				pdxltrctxOut,
				pplan
				);
		}
		GPOS_ASSERT(gpdb::UlListLength(plHashExpr) == gpdb::UlListLength(plHashExprTypes));

		pmotion->hashExpr = plHashExpr;
		pmotion->hashDataTypes = plHashExprTypes;
	}

	// cleanup
	pdrgpdxltrctx->Release();

	// create flow for child node to distinguish between singleton flows and all-segment flows
	Flow *pflow = MakeNode(Flow);

	const DrgPi *pdrgpiInputSegmentIds = pdxlopMotion->PdrgpiInputSegIds();

	if (1 == pdrgpiInputSegmentIds->UlLength())
	{
		// only one sender - child has a singleton flow
		pflow->flotype = FLOW_SINGLETON;
		pflow->segindex = *((*pdrgpiInputSegmentIds)[0]);
	}
	else
	{
		pflow->flotype = FLOW_UNDEFINED;
	}

	pplanChild->flow = pflow;

	pmotion->motionID = m_pctxdxltoplstmt->UlNextMotionId();
	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes + 1;

	// translate properties of the specific type of motion operator

	switch (pdxlopMotion->Edxlop())
	{
		case EdxlopPhysicalMotionGather:
		{
			pmotion->motionType = MOTIONTYPE_FIXED;
			// get segment id
			INT iSegId = CDXLPhysicalGatherMotion::PdxlopConvert(pdxlopMotion)->IOutputSegIdx();
			pmotion->numOutputSegs = 1;
			pmotion->outputSegIdx = (INT *) gpdb::GPDBAlloc(sizeof(INT));
			*(pmotion->outputSegIdx) = iSegId;
			break;
		}
		case EdxlopPhysicalMotionRedistribute:
		case EdxlopPhysicalMotionRandom:
		{
			pmotion->motionType = MOTIONTYPE_HASH;
			// translate output segment ids
			const DrgPi *pdrgpiSegIds = CDXLPhysicalMotion::PdxlopConvert(pdxlopMotion)->PdrgpiOutputSegIds();

			GPOS_ASSERT(NULL != pdrgpiSegIds && 0 < pdrgpiSegIds->UlLength());
			ULONG ulSegIdCount = pdrgpiSegIds->UlLength();
			pmotion->outputSegIdx = (INT *) gpdb::GPDBAlloc (ulSegIdCount * sizeof(INT));
			pmotion->numOutputSegs = ulSegIdCount;

			for(ULONG ul = 0; ul < ulSegIdCount; ul++)
			{
				INT iSegId = *((*pdrgpiSegIds)[ul]);
				pmotion->outputSegIdx[ul] = iSegId;
			}

			break;
		}
		case EdxlopPhysicalMotionBroadcast:
		{
			pmotion->motionType = MOTIONTYPE_FIXED;
			pmotion->numOutputSegs = 0;
			pmotion->outputSegIdx = NULL;
			break;
		}
		case EdxlopPhysicalMotionRoutedDistribute:
		{
			pmotion->motionType = MOTIONTYPE_EXPLICIT;
			pmotion->numOutputSegs = 0;
			pmotion->outputSegIdx = NULL;
			ULONG ulSegIdCol = CDXLPhysicalRoutedDistributeMotion::PdxlopConvert(pdxlopMotion)->UlSegmentIdCol();
			const TargetEntry *pteSortCol = dxltrctxChild.Pte(ulSegIdCol);
			pmotion->segidColIdx = pteSortCol->resno;

			break;
			
		}
		default:
			GPOS_ASSERT(!"Unrecognized Motion operator");
			return NULL;
	}

	SetParamIds(pplan);

	return (Plan *) pmotion;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanResultHashFilters
//
//	@doc:
//		Translate DXL duplicate sensitive redistribute motion node into 
//		GPDB result node with hash filters
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanResultHashFilters
	(
	const CDXLNode *pdxlnMotion,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create motion node
	Result *presult = MakeNode(Result);

	Plan *pplan = &(presult->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalRedistributeMotion *pdxlopMotion = CDXLPhysicalRedistributeMotion::PdxlopConvert(pdxlnMotion->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopMotion->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	CDXLNode *pdxlnPrL = (*pdxlnMotion)[EdxlrmIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnMotion)[EdxlrmIndexFilter];
	CDXLNode *pdxlnSortColList = (*pdxlnMotion)[EdxlrmIndexSortColList];
	CDXLNode *pdxlnChild = (*pdxlnMotion)[EdxlrmIndexChild];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate hash expr list
	presult->hashFilter = true;
	CDXLNode *pdxlnHashExprList = (*pdxlnMotion)[EdxlrmIndexHashExprList];
	const ULONG ulLength = pdxlnHashExprList->UlArity();
	GPOS_ASSERT(0 < ulLength);
	
	for (ULONG ul = 0; ul < ulLength; ul++)
	{
		CDXLNode *pdxlnHashExpr = (*pdxlnHashExprList)[ul];
		CDXLNode *pdxlnScIdent = (*pdxlnHashExpr)[0];
		ULONG ulColId = CDXLScalarIdent::PdxlopConvert(pdxlnScIdent->Pdxlop())->Pdxlcr()->UlID();
		presult->hashList = gpdb::PlAppendInt(presult->hashList, pdxltrctxOut->Pte(ulColId)->resno);
	}
			
	// cleanup
	pdrgpdxltrctx->Release();

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	SetParamIds(pplan);

	return (Plan *) presult;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PaggFromDXLAgg
//
//	@doc:
//		Translate DXL aggregate node into GPDB Agg plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PaggFromDXLAgg
	(
	const CDXLNode *pdxlnAgg,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create aggregate plan node
	Agg *pagg = MakeNode(Agg);

	Plan *pplan = &(pagg->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalAgg *pdxlopAgg = CDXLPhysicalAgg::PdxlopConvert(pdxlnAgg->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopAgg->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate agg child
	CDXLNode *pdxlnChild = (*pdxlnAgg)[EdxlaggIndexChild];

	CDXLNode *pdxlnPrL = (*pdxlnAgg)[EdxlaggIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnAgg)[EdxlaggIndexFilter];

	CDXLTranslateContext dxltrctxChild(m_pmp, true, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,			// pdxltrctxRight,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	// translate aggregation strategy
	switch (pdxlopAgg->Edxlaggstr())
	{
		case EdxlaggstrategyPlain:
			pagg->aggstrategy = AGG_PLAIN;
			break;
		case EdxlaggstrategySorted:
			pagg->aggstrategy = AGG_SORTED;
			break;
		case EdxlaggstrategyHashed:
			pagg->aggstrategy = AGG_HASHED;
			break;
		default:
			GPOS_ASSERT(!"Invalid aggregation strategy");
	}

	// translate grouping cols
	const DrgPul *pdrpulGroupingCols = pdxlopAgg->PdrgpulGroupingCols();
	pagg->numCols = pdrpulGroupingCols->UlLength();
	if (pagg->numCols > 0)
	{
		pagg->grpColIdx = (AttrNumber *) gpdb::GPDBAlloc(pagg->numCols * sizeof(AttrNumber));
	}
	else
	{
		pagg->grpColIdx = NULL;
	}

	const ULONG ulLen = pdrpulGroupingCols->UlLength();
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		ULONG ulGroupingColId = *((*pdrpulGroupingCols)[ul]);
		const TargetEntry *pteGroupingCol = dxltrctxChild.Pte(ulGroupingColId);
		GPOS_ASSERT(NULL != pteGroupingCol);
		pagg->grpColIdx[ul] = pteGroupingCol->resno;
	}
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) pagg;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PwindowFromDXLWindow
//
//	@doc:
//		Translate DXL window node into GPDB window plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PwindowFromDXLWindow
	(
	const CDXLNode *pdxlnWindow,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create a window plan node
	Window *pwindow = MakeNode(Window);

	Plan *pplan = &(pwindow->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalWindow *pdxlopWindow = CDXLPhysicalWindow::PdxlopConvert(pdxlnWindow->Pdxlop());

	// translate the operator costs
	TranslatePlanCosts
		(
		pdxlopWindow->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate children
	CDXLNode *pdxlnChild = (*pdxlnWindow)[EdxlwindowIndexChild];
	CDXLNode *pdxlnPrL = (*pdxlnWindow)[EdxlwindowIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnWindow)[EdxlwindowIndexFilter];

	CDXLTranslateContext dxltrctxChild(m_pmp, true, pdxltrctxOut->PhmColParam());
	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,			// pdxltrctxRight,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	// translate partition columns
	const DrgPul *pdrpulPartCols = pdxlopWindow->PrgpulPartCols();
	pwindow->numPartCols = pdrpulPartCols->UlLength();
	pwindow->partColIdx = NULL;

	if (pwindow->numPartCols > 0)
	{
		pwindow->partColIdx = (AttrNumber *) gpdb::GPDBAlloc(pwindow->numPartCols * sizeof(AttrNumber));
	}

	const ULONG ulPartCols = pdrpulPartCols->UlLength();
	for (ULONG ul = 0; ul < ulPartCols; ul++)
	{
		ULONG ulPartColId = *((*pdrpulPartCols)[ul]);
		const TargetEntry *ptePartCol = dxltrctxChild.Pte(ulPartColId);
		GPOS_ASSERT(NULL != ptePartCol);
		pwindow->partColIdx[ul] = ptePartCol->resno;
	}

	// translate window keys
	pwindow->windowKeys = NIL;
	const ULONG ulSize = pdxlopWindow->UlWindowKeys();
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		WindowKey *pwindowkey = MakeNode(WindowKey);

		// translate the sorting columns used in the window key
		const CDXLWindowKey *pdxlwindowkey = pdxlopWindow->PdxlWindowKey(ul);
		const CDXLNode *pdxlnSortColList = pdxlwindowkey->PdxlnSortColList();

		const ULONG ulNumCols = pdxlnSortColList->UlArity();
		pwindowkey->numSortCols = ulNumCols;
		pwindowkey->sortColIdx = (AttrNumber *) gpdb::GPDBAlloc(ulNumCols * sizeof(AttrNumber));
		pwindowkey->sortOperators = (Oid *) gpdb::GPDBAlloc(ulNumCols * sizeof(Oid));
		TranslateSortCols(pdxlnSortColList, &dxltrctxChild, pwindowkey->sortColIdx, pwindowkey->sortOperators);

		// translate the window frame specified in the window key
		pwindowkey->frame = NULL;
		if (NULL != pdxlwindowkey->Pdxlwf())
		{
			pwindowkey->frame = Pwindowframe(pdxlwindowkey->Pdxlwf(), &dxltrctxChild, pdxltrctxOut, pplan);
		}
		pwindow->windowKeys = gpdb::PlAppendElement(pwindow->windowKeys, pwindowkey);
	}

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) pwindow;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::Pwindowframe
//
//	@doc:
//		Translate the DXL window frame into GPDB Window frame node
//
//---------------------------------------------------------------------------
WindowFrame *
CTranslatorDXLToPlStmt::Pwindowframe
	(
	const CDXLWindowFrame *pdxlwf,
	const CDXLTranslateContext *pdxltrctxChild,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplan
	)
{
	WindowFrame *pwindowframe = MakeNode(WindowFrame);

	if (EdxlfsRow == pdxlwf->Edxlfs())
	{
		pwindowframe->is_rows = true;
	}
	else
	{
		pwindowframe->is_rows = false;
	}
	pwindowframe->is_between = true;

	pwindowframe->exclude = CTranslatorUtils::Windowexclusion(pdxlwf->Edxlfes());

	// translate the CDXLNodes representing the leading and trailing edge
	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(pdxltrctxChild);

	CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
												(
												m_pmp,
												NULL,
												pdrgpdxltrctx,
												pdxltrctxOut,
												m_pctxdxltoplstmt,
												pplan
												);

	CDXLNode *pdxlnLead = pdxlwf->PdxlnLeading();
	pwindowframe->lead = MakeNode(WindowFrameEdge);
	pwindowframe->lead->kind = CTranslatorUtils::Windowboundkind(CDXLScalarWindowFrameEdge::PdxlopConvert(pdxlnLead->Pdxlop())->Edxlfb());
	pwindowframe->lead->val = NULL;
	if (0 != pdxlnLead->UlArity())
	{
		pwindowframe->lead->val = (Node*) m_pdxlsctranslator->PexprFromDXLNodeScalar((*pdxlnLead)[0], &mapcidvarplstmt);
	}


	CDXLNode *pdxlnTrail = pdxlwf->PdxlnTrailing();
	pwindowframe->trail = MakeNode(WindowFrameEdge);
	pwindowframe->trail->kind = CTranslatorUtils::Windowboundkind(CDXLScalarWindowFrameEdge::PdxlopConvert(pdxlnTrail->Pdxlop())->Edxlfb());
	pwindowframe->trail->val = NULL;
	if (0 != pdxlnTrail->UlArity())
	{
		pwindowframe->trail->val = (Node*) m_pdxlsctranslator->PexprFromDXLNodeScalar((*pdxlnTrail)[0], &mapcidvarplstmt);
	}

	// cleanup
	pdrgpdxltrctx->Release();

	return pwindowframe;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PsortFromDXLSort
//
//	@doc:
//		Translate DXL sort node into GPDB Sort plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PsortFromDXLSort
	(
	const CDXLNode *pdxlnSort,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create sort plan node
	Sort *psort = MakeNode(Sort);

	Plan *pplan = &(psort->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalSort *pdxlopSort = CDXLPhysicalSort::PdxlopConvert(pdxlnSort->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopSort->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate sort child
	CDXLNode *pdxlnChild = (*pdxlnSort)[EdxlsortIndexChild];
	CDXLNode *pdxlnPrL = (*pdxlnSort)[EdxlsortIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnSort)[EdxlsortIndexFilter];
	CDXLNode *pdxlnLimitCount = (*pdxlnSort)[EdxlsortIndexLimitCount];
	CDXLNode *pdxlnLimitOffset = (*pdxlnSort)[EdxlsortIndexLimitOffset];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	// set sorting info
	psort->noduplicates = pdxlopSort->FDiscardDuplicates();

	// translate sorting columns
	// TODO: antovl - Jan 19, 2011; GPDB only supports nullsLast right now, so nullsFirst is unused
	psort->nullsFirst = NULL;

	const CDXLNode *pdxlnSortColList = (*pdxlnSort)[EdxlsortIndexSortColList];

	const ULONG ulNumCols = pdxlnSortColList->UlArity();
	psort->numCols = ulNumCols;
	psort->sortColIdx = (AttrNumber *) gpdb::GPDBAlloc(ulNumCols * sizeof(AttrNumber));
	psort->sortOperators = (Oid *) gpdb::GPDBAlloc(ulNumCols * sizeof(Oid));

	TranslateSortCols(pdxlnSortColList, &dxltrctxChild, psort->sortColIdx, psort->sortOperators);

	// translate limit information
	if (0 < pdxlnLimitCount->UlArity())
	{
		GPOS_ASSERT(1 == pdxlnLimitCount->UlArity());
		const CDXLNode *pdxlnLimitCountExpr = (*pdxlnLimitCount)[0];
		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
																(
																m_pmp,
																NULL,
																pdrgpdxltrctx,
																pdxltrctxOut,
																m_pctxdxltoplstmt,
																pplan
																);

		psort->limitCount = (Node *) m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnLimitCountExpr, &mapcidvarplstmt);
	}

	if (0 < pdxlnLimitOffset->UlArity())
	{
		GPOS_ASSERT(1 == pdxlnLimitOffset->UlArity());
		const CDXLNode *pdxlnLimitOffsetExpr = (*pdxlnLimitOffset)[0];

		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
																(
																m_pmp,
																NULL,
																pdrgpdxltrctx,
																pdxltrctxOut,
																m_pctxdxltoplstmt,
																pplan
																);

		psort->limitOffset = (Node *) m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnLimitOffsetExpr, &mapcidvarplstmt);
	}
	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) psort;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PsubqscanFromDXLSubqScan
//
//	@doc:
//		Translate DXL subquery scan node into GPDB SubqueryScan plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PsubqscanFromDXLSubqScan
	(
	const CDXLNode *pdxlnSubqScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create sort plan node
	SubqueryScan *psubqscan = MakeNode(SubqueryScan);

	Plan *pplan = &(psubqscan->scan.plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalSubqueryScan *pdxlopSubqscan = CDXLPhysicalSubqueryScan::PdxlopConvert(pdxlnSubqScan->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopSubqscan->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate subplan
	CDXLNode *pdxlnChild = (*pdxlnSubqScan)[EdxlsubqscanIndexChild];
	CDXLNode *pdxlnPrL = (*pdxlnSubqScan)[EdxlsubqscanIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnSubqScan)[EdxlsubqscanIndexFilter];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	// create an rtable entry for the subquery scan
	RangeTblEntry *prte = MakeNode(RangeTblEntry);
	prte->rtekind = RTE_SUBQUERY;

	Alias *palias = MakeNode(Alias);
	palias->colnames = NIL;

	// get table alias
	palias->aliasname = CTranslatorUtils::SzFromWsz(pdxlopSubqscan->Pmdname()->Pstr()->Wsz());

	// get column names from child project list
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;
	(psubqscan->scan).scanrelid = iRel;
	dxltrctxbt.SetIdx(iRel);

	ListCell *plcTE = NULL;

	CDXLNode *pdxlnChildProjList = (*pdxlnChild)[0];

	ULONG ul = 0;

	ForEach (plcTE, pplanChild->targetlist)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);

		// non-system attribute
		CHAR *szColName = PStrDup(pte->resname);
		Value *pvalColName = gpdb::PvalMakeString(szColName);
		palias->colnames = gpdb::PlAppendElement(palias->colnames, pvalColName);

		// get corresponding child project element
		CDXLScalarProjElem *pdxlopPrel = CDXLScalarProjElem::PdxlopConvert((*pdxlnChildProjList)[ul]->Pdxlop());

		// save mapping col id -> index in translate context
		(void) dxltrctxbt.FInsertMapping(pdxlopPrel->UlId(), pte->resno);
		ul++;
	}

	prte->eref = palias;

	// add range table entry for the subquery to the list
	m_pctxdxltoplstmt->AddRTE(prte);

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		&dxltrctxbt,		// translate context for the base table
		NULL,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	psubqscan->subplan = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	SetParamIds(pplan);
	return (Plan *) psubqscan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PresultFromDXLResult
//
//	@doc:
//		Translate DXL result node into GPDB result plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PresultFromDXLResult
	(
	const CDXLNode *pdxlnResult,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create result plan node
	Result *presult = MakeNode(Result);

	Plan *pplan = &(presult->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalResult *pdxlopResult = CDXLPhysicalResult::PdxlopConvert(pdxlnResult->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopResult->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	pplan->nMotionNodes = 0;

	CDXLNode *pdxlnChild = NULL;
	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	if (pdxlnResult->UlArity() - 1 == EdxlresultIndexChild)
	{
		// translate child plan
		pdxlnChild = (*pdxlnResult)[EdxlresultIndexChild];

		Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

		GPOS_ASSERT(NULL != pplanChild && "child plan cannot be NULL");

		presult->plan.lefttree = pplanChild;

		pplan->nMotionNodes = pplanChild->nMotionNodes;
	}

	CDXLNode *pdxlnPrL = (*pdxlnResult)[EdxlresultIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnResult)[EdxlresultIndexFilter];
	CDXLNode *pdxlnOneTimeFilter = (*pdxlnResult)[EdxlresultIndexOneTimeFilter];

	List *plQuals = NULL;

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,		// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&plQuals,
		pdxltrctxOut,
		pplan
		);

	// translate one time filter
	List *plOneTimeQuals = PlQualFromFilter
							(
							pdxlnOneTimeFilter,
							NULL,			// base table translation context
							pdrgpdxltrctx,
							pdxltrctxOut,
							pplan
							);

	pplan->qual = plQuals;

	presult->resconstantqual = (Node *) plOneTimeQuals;

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) presult;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PappendFromDXLAppend
//
//	@doc:
//		Translate DXL append node into GPDB Append plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PappendFromDXLAppend
	(
	const CDXLNode *pdxlnAppend,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create append plan node
	Append *pappend = MakeNode(Append);

	Plan *pplan = &(pappend->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalAppend *pdxlopAppend = CDXLPhysicalAppend::PdxlopConvert(pdxlnAppend->Pdxlop());

	pappend->isTarget = pdxlopAppend->FIsTarget();
	pappend->isZapped = pdxlopAppend->FIsZapped();

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopAppend->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);


	// translate first child
	const ULONG ulArity = pdxlnAppend->UlArity();
	GPOS_ASSERT(EdxlappendIndexFirstChild < ulArity);

	CDXLNode *pdxlnFirstChild = (*pdxlnAppend)[EdxlappendIndexFirstChild];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanFirstChild = PplFromDXL(pdxlnFirstChild, &dxltrctxChild, pplan);

	GPOS_ASSERT(NULL != pplanFirstChild && "child plan cannot be NULL");

	pappend->appendplans = gpdb::PlAppendElement(pappend->appendplans, pplanFirstChild);
	pplan->nMotionNodes = pplanFirstChild->nMotionNodes;

	CDXLNode *pdxlnPrL = (*pdxlnAppend)[EdxlappendIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnAppend)[EdxlappendIndexFilter];

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	// translate the rest of the children
	for (ULONG ul = EdxlappendIndexFirstChild + 1; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnChild = (*pdxlnAppend)[ul];

		Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

		GPOS_ASSERT(NULL != pplanChild && "child plan cannot be NULL");

		pappend->appendplans = gpdb::PlAppendElement(pappend->appendplans, pplanChild);
		pplan->nMotionNodes += pplanChild->nMotionNodes;
	}

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) pappend;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PmatFromDXLMaterialize
//
//	@doc:
//		Translate DXL materialize node into GPDB Material plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PmatFromDXLMaterialize
	(
	const CDXLNode *pdxlnMaterialize,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create materialize plan node
	Material *pmat = MakeNode(Material);

	Plan *pplan = &(pmat->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalMaterialize *pdxlopMat = CDXLPhysicalMaterialize::PdxlopConvert(pdxlnMaterialize->Pdxlop());

	pmat->cdb_strict = pdxlopMat->FEager();

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopMat->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate materialize child
	CDXLNode *pdxlnChild = (*pdxlnMaterialize)[EdxlmatIndexChild];

	CDXLNode *pdxlnPrL = (*pdxlnMaterialize)[EdxlmatIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnMaterialize)[EdxlmatIndexFilter];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	// set spooling info
	if (pdxlopMat->FSpooling())
	{
		pmat->share_id = pdxlopMat->UlSpoolId();
		pmat->driver_slice = pdxlopMat->IExecutorSlice();
		pmat->nsharer_xslice = pdxlopMat->UlConsumerSlices();
		pmat->share_type = (0 < pdxlopMat->UlConsumerSlices()) ?
							SHARE_MATERIAL_XSLICE : SHARE_MATERIAL;
	}
	else
	{
		pmat->share_type = SHARE_NOTSHARED;
	}

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) pmat;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PshscanFromDXLSharedScan
//
//	@doc:
//		Translate DXL materialize node into GPDB Material plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PshscanFromDXLSharedScan
	(
	const CDXLNode *pdxlnSharedScan,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create sort plan node
	ShareInputScan *pshscan = MakeNode(ShareInputScan);

	CDXLPhysicalSharedScan *pdxlopShscan = CDXLPhysicalSharedScan::PdxlopConvert(pdxlnSharedScan->Pdxlop());

	// set spooling info
	const CDXLSpoolInfo *pspoolinfo = pdxlopShscan->Pspoolinfo();

	pshscan->share_id = pspoolinfo->UlSpoolId();
	pshscan->driver_slice = pspoolinfo->IExecutorSlice();
	pshscan->share_type = ShtypeFromSpoolInfo(pspoolinfo);

	GPOS_ASSERT(SHARE_NOTSHARED != pshscan->share_type);

	Plan *pplan = &(pshscan->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopShscan->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	CDXLTranslateContext *pdxltrctxChild = NULL;

	if (EdxlshscanIndexSentinel == pdxlnSharedScan->UlArity())
	{
		// translate shared scan child
		CDXLNode *pdxlnChild = (*pdxlnSharedScan)[EdxlshscanIndexChild];
		pdxltrctxChild = New(m_pmp) CDXLTranslateContext(m_pmp, false, pdxltrctxOut->PhmColParam());

		Plan *pplanChild = PplFromDXL(pdxlnChild, pdxltrctxChild, pplan);

		pplan->lefttree = pplanChild;
		pplan->nMotionNodes = pplanChild->nMotionNodes;

		// store translation context
		m_pctxdxltoplstmt->AddSharedScanTranslationContext(pspoolinfo->UlSpoolId(), pdxltrctxChild);
	}
	else
	{
		// no direct child: we must have translated the actual spool for this shared scan already
		// find the corresponding translation context
		pdxltrctxChild = const_cast<CDXLTranslateContext*>(m_pctxdxltoplstmt->PdxltrctxForSharedScan(pspoolinfo->UlSpoolId()));
	}

	GPOS_ASSERT(NULL != pdxltrctxChild);

	CDXLNode *pdxlnPrL = (*pdxlnSharedScan)[EdxlshscanIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnSharedScan)[EdxlshscanIndexFilter];

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(pdxltrctxChild);

	// translate proj list and filter
	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&pplan->qual,
		pdxltrctxOut,
		pplan
		);

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) pshscan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanSequence
//
//	@doc:
//		Translate DXL sequence node into GPDB Sequence plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanSequence
	(
	const CDXLNode *pdxlnSequence,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create append plan node
	Sequence *psequence = MakeNode(Sequence);

	Plan *pplan = &(psequence->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalSequence *pdxlop = CDXLPhysicalSequence::PdxlopConvert(pdxlnSequence->Pdxlop());

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlop->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate last child
	ULONG ulArity = pdxlnSequence->UlArity();

	CDXLNode *pdxlnLastChild = (*pdxlnSequence)[ulArity - 1];

	CDXLTranslateContext dxltrctxChild(m_pmp, false);

	Plan *pplanLastChild = PplFromDXL(pdxlnLastChild, &dxltrctxChild, pplan);
	pplan->nMotionNodes = pplanLastChild->nMotionNodes;

	CDXLNode *pdxlnPrL = (*pdxlnSequence)[0];

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	// translate proj list
	pplan->targetlist = PlTargetListFromProjList
						(
						pdxlnPrL,
						NULL,		// base table translation context
						pdrgpdxltrctx,
						pdxltrctxOut,
						pplan
						);

	// translate the rest of the children
	for (ULONG ul = 1; ul < ulArity - 1; ul++)
	{
		CDXLNode *pdxlnChild = (*pdxlnSequence)[ul];

		Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

		psequence->subplans = gpdb::PlAppendElement(psequence->subplans, pplanChild);
		pplan->nMotionNodes += pplanChild->nMotionNodes;
	}

	psequence->subplans = gpdb::PlAppendElement(psequence->subplans, pplanLastChild);

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) psequence;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanDTS
//
//	@doc:
//		Translates a DXL dynamic table scan node into a DynamicTableScan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanDTS
	(
	const CDXLNode *pdxlnDTS,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalDynamicTableScan *pdxlop = CDXLPhysicalDynamicTableScan::PdxlopConvert(pdxlnDTS->Pdxlop());

	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	// add the new range table entry as the last element of the range table
	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;

	RangeTblEntry *prte = PrteFromTblDescr(pdxlop->Pdxltabdesc(), NULL /*pdxlid*/, iRel, &dxltrctxbt);
	GPOS_ASSERT(NULL != prte);

	m_pctxdxltoplstmt->AddRTE(prte);
	m_pctxdxltoplstmt->AddPartitionedTable(prte->relid);

	// create dynamic scan node
	DynamicTableScan *pdts = MakeNode(DynamicTableScan);

	pdts->scan.scanrelid = iRel;
	pdts->partIndex = pdxlop->UlPartIndexId();

	Plan *pplan = &(pdts->scan.plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);
	pplan->nMotionNodes = 0;

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlop->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	// translate proj list
	GPOS_ASSERT(1 == pdxlnDTS->UlArity());
	CDXLNode *pdxlnPrL = (*pdxlnDTS)[0];

	pplan->targetlist = PlTargetListFromProjList
						(
						pdxlnPrL,
						&dxltrctxbt,		// base table translation context
						NULL,			// child translation contexts
						pdxltrctxOut,
						pplan
						);

	SetParamIds(pplan);

	return (Plan *) pdts;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanDML
//
//	@doc:
//		Translates a DXL DML node 
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanDML
	(
	const CDXLNode *pdxlnDML,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate table descriptor into a range table entry
	CDXLPhysicalDML *pdxlop = CDXLPhysicalDML::PdxlopConvert(pdxlnDML->Pdxlop());

	// create DML node
	DML *pdml = MakeNode(DML);
	Plan *pplan = &(pdml->plan);
	
	switch (pdxlop->EdxlDmlOpType())
	{
		case Edxldmldelete:
			m_cmdtype = CMD_DELETE;
			break;
		
		case Edxldmlupdate:
			m_cmdtype = CMD_UPDATE;
			break;

		case Edxldmlinsert:
			m_cmdtype = CMD_INSERT;
			break;
	}
	
	IMDId *pmdidTargetTable = pdxlop->Pdxltabdesc()->Pmdid();
	if (IMDRelation::EreldistrMasterOnly != m_pmda->Pmdrel(pmdidTargetTable)->Ereldistribution())
	{
		m_fTargetTableDistributed = true;
	}
	
	// translation context for column mappings in the base relation
	CDXLTranslateContextBaseTable dxltrctxbt(m_pmp);

	// add the new range table entry as the last element of the range table
	Index iRel = gpdb::UlListLength(m_pctxdxltoplstmt->PlPrte()) + 1;
	pdml->scanrelid = iRel;
	
	m_plResultRelations = gpdb::PlAppendInt(m_plResultRelations, iRel);

	CDXLTableDescr *pdxltabdesc = pdxlop->Pdxltabdesc();
	RangeTblEntry *prte = PrteFromTblDescr(pdxltabdesc, NULL /*pdxlid*/, iRel, &dxltrctxbt);
	GPOS_ASSERT(NULL != prte);

	m_pctxdxltoplstmt->AddRTE(prte);
	const IMDRelation *pmdrel = m_pmda->Pmdrel(pdxltabdesc->Pmdid());
	
	if (pmdrel->FPartitioned())
	{
		m_pctxdxltoplstmt->AddPartitionedTable(prte->relid);
	}
	
	CDXLNode *pdxlnPrL = (*pdxlnDML)[0];
	CDXLNode *pdxlnChild = (*pdxlnDML)[1];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(&dxltrctxChild);

	// translate proj list
	List *plTargetList = PlTargetListFromProjList
		(
		pdxlnPrL,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		pdxltrctxOut,
		pplan
		);
	
	// add ctid, action and oid columns to target list
	pdml->oidColIdx = UlAddTargetEntryForColId(&plTargetList, &dxltrctxChild, pdxlop->UlOid(), true /*fResjunk*/);
	pdml->actionColIdx = UlAddTargetEntryForColId(&plTargetList, &dxltrctxChild, pdxlop->UlAction(), true /*fResjunk*/);
	pdml->ctidColIdx = UlAddTargetEntryForColId(&plTargetList, &dxltrctxChild, pdxlop->UlCtid(), true /*fResjunk*/);
	
	GPOS_ASSERT(0 != pdml->actionColIdx);
	GPOS_ASSERT(0 != pdml->oidColIdx);

	pplan->targetlist = plTargetList;
	
	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();


	// translate operator costs
	TranslatePlanCosts
		(
		pdxlop->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	return (Plan *) pdml;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanSplit
//
//	@doc:
//		Translates a DXL Split node 
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanSplit
	(
	const CDXLNode *pdxlnSplit,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	CDXLPhysicalSplit *pdxlop = CDXLPhysicalSplit::PdxlopConvert(pdxlnSplit->Pdxlop());

	// create SplitUpdate node
	SplitUpdate *psplit = MakeNode(SplitUpdate);
	Plan *pplan = &(psplit->plan);
	
	CDXLNode *pdxlnPrL = (*pdxlnSplit)[0];
	CDXLNode *pdxlnChild = (*pdxlnSplit)[1];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(&dxltrctxChild);

	// translate proj list and filter
	pplan->targetlist = PlTargetListFromProjList
		(
		pdxlnPrL,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		pdxltrctxOut,
		pplan
		);

	// translate delete and insert columns
	DrgPul *pdrgpulDeleteCols = pdxlop->PdrgpulDelete();
	DrgPul *pdrgpulInsertCols = pdxlop->PdrgpulInsert();
		
	GPOS_ASSERT(pdrgpulInsertCols->UlLength() == pdrgpulDeleteCols->UlLength());
	
	psplit->deleteColIdx = CTranslatorUtils::PlAttnosFromColids(pdrgpulDeleteCols, &dxltrctxChild);
	psplit->insertColIdx = CTranslatorUtils::PlAttnosFromColids(pdrgpulInsertCols, &dxltrctxChild);
	
	const TargetEntry *pteActionCol = pdxltrctxOut->Pte(pdxlop->UlAction());
	const TargetEntry *pteCtidCol = pdxltrctxOut->Pte(pdxlop->UlCtid());
	
	GPOS_ASSERT(NULL != pteActionCol);
	GPOS_ASSERT(NULL != pteCtidCol);
	
	psplit->actionColIdx = pteActionCol->resno;
	psplit->ctidColIdx = pteCtidCol->resno;
	
	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlop->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	return (Plan *) psplit;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanAssert
//
//	@doc:
//		Translate DXL assert node into GPDB assert plan node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanAssert
	(
	const CDXLNode *pdxlnAssert,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// create assert plan node
	AssertOp *passert = MakeNode(AssertOp);

	Plan *pplan = &(passert->plan);
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	CDXLPhysicalAssert *pdxlopAssert = CDXLPhysicalAssert::PdxlopConvert(pdxlnAssert->Pdxlop());

	// TODO: antova - Nov 2, 2012; set error code
	passert->errcode = pdxlopAssert->Eerrc();
	
	// translate operator costs
	TranslatePlanCosts
		(
		pdxlopAssert->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	// translate child plan
	CDXLNode *pdxlnChild = (*pdxlnAssert)[CDXLPhysicalAssert::EdxlassertIndexChild];
	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	GPOS_ASSERT(NULL != pplanChild && "child plan cannot be NULL");

	passert->plan.lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;

	CDXLNode *pdxlnPrL = (*pdxlnAssert)[CDXLPhysicalAssert::EdxlassertIndexProjList];
	CDXLNode *pdxlnFilter = (*pdxlnAssert)[CDXLPhysicalAssert::EdxlassertIndexFilter];

	List *plQuals = NULL;

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(const_cast<CDXLTranslateContext*>(&dxltrctxChild));

	TranslateProjListAndFilter
		(
		pdxlnPrL,
		pdxlnFilter,
		NULL,		// translate context for the base table
		pdrgpdxltrctx,
		&pplan->targetlist,
		&plQuals,
		pdxltrctxOut,
		pplan
		);


	pplan->qual = plQuals;

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	return (Plan *) passert;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PplanRowTrigger
//
//	@doc:
//		Translates a DXL Row Trigger node
//
//---------------------------------------------------------------------------
Plan *
CTranslatorDXLToPlStmt::PplanRowTrigger
	(
	const CDXLNode *pdxlnRowTrigger,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	CDXLPhysicalRowTrigger *pdxlop = CDXLPhysicalRowTrigger::PdxlopConvert(pdxlnRowTrigger->Pdxlop());

	// create RowTrigger node
	RowTriggerOp *prowtrigger = MakeNode(RowTriggerOp);
	Plan *pplan = &(prowtrigger->plan);

	CDXLNode *pdxlnPrL = (*pdxlnRowTrigger)[0];
	CDXLNode *pdxlnChild = (*pdxlnRowTrigger)[1];

	CDXLTranslateContext dxltrctxChild(m_pmp, false, pdxltrctxOut->PhmColParam());

	Plan *pplanChild = PplFromDXL(pdxlnChild, &dxltrctxChild, pplan);

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(&dxltrctxChild);

	// translate proj list and filter
	pplan->targetlist = PlTargetListFromProjList
		(
		pdxlnPrL,
		NULL,			// translate context for the base table
		pdrgpdxltrctx,
		pdxltrctxOut,
		pplan
		);

	Oid oidRelid = CMDIdGPDB::PmdidConvert(pdxlop->PmdidRel())->OidObjectId();
	GPOS_ASSERT(InvalidOid != oidRelid);
	prowtrigger->relid = oidRelid;
	prowtrigger->eventFlags = pdxlop->IType();

	// translate old and new columns
	DrgPul *pdrgpulOldCols = pdxlop->PdrgpulOld();
	DrgPul *pdrgpulNewCols = pdxlop->PdrgpulNew();

	GPOS_ASSERT_IMP(NULL != pdrgpulOldCols && NULL != pdrgpulNewCols,
					pdrgpulNewCols->UlLength() == pdrgpulOldCols->UlLength());

	if (NULL == pdrgpulOldCols)
	{
		prowtrigger->oldValuesColIdx = NIL;
	}
	else
	{
		prowtrigger->oldValuesColIdx = CTranslatorUtils::PlAttnosFromColids(pdrgpulOldCols, &dxltrctxChild);
	}

	if (NULL == pdrgpulNewCols)
	{
		prowtrigger->newValuesColIdx = NIL;
	}
	else
	{
		prowtrigger->newValuesColIdx = CTranslatorUtils::PlAttnosFromColids(pdrgpulNewCols, &dxltrctxChild);
	}

	pplan->lefttree = pplanChild;
	pplan->nMotionNodes = pplanChild->nMotionNodes;
	pplan->plan_node_id = m_pctxdxltoplstmt->UlNextPlanId();
	pplan->plan_parent_node_id = IPlanId(pplanParent);

	SetParamIds(pplan);

	// cleanup
	pdrgpdxltrctx->Release();

	// translate operator costs
	TranslatePlanCosts
		(
		pdxlop->Pdxlprop()->Pdxlopcost(),
		&(pplan->startup_cost),
		&(pplan->total_cost),
		&(pplan->plan_rows),
		&(pplan->plan_width)
		);

	return (Plan *) prowtrigger;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PrteFromTblDescr
//
//	@doc:
//		Translates a DXL table descriptor into a range table entry. If an index
//		descriptor is provided, we use the mapping from colids to index attnos
//		instead of table attnos
//
//---------------------------------------------------------------------------
RangeTblEntry *
CTranslatorDXLToPlStmt::PrteFromTblDescr
	(
	const CDXLTableDescr *pdxltabdesc,
	const CDXLIndexDescr *pdxlid,
	Index iRel,
	CDXLTranslateContextBaseTable *pdxltrctxbtOut
	)
{
	RangeTblEntry *prte = MakeNode(RangeTblEntry);
	prte->rtekind = RTE_RELATION;

	// get the index if given
	const IMDIndex *pmdindex = NULL;
	if (NULL != pdxlid)
	{
		pmdindex = m_pmda->Pmdindex(pdxlid->Pmdid());
	}

	// get oid for table
	Oid oid = CMDIdGPDB::PmdidConvert(pdxltabdesc->Pmdid())->OidObjectId();
	GPOS_ASSERT(InvalidOid != oid);

	prte->relid = oid;

	// save oid and range index in translation context
	pdxltrctxbtOut->SetOID(oid);
	pdxltrctxbtOut->SetIdx(iRel);

	Alias *palias = MakeNode(Alias);
	palias->colnames = NIL;

	// get table alias
	palias->aliasname = CTranslatorUtils::SzFromWsz(pdxltabdesc->Pmdname()->Pstr()->Wsz());

	// get column names
	const ULONG ulArity = pdxltabdesc->UlArity();
	for (ULONG ul = 0; ul < ulArity; ++ul)
	{
		const CDXLColDescr *pdxlcd = pdxltabdesc->Pdxlcd(ul);
		GPOS_ASSERT(NULL != pdxlcd);

		INT iAttno = pdxlcd->IAttno();

		GPOS_ASSERT(0 != iAttno);

		if (0 < iAttno)
		{
			// non-system attribute
			CHAR *szColName = CTranslatorUtils::SzFromWsz(pdxlcd->Pmdname()->Pstr()->Wsz());
			Value *pvalColName = gpdb::PvalMakeString(szColName);

			palias->colnames = gpdb::PlAppendElement(palias->colnames, pvalColName);
		}

		// get the attno from the index, in case of indexonlyscan
		if (NULL != pmdindex)
		{
			iAttno = 1 + pmdindex->UlPosInKey((ULONG) iAttno - 1);
		}

		// save mapping col id -> index in translate context
		(void) pdxltrctxbtOut->FInsertMapping(pdxlcd->UlID(), iAttno);
	}

	prte->eref = palias;

	return prte;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlTargetListFromProjList
//
//	@doc:
//		Translates a DXL projection list node into a target list.
//		For base table projection lists, the caller should provide a base table
//		translation context with table oid, rtable index and mappings for the columns.
//		For other nodes pdxltrctxLeft and pdxltrctxRight give
//		the mappings of column ids to target entries in the corresponding child nodes
//		for resolving the origin of the target entries
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::PlTargetListFromProjList
	(
	const CDXLNode *pdxlnPrL,
	const CDXLTranslateContextBaseTable *pdxltrctxbt,
	DrgPdxltrctx *pdrgpdxltrctx,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	if (NULL == pdxlnPrL)
	{
		return NULL;
	}

	List *plTargetList = NIL;

	// translate each DXL project element into a target entry
	const ULONG ulArity = pdxlnPrL->UlArity();
	for (ULONG ul = 0; ul < ulArity; ++ul)
	{
		CDXLNode *pdxlnPrEl = (*pdxlnPrL)[ul];
		GPOS_ASSERT(EdxlopScalarProjectElem == pdxlnPrEl->Pdxlop()->Edxlop());
		CDXLScalarProjElem *pdxlopPrel = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop());
		GPOS_ASSERT(1 == pdxlnPrEl->UlArity());

		// translate proj element expression
		CDXLNode *pdxlnExpr = (*pdxlnPrEl)[0];

		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
																(
																m_pmp,
																pdxltrctxbt,
																pdrgpdxltrctx,
																pdxltrctxOut,
																m_pctxdxltoplstmt,
																pplanParent
																);

		Expr *pexpr = m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnExpr, &mapcidvarplstmt);

		GPOS_ASSERT(NULL != pexpr);

		TargetEntry *pte = MakeNode(TargetEntry);
		pte->expr = pexpr;
		pte->resname = CTranslatorUtils::SzFromWsz(pdxlopPrel->PmdnameAlias()->Pstr()->Wsz());
		pte->resno = (AttrNumber) (ul + 1);

		if (IsA(pexpr, Var))
		{
			// check the origin of the left or the right side
			// of the current operator and if it is derived from a base relation,
			// set resorigtbl and resorigcol appropriately

			if (NULL != pdxltrctxbt)
			{
				// translating project list of a base table
				pte->resorigtbl = pdxltrctxbt->OidRel();
				pte->resorigcol = ((Var *) pexpr)->varattno;
			}
			else
			{
				// not translating a base table proj list: variable must come from
				// the left or right child of the operator

				GPOS_ASSERT(NULL != pdrgpdxltrctx);
				GPOS_ASSERT(0 != pdrgpdxltrctx->UlSafeLength());
				ULONG ulColId = CDXLScalarIdent::PdxlopConvert(pdxlnExpr->Pdxlop())->Pdxlcr()->UlID();

				const CDXLTranslateContext *pdxltrctxLeft = (*pdrgpdxltrctx)[0];
				GPOS_ASSERT(NULL != pdxltrctxLeft);
				const TargetEntry *pteOriginal = pdxltrctxLeft->Pte(ulColId);

				if (NULL == pteOriginal)
				{
					// variable not found on the left side
					GPOS_ASSERT(2 == pdrgpdxltrctx->UlSafeLength());
					const CDXLTranslateContext *pdxltrctxRight = (*pdrgpdxltrctx)[1];

					GPOS_ASSERT(NULL != pdxltrctxRight);
					pteOriginal = pdxltrctxRight->Pte(ulColId);
				}

				GPOS_ASSERT(NULL != pteOriginal);
				pte->resorigtbl = pteOriginal->resorigtbl;
				pte->resorigcol = pteOriginal->resorigcol;
			}
		}

		// add column mapping to output translation context
		pdxltrctxOut->InsertMapping(pdxlopPrel->UlId(), pte);

		plTargetList = gpdb::PlAppendElement(plTargetList, pte);
	}

	return plTargetList;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlTargetListForHashNode
//
//	@doc:
//		Create a target list for the hash node of a hash join plan node by creating a list
//		of references to the elements in the child project list
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::PlTargetListForHashNode
	(
	const CDXLNode *pdxlnPrL,
	CDXLTranslateContext *pdxltrctxChild,
	CDXLTranslateContext *pdxltrctxOut
	)
{
	List *plTargetList = NIL;
	const ULONG ulArity = pdxlnPrL->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnPrEl = (*pdxlnPrL)[ul];
		CDXLScalarProjElem *pdxlopPrel = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop());

		const TargetEntry *pteChild = pdxltrctxChild->Pte(pdxlopPrel->UlId());
		GPOS_ASSERT(NULL != pteChild);

		// get type oid for project element's expression
		GPOS_ASSERT(1 == pdxlnPrEl->UlArity());

		// find column type
		OID oidType = gpdb::OidExprType((Node*) pteChild->expr);

		// find the original varno and attno for this column
		Index idxVarnoold = 0;
		AttrNumber attnoOld = 0;

		if (IsA(pteChild->expr, Var))
		{
			Var *pv = (Var*) pteChild->expr;
			idxVarnoold = pv->varnoold;
			attnoOld = pv->varoattno;
		}
		else
		{
			idxVarnoold = OUTER;
			attnoOld = pteChild->resno;
		}

		// create a Var expression for this target list entry expression
		Var *pvar = gpdb::PvarMakeVar
					(
					OUTER,
					pteChild->resno,
					oidType,
					-1,	// vartypmod
					0	// varlevelsup
					);

		// set old varno and varattno since makeVar does not set them
		pvar->varnoold = idxVarnoold;
		pvar->varoattno = attnoOld;

		CHAR *szResname = CTranslatorUtils::SzFromWsz(pdxlopPrel->PmdnameAlias()->Pstr()->Wsz());

		TargetEntry *pte = gpdb::PteMakeTargetEntry
							(
							(Expr *) pvar,
							(AttrNumber) (ul + 1),
							szResname,
							false		// resjunk
							);

		plTargetList = gpdb::PlAppendElement(plTargetList, pte);
		pdxltrctxOut->InsertMapping(pdxlopPrel->UlId(), pte);
	}

	return plTargetList;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlQualFromFilter
//
//	@doc:
//		Translates a DXL filter node into a Qual list.
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::PlQualFromFilter
	(
	const CDXLNode * pdxlnFilter,
	const CDXLTranslateContextBaseTable *pdxltrctxbt,
	DrgPdxltrctx *pdrgpdxltrctx,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	const ULONG ulArity = pdxlnFilter->UlArity();
	if (0 == ulArity)
	{
		return NIL;
	}

	GPOS_ASSERT(1 == ulArity);

	CDXLNode *pdxlnFilterCond = (*pdxlnFilter)[0];
	GPOS_ASSERT(CTranslatorDXLToScalar::FBoolean(pdxlnFilterCond, m_pmda));

	return PlQualFromScalarCondNode(pdxlnFilterCond, pdxltrctxbt, pdrgpdxltrctx, pdxltrctxOut, pplanParent);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::PlQualFromScalarCondNode
//
//	@doc:
//		Translates a DXL scalar condition node node into a Qual list.
//
//---------------------------------------------------------------------------
List *
CTranslatorDXLToPlStmt::PlQualFromScalarCondNode
	(
	const CDXLNode *pdxlnCond,
	const CDXLTranslateContextBaseTable *pdxltrctxbt,
	DrgPdxltrctx *pdrgpdxltrctx,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	List *plQuals = NIL;

	GPOS_ASSERT(CTranslatorDXLToScalar::FBoolean(const_cast<CDXLNode*>(pdxlnCond), m_pmda));

	CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
															(
															m_pmp,
															pdxltrctxbt,
															pdrgpdxltrctx,
															pdxltrctxOut,
															m_pctxdxltoplstmt,
															pplanParent
															);

	Expr *pexpr = m_pdxlsctranslator->PexprFromDXLNodeScalar
					(
					pdxlnCond,
					&mapcidvarplstmt
					);

	plQuals = gpdb::PlAppendElement(plQuals, pexpr);

	return plQuals;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslatePlanCosts
//
//	@doc:
//		Translates DXL plan costs into the GPDB cost variables
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslatePlanCosts
	(
	const CDXLOperatorCost *pdxlopcost,
	Cost *pcostStartupOut,
	Cost *pcostTotalOut,
	Cost *pcostRowsOut,
	INT * piWidthOut
	)
{
	*pcostStartupOut = CostFromStr(pdxlopcost->PstrStartupCost());
	*pcostTotalOut = CostFromStr(pdxlopcost->PstrTotalCost());
	*pcostRowsOut = CostFromStr(pdxlopcost->PstrRows());
	*piWidthOut = CTranslatorUtils::IFromStr(pdxlopcost->PstrWidth());
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateProjListAndFilter
//
//	@doc:
//		Translates DXL proj list and filter into GPDB's target and qual lists,
//		respectively
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateProjListAndFilter
	(
	const CDXLNode *pdxlnPrL,
	const CDXLNode *pdxlnFilter,
	const CDXLTranslateContextBaseTable *pdxltrctxbt,
	DrgPdxltrctx *pdrgpdxltrctx,
	List **pplTargetListOut,
	List **pplQualOut,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	// translate proj list
	*pplTargetListOut = PlTargetListFromProjList
						(
						pdxlnPrL,
						pdxltrctxbt,		// base table translation context
						pdrgpdxltrctx,
						pdxltrctxOut,
						pplanParent
						);

	// translate filter
	*pplQualOut = PlQualFromFilter
					(
					pdxlnFilter,
					pdxltrctxbt,			// base table translation context
					pdrgpdxltrctx,
					pdxltrctxOut,
					pplanParent
					);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateHashExprList
//
//	@doc:
//		Translates DXL hash expression list in a redistribute motion node into
//		GPDB's hash expression and expression types lists, respectively
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateHashExprList
	(
	const CDXLNode *pdxlnHashExprList,
	const CDXLTranslateContext *pdxltrctxChild,
	List **pplHashExprOut,
	List **pplHashExprTypesOut,
	CDXLTranslateContext *pdxltrctxOut,
	Plan *pplanParent
	)
{
	GPOS_ASSERT(NIL == *pplHashExprOut);
	GPOS_ASSERT(NIL == *pplHashExprTypesOut);

	List *plHashExpr = NIL;
	List *plHashExprTypes = NIL;

	DrgPdxltrctx *pdrgpdxltrctx = New(m_pmp) DrgPdxltrctx(m_pmp);
	pdrgpdxltrctx->Append(pdxltrctxChild);

	const ULONG ulArity = pdxlnHashExprList->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnHashExpr = (*pdxlnHashExprList)[ul];
		CDXLScalarHashExpr *pdxlopHashExpr = CDXLScalarHashExpr::PdxlopConvert(pdxlnHashExpr->Pdxlop());

		const CMDIdGPDB *pmdidType = CMDIdGPDB::PmdidConvert(pdxlopHashExpr->PmdidType());
		plHashExprTypes = gpdb::PlAppendOid(plHashExprTypes, pmdidType->OidObjectId());

		GPOS_ASSERT(1 == pdxlnHashExpr->UlArity());
		CDXLNode *pdxlnExpr = (*pdxlnHashExpr)[0];

		CMappingColIdVarPlStmt mapcidvarplstmt = CMappingColIdVarPlStmt
																(
																m_pmp,
																NULL,
																pdrgpdxltrctx,
																pdxltrctxOut,
																m_pctxdxltoplstmt,
																pplanParent
																);

		Expr *pexpr = m_pdxlsctranslator->PexprFromDXLNodeScalar(pdxlnExpr, &mapcidvarplstmt);

		plHashExpr = gpdb::PlAppendElement(plHashExpr, pexpr);

		GPOS_ASSERT((ULONG) gpdb::UlListLength(plHashExpr) == ul + 1);
	}


	*pplHashExprOut = plHashExpr;
	*pplHashExprTypesOut = plHashExprTypes;

	// cleanup
	pdrgpdxltrctx->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::TranslateSortCols
//
//	@doc:
//		Translates DXL sorting columns list into GPDB's arrays of sorting attribute numbers,
//		and sorting operator ids, respectively.
//		The two arrays must be allocated by the caller.
//
//---------------------------------------------------------------------------
void
CTranslatorDXLToPlStmt::TranslateSortCols
	(
	const CDXLNode *pdxlnSortColList,
	const CDXLTranslateContext *pdxltrctxChild,
	AttrNumber *pattnoSortColIds,
	Oid *poidSortOpIds
	)
{
	const ULONG ulArity = pdxlnSortColList->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnSortCol = (*pdxlnSortColList)[ul];
		CDXLScalarSortCol *pdxlopSortCol = CDXLScalarSortCol::PdxlopConvert(pdxlnSortCol->Pdxlop());

		ULONG ulSortColId = pdxlopSortCol->UlColId();
		const TargetEntry *pteSortCol = pdxltrctxChild->Pte(ulSortColId);
		GPOS_ASSERT(NULL != pteSortCol);

		pattnoSortColIds[ul] = pteSortCol->resno;
		poidSortOpIds[ul] = CMDIdGPDB::PmdidConvert(pdxlopSortCol->PmdidSortOp())->OidObjectId();
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::CostFromStr
//
//	@doc:
//		Parses a cost value from a string
//
//---------------------------------------------------------------------------
Cost
CTranslatorDXLToPlStmt::CostFromStr
	(
	const CWStringBase *pstr
	)
{
	CHAR *sz = CTranslatorUtils::SzFromWsz(pstr->Wsz());
	return gpos::clib::DStrToD(sz);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::FTargetTableDistributed
//
//	@doc:
//		Check if given operator is a DML on a distributed table
//
//---------------------------------------------------------------------------
BOOL
CTranslatorDXLToPlStmt::FTargetTableDistributed
	(
	CDXLOperator *pdxlop
	)
{
	if (EdxlopPhysicalDML != pdxlop->Edxlop())
	{
		return false;
	}

	CDXLPhysicalDML *pdxlopDML = CDXLPhysicalDML::PdxlopConvert(pdxlop);
	IMDId *pmdid = pdxlopDML->Pdxltabdesc()->Pmdid();

	return IMDRelation::EreldistrMasterOnly != m_pmda->Pmdrel(pmdid)->Ereldistribution();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::IAddTargetEntryForColId
//
//	@doc:
//		Add a new target entry for the given colid to the given target list and
//		return the position of the new entry
//
//---------------------------------------------------------------------------
ULONG
CTranslatorDXLToPlStmt::UlAddTargetEntryForColId
	(
	List **pplTargetList,
	CDXLTranslateContext *pdxltrctx,
	ULONG ulColId,
	BOOL fResjunk
	)
{
	GPOS_ASSERT(NULL != pplTargetList);
	
	const TargetEntry *pte = pdxltrctx->Pte(ulColId);
	
	if (NULL == pte)
	{
		// colid not found in translate context
		return 0;
	}
	
	// TODO: antova - Oct 29, 2012; see if entry already exists in the target list
	
	OID oidExpr = gpdb::OidExprType((Node*) pte->expr);
	Var *pvar = gpdb::PvarMakeVar
						(
						OUTER,
						pte->resno,
						oidExpr,
						-1,	// vartypmod
						0	// varlevelsup
						);
	ULONG ulResNo = gpdb::UlListLength(*pplTargetList) + 1;
	CHAR *szResName = PStrDup(pte->resname);
	TargetEntry *pteNew = gpdb::PteMakeTargetEntry((Expr*) pvar, ulResNo, szResName, fResjunk);
	*pplTargetList = gpdb::PlAppendElement(*pplTargetList, pteNew);
	
	return ulResNo;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorDXLToPlStmt::JtFromEdxljt
//
//	@doc:
//		Translates the join type from its DXL representation into the GPDB one
//
//---------------------------------------------------------------------------
JoinType
CTranslatorDXLToPlStmt::JtFromEdxljt
	(
	EdxlJoinType edxljt
	)
{
	GPOS_ASSERT(EdxljtSentinel > edxljt);

	JoinType jt = JOIN_INNER;

	switch (edxljt)
	{
		case EdxljtInner:
			jt = JOIN_INNER;
			break;
		case EdxljtLeft:
			jt = JOIN_LEFT;
			break;
		case EdxljtFull:
			jt = JOIN_FULL;
			break;
		case EdxljtRight:
			jt = JOIN_RIGHT;
			break;
		case EdxljtIn:
			jt = JOIN_IN;
			break;
		case EdxljtLeftAntiSemijoin:
			jt = JOIN_LASJ;
			break;
		default:
			GPOS_ASSERT(!"Unrecognized join type");
	}

	return jt;
}

// EOF
