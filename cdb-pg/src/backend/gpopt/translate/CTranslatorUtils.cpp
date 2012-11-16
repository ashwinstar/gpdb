//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CTranslatorUtils.cpp
//
//	@doc:
//		Implementation of the utility methods for translating GPDB's
//		Query / PlannedStmt into DXL Tree
//
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "postgres.h"

#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "access/sysattr.h"
#include "optimizer/walkers.h"

#include "gpos/base.h"
#include "gpos/common/CAutoTimer.h"
#include "gpos/common/CBitSetIter.h"
#include "gpos/string/CWStringDynamic.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CDXLTranslateContext.h"

#include "dxl/CDXLUtils.h"
#include "dxl/xml/dxltokens.h"
#include "dxl/operators/CDXLDatumOid.h"
#include "dxl/operators/CDXLDatumInt4.h"
#include "dxl/operators/CDXLNode.h"
#include "dxl/operators/CDXLSpoolInfo.h"
#include "dxl/operators/CDXLColDescr.h"
#include "dxl/gpdb_types.h"

#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/base/CUtils.h"

#include "md/CMDIdColStats.h"
#include "md/CMDIdRelStats.h"
#include "md/IMDAggregate.h"
#include "md/IMDRelation.h"
#include "md/IMDIndex.h"
#include "md/IMDTypeBool.h"
#include "md/IMDTypeInt4.h"
#include "md/IMDTypeInt8.h"
#include "md/IMDTypeOid.h"

#include "traceflags/traceflags.h"

#include "gpopt/gpdbwrappers.h"

using namespace gpdxl;
using namespace gpmd;
using namespace gpos;
using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PreloadMD
//
//	@doc:
//		Preload the cache with MD information
//
//---------------------------------------------------------------------------
void
CTranslatorUtils::PreloadMD
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CSystemId sysid,
	Query *pquery
	)
{
	CAutoTimer at("\n[OPT]: Metadata Preload Time", GPOS_FTRACE(EopttracePrintOptStats));

	// TODO: venky Jan 11th 2012; this is a temporary fix that preloads the
	// cache with MD information of base types

	CContextPreloadMD ctxpreloadmd(pmp, pmda);

	// find all relations accessed by query and preload their
	// relstats and colstats
	(void) FPreloadMDStatsWalker
				(
				(Node *) pquery,
				&ctxpreloadmd
				);

	// preload types bool, oid, int4 and int8
	const IMDType *pmdtypeBool = pmda->PtMDType<IMDTypeBool>(sysid);
	PreloadMDType(pmda, pmdtypeBool);
	const IMDType *pmdtypeInt4 = pmda->PtMDType<IMDTypeInt4>(sysid);
	PreloadMDType(pmda, pmdtypeInt4);
	const IMDType *pmdtypeInt8 = pmda->PtMDType<IMDTypeInt8>(sysid);
	PreloadMDType(pmda, pmdtypeInt8);
	const IMDType *pmdtypeOid = pmda->PtMDType<IMDTypeOid>(sysid);
	PreloadMDType(pmda, pmdtypeOid);

	CMDIdGPDB *pmdidDate = New(pmp) CMDIdGPDB(CMDIdGPDB::m_mdidDate.OidObjectId());
	(void) pmda->Pmdtype(pmdidDate);

	CMDIdGPDB *pmdidNumeric = New(pmp) CMDIdGPDB(CMDIdGPDB::m_mdidNumeric.OidObjectId());
	(void) pmda->Pmdtype(pmdidNumeric);

	CMDIdGPDB *pmdidTimestamp = New(pmp) CMDIdGPDB(CMDIdGPDB::m_mdidTimestamp.OidObjectId());
	(void) pmda->Pmdtype(pmdidTimestamp);

	pmdidDate->Release();
	pmdidNumeric->Release();
	pmdidTimestamp->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PreloadMDType
//
//	@doc:
//		Preload the cache with MD information of a particular type
//
//---------------------------------------------------------------------------
void
CTranslatorUtils::PreloadMDType
	(
	CMDAccessor *pmda,
	const IMDType *pmdtype
	)
{
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptEq));
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptNEq));
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptL));
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptLEq));
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptG));
	(void) pmda->Pmdscop(pmdtype->Pmdid(IMDType::EcmptGEq));
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FPreloadMDStatsWalker
//
//	@doc:
//		Walks the expression, descends into queries, finds relations
//		mentioned in the query and preloads column stats for them.
//
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FPreloadMDStatsWalker
	(
	Node *pnode,
	CContextPreloadMD *pctxpreloadmd
	)
{
	if (NULL == pnode)
	{
		return false;
	}

	if (IsA(pnode, RangeTblEntry))
	{
		RangeTblEntry *prte = (RangeTblEntry *) pnode;

		// need to exclude views
		if (prte->rtekind == RTE_RELATION)
		{
			PreloadMDStats(pctxpreloadmd->m_pmp, pctxpreloadmd->m_pmda, prte->relid);
		}
		else if (prte->rtekind == RTE_SUBQUERY)
		{
			gpdb::FWalkQueryOrExpressionTree
								(
								(Node *) prte->subquery,
								(BOOL (*)()) CTranslatorUtils::FPreloadMDStatsWalker,
								pctxpreloadmd,
								QTW_EXAMINE_RTES // query tree walker flags
								);
		}
		return false;
	}

	if (IsA(pnode, Query))
	{
		return gpdb::FWalkQueryOrExpressionTree
					(
					pnode,
					(BOOL (*)()) CTranslatorUtils::FPreloadMDStatsWalker,
					pctxpreloadmd,
					QTW_EXAMINE_RTES // query tree walker flags
					);
	}

	return gpdb::FWalkExpressionTree
			(
			pnode,
			(BOOL (*)()) CTranslatorUtils::FPreloadMDStatsWalker,
			pctxpreloadmd
			);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PreloadMDStats
//
//	@doc:
//		Preloads the relstats and column stats for relation
//
//---------------------------------------------------------------------------
void
CTranslatorUtils::PreloadMDStats
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	OID oidRelation
	)
{
	CMDIdGPDB *pmdidgpdbRel = CDXLUtils::Pmdid(pmp, oidRelation);

	// preload column stats
	const IMDRelation *pmdrelation = pmda->Pmdrel(pmdidgpdbRel);

	if (pmdrelation->Erelstorage() >= IMDRelation::ErelstorageExternal)
	{
		return;
	}

	for (ULONG ulAttNo = 0; ulAttNo < pmdrelation->UlColumns(); ulAttNo++)
	{
		pmdidgpdbRel->AddRef();
		CMDIdColStats *pmdidColStats = New(pmp) CMDIdColStats(pmdidgpdbRel, ulAttNo);
		(void *) pmda->Pmdcolstats(pmdidColStats);
		pmdidColStats->Release();
	}

	// preload relation stats
	{
		pmdidgpdbRel->AddRef();
		CMDIdRelStats *pmdidRelStats = New(pmp) CMDIdRelStats(pmdidgpdbRel);
		(void *) pmda->Pmdrelstats(pmdidRelStats);
		pmdidRelStats->Release();
	}

	pmdidgpdbRel->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Pdxlid
//
//	@doc:
//		Create a DXL index descriptor from an index MD id
//
//---------------------------------------------------------------------------
CDXLIndexDescr *
CTranslatorUtils::Pdxlid
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	const IMDIndex *pmdindex = pmda->Pmdindex(pmdid);
	const CWStringConst *pstrIndexName = pmdindex->Mdname().Pstr();
	CMDName *pmdnameIdx = New(pmp) CMDName(pmp, pstrIndexName);

	return New (pmp) CDXLIndexDescr(pmp, pmdid, pmdnameIdx);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Pdxltabdesc
//
//	@doc:
//		Create a DXL table descriptor from a GPDB range table entry
//
//---------------------------------------------------------------------------
CDXLTableDescr *
CTranslatorUtils::Pdxltabdesc
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CIdGenerator *pidgtor,
	const RangeTblEntry *prte
	)
{
	// generate an MDId for the table desc.
	CMDIdGPDB *pmdid = CDXLUtils::Pmdid(pmp, prte->relid);

	// look up table name
	const IMDRelation *pmdrel = pmda->Pmdrel(pmdid);

	if (IMDRelation::ErelstorageExternal == pmdrel->Erelstorage())
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("External tables"));
	}

	const CWStringConst *pstrTblName = pmdrel->Mdname().Pstr();

	// copy table name
	CMDName *pmdnameTbl = New(pmp) CMDName(pmp, pstrTblName);

	CDXLTableDescr *pdxltabdesc = New(pmp) CDXLTableDescr(pmp, pmdid, pmdnameTbl);

	const ULONG ulLen = pmdrel->UlColumns();
	// add columns from md cache relation object to table descriptor
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		CMDIdGPDB *pmdidColType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType());
		pmdidColType->AddRef();

		// create a column descriptor for the column
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr(pmp, pmdnameCol, pidgtor->UlNextId(), pmdcol->IAttno(), pmdidColType, false /* fColDropped */);
		pdxltabdesc->AddColumnDescr(pdxlcd);
	}

	return pdxltabdesc;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Pdxltvf
//
//	@doc:
//		Create a DXL logical TVF from a GPDB range table entry
//
//---------------------------------------------------------------------------
CDXLLogicalTVF *
CTranslatorUtils::Pdxltvf
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CIdGenerator *pidgtor,
	const RangeTblEntry *prte
	)
{
	GPOS_ASSERT(NULL != prte->funcexpr);
	FuncExpr *pfuncexpr = (FuncExpr *)prte->funcexpr;

	// get function id
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(pfuncexpr->funcid);

	// get function from MDcache
	const IMDFunction *pmdfunc = pmda->Pmdfunc(pmdid);

	IMDId *pmdidRetType = pmdfunc->PmdidTypeResult();
	DrgPmdid *pdrgpmdidOutArgTypes = pmdfunc->PdrgpmdidOutputArgTypes();

	DrgPdxlcd *pdrgdxlcd = NULL;

	if (NULL != prte->funccoltypes)
	{
		// function returns record - use col names and types from query
		pdrgdxlcd = PdrgdxlcdRecord(pmp, pidgtor, prte->eref->colnames, prte->funccoltypes);
		CRefCount::SafeRelease(pdrgpmdidOutArgTypes);
	}
	else if (NULL != pdrgpmdidOutArgTypes)
	{
		// function returns record - but output col types are defined in catalog
		pdrgdxlcd = PdrgdxlcdRecord(pmp, pidgtor, prte->eref->colnames, pdrgpmdidOutArgTypes);
	}
	else
	{
		const IMDType *pmdType = pmda->Pmdtype(pmdidRetType);
		if (pmdType->FComposite())
		{
			pdrgdxlcd = PdrgdxlcdComposite(pmp, pmda, pidgtor, pmdType);
		}
		else
		{
			CMDName mdnameFunc = pmdfunc->Mdname();
			pdrgdxlcd = PdrgdxlcdBase(pmp, pidgtor, pmdidRetType, &mdnameFunc);
		}
	}

	CMDName *pmdfuncname = New(pmp) CMDName(pmp, pmdfunc->Mdname().Pstr());

	CDXLLogicalTVF *pdxlopTVF = New(pmp) CDXLLogicalTVF(pmp, pmdid, pmdfuncname, pdrgdxlcd);

	return pdxlopTVF;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgdxlcdRecord
//
//	@doc:
//		Get column descriptors from a record type
//
//---------------------------------------------------------------------------
DrgPdxlcd *
CTranslatorUtils::PdrgdxlcdRecord
	(
	IMemoryPool *pmp,
	CIdGenerator *pidgtor,
	List *plColNames,
	List *plColTypes
	)
{
	ListCell *plcColName = NULL;
	ListCell *plcColType = NULL;

	ULONG ul = 0;
	DrgPdxlcd *pdrgdxlcd = New(pmp) DrgPdxlcd(pmp);

	ForBoth (plcColName, plColNames,
			plcColType, plColTypes)
	{
		Value *pvalue = (Value *) lfirst(plcColName);
		Oid coltype = lfirst_oid(plcColType);

		CHAR *szColName = strVal(pvalue);
		CWStringDynamic *pstrColName = CDXLUtils::PstrFromSz(pmp, szColName);
		CMDName *pmdColName = New(pmp) CMDName(pmp, pstrColName);
		delete(pstrColName);

		IMDId *pmdidColType = New(pmp) CMDIdGPDB(coltype);

		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdColName,
										pidgtor->UlNextId(),
										INT(ul + 1) /* iAttno */,
										pmdidColType,
										false /* fColDropped */
										);
		pdrgdxlcd->Append(pdxlcd);
		ul++;
	}

	return pdrgdxlcd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgdxlcdRecord
//
//	@doc:
//		Get column descriptors from a record type
//
//---------------------------------------------------------------------------
DrgPdxlcd *
CTranslatorUtils::PdrgdxlcdRecord
	(
	IMemoryPool *pmp,
	CIdGenerator *pidgtor,
	List *plColNames,
	DrgPmdid *pdrgpmdidOutArgTypes
	)
{
	GPOS_ASSERT(pdrgpmdidOutArgTypes->UlLength() == (ULONG) gpdb::UlListLength(plColNames));
	ListCell *plcColName = NULL;

	ULONG ul = 0;
	DrgPdxlcd *pdrgdxlcd = New(pmp) DrgPdxlcd(pmp);

	ForEach (plcColName, plColNames)
	{
		Value *pvalue = (Value *) lfirst(plcColName);

		CHAR *szColName = strVal(pvalue);
		CWStringDynamic *pstrColName = CDXLUtils::PstrFromSz(pmp, szColName);
		CMDName *pmdColName = New(pmp) CMDName(pmp, pstrColName);
		delete(pstrColName);

		IMDId *pmdidColType = (*pdrgpmdidOutArgTypes)[ul];
		pmdidColType->AddRef();

		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdColName,
										pidgtor->UlNextId(),
										INT(ul + 1) /* iAttno */,
										pmdidColType,
										false /* fColDropped */
										);
		pdrgdxlcd->Append(pdxlcd);
		ul++;
	}

	return pdrgdxlcd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgdxlcdBase
//
//	@doc:
//		Get column descriptor from a base type
//
//---------------------------------------------------------------------------
DrgPdxlcd *
CTranslatorUtils::PdrgdxlcdBase
	(
	IMemoryPool *pmp,
	CIdGenerator *pidgtor,
	IMDId *pmdidRetType,
	CMDName *pmdName
	)
{
	DrgPdxlcd *pdrgdxlcd = New(pmp) DrgPdxlcd(pmp);

	pmdidRetType->AddRef();
	CMDName *pmdColName = New(pmp) CMDName(pmp, pmdName->Pstr());

	CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
									(
									pmp,
									pmdColName,
									pidgtor->UlNextId(),
									INT(1) /* iAttno */,
									pmdidRetType,
									false /* fColDropped */
									);

	pdrgdxlcd->Append(pdxlcd);

	return pdrgdxlcd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgdxlcdComposite
//
//	@doc:
//		Get column descriptors from a composite type
//
//---------------------------------------------------------------------------
DrgPdxlcd *
CTranslatorUtils::PdrgdxlcdComposite
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CIdGenerator *pidgtor,
	const IMDType *pmdType
	)
{
	DrgPmdcol *pdrgPmdCol = ExpandCompositeType(pmp, pmda, pmdType);

	DrgPdxlcd *pdrgdxlcd = New(pmp) DrgPdxlcd(pmp);

	for (ULONG ul = 0; ul < pdrgPmdCol->UlLength(); ul++)
	{
		CMDColumn *pmdcol = (*pdrgPmdCol)[ul];

		CMDName *pmdColName = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		IMDId *pmdidColType = pmdcol->PmdidType();

		pmdidColType->AddRef();
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdColName,
										pidgtor->UlNextId(),
										INT(ul + 1) /* iAttno */,
										pmdidColType,
										false /* fColDropped */
										);
		pdrgdxlcd->Append(pdxlcd);
	}

	pdrgPmdCol->Release();

	return pdrgdxlcd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::ExpandCompositeType
//
//	@doc:
//		Expand a composite type into an array of IMDColumns
//
//---------------------------------------------------------------------------
DrgPmdcol *
CTranslatorUtils::ExpandCompositeType
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	const IMDType *pmdType
	)
{
	GPOS_ASSERT(NULL != pmdType);
	GPOS_ASSERT(pmdType->FComposite());

	IMDId *pmdidRel = pmdType->PmdidBaseRelation();
	const IMDRelation *pmdrel = pmda->Pmdrel(pmdidRel);
	GPOS_ASSERT(NULL != pmdrel);

	DrgPmdcol *pdrgPmdcol = New(pmp) DrgPmdcol(pmp);

	for(ULONG ul = 0; ul < pmdrel->UlColumns(); ul++)
	{
		CMDColumn *pmdcol = (CMDColumn *) pmdrel->Pmdcol(ul);

		if (!pmdcol->FSystemColumn())
		{
			pmdcol->AddRef();
			pdrgPmdcol->Append(pmdcol);
		}
	}

	return pdrgPmdcol;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::EdxljtFromJoinType
//
//	@doc:
//		Translates the join type from its GPDB representation into the DXL one
//
//---------------------------------------------------------------------------
EdxlJoinType
CTranslatorUtils::EdxljtFromJoinType
	(
	JoinType jt
	)
{
	EdxlJoinType edxljt = EdxljtSentinel;

	switch (jt)
	{
		case JOIN_INNER:
			edxljt = EdxljtInner;
			break;

		case JOIN_LEFT:
			edxljt = EdxljtLeft;
			break;

		case JOIN_FULL:
			edxljt = EdxljtFull;
			break;

		case JOIN_RIGHT:
			edxljt = EdxljtRight;
			break;

		case JOIN_IN:
			edxljt = EdxljtIn;
			break;

		case JOIN_LASJ:
			edxljt = EdxljtLeftAntiSemijoin;
			break;

		default:
			GPOS_ASSERT(!"Unrecognized join type");
	}

	GPOS_ASSERT(EdxljtSentinel > edxljt);

	return edxljt;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::EdxlIndexDirection
//
//	@doc:
//		Translates the DXL index scan direction from GPDB representation
//
//---------------------------------------------------------------------------
EdxlIndexScanDirection
CTranslatorUtils::EdxlIndexDirection
	(
	ScanDirection sd
	)
{
	EdxlIndexScanDirection edxlisd = EdxlisdSentinel;

	switch (sd)
	{
		case BackwardScanDirection:
			edxlisd = EdxlisdBackward;
			break;

		case ForwardScanDirection:
			edxlisd = EdxlisdForward;
			break;

		case NoMovementScanDirection:
			edxlisd = EdxlisdNoMovement;
			break;

		default:
			GPOS_ASSERT(!"Unrecognized index scan direction");
	}

	GPOS_ASSERT(EdxlisdSentinel > edxlisd);

	return edxlisd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Pdxlcd
//
//	@doc:
//		Find the n-th col descr entry
//
//---------------------------------------------------------------------------
const CDXLColDescr *
CTranslatorUtils::Pdxlcd
	(
	const CDXLTableDescr *pdxltabdesc,
	ULONG ulPos
	)
{
	GPOS_ASSERT(0 != ulPos);
	GPOS_ASSERT(ulPos < pdxltabdesc->UlArity());

	return pdxltabdesc->Pdxlcd(ulPos);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PstrSystemColName
//
//	@doc:
//		Return the name for the system attribute with the given attribute number.
//
//---------------------------------------------------------------------------
const CWStringConst *
CTranslatorUtils::PstrSystemColName
	(
	AttrNumber attno
	)
{
	GPOS_ASSERT(FirstLowInvalidHeapAttributeNumber < attno && 0 > attno);

	switch (attno)
	{
		case SelfItemPointerAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenCtidColName);

		case ObjectIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenOidColName);

		case MinTransactionIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenXminColName);

		case MinCommandIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenCminColName);

		case MaxTransactionIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenXmaxColName);

		case MaxCommandIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenCmaxColName);

		case TableOidAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenTableOidColName);

		case GpSegmentIdAttributeNumber:
			return CDXLTokens::PstrToken(EdxltokenGpSegmentIdColName);

		default:
			GPOS_RAISE
				(
				gpdxl::ExmaDXL,
				gpdxl::ExmiPlStmt2DXLConversion,
				GPOS_WSZ_LIT("Invalid attribute number")
				);
			return NULL;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PmdidSystemColType
//
//	@doc:
//		Return the type id for the system attribute with the given attribute number.
//
//---------------------------------------------------------------------------
CMDIdGPDB *
CTranslatorUtils::PmdidSystemColType
	(
	IMemoryPool *pmp,
	AttrNumber attno
	)
{
	GPOS_ASSERT(FirstLowInvalidHeapAttributeNumber < attno && 0 > attno);

	switch (attno)
	{
		case SelfItemPointerAttributeNumber:
			// tid type
			return New(pmp) CMDIdGPDB(GPDB_TID);

		case ObjectIdAttributeNumber:
		case TableOidAttributeNumber:
			// OID type
			return New(pmp) CMDIdGPDB(GPDB_OID);

		case MinTransactionIdAttributeNumber:
		case MaxTransactionIdAttributeNumber:
			// xid type
			return New(pmp) CMDIdGPDB(GPDB_XID);

		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			// cid type
			return New(pmp) CMDIdGPDB(GPDB_CID);

		case GpSegmentIdAttributeNumber:
			// int4
			return New(pmp) CMDIdGPDB(GPDB_INT4);

		default:
			GPOS_RAISE
				(
				gpdxl::ExmaDXL,
				gpdxl::ExmiPlStmt2DXLConversion,
				GPOS_WSZ_LIT("Invalid attribute number")
				);
			return NULL;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Scandirection
//
//	@doc:
//		Return the GPDB specific scan direction from its corresponding DXL
//		representation
//
//---------------------------------------------------------------------------
ScanDirection
CTranslatorUtils::Scandirection
	(
	EdxlIndexScanDirection edxlisd
	)
{
	if (EdxlisdBackward == edxlisd)
	{
		return BackwardScanDirection;
	}

	if (EdxlisdForward == edxlisd)
	{
		return ForwardScanDirection;
	}

	return NoMovementScanDirection;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Strategynumber
//
//	@doc:
//		Return the index strategy number for a given expression
//
//---------------------------------------------------------------------------
StrategyNumber
CTranslatorUtils::Strategynumber
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	Expr* pexpr
	)
{
	OID oidOpNo = InvalidOid;

	GPOS_ASSERT(IsA(pexpr, OpExpr) || IsA(pexpr, ScalarArrayOpExpr) || IsA(pexpr, RowCompareExpr));

	switch (pexpr->type)
	{
		case T_OpExpr:
				oidOpNo = ((OpExpr *) pexpr)->opno;
				break;

		case T_ScalarArrayOpExpr:
				oidOpNo = ((ScalarArrayOpExpr*) pexpr)->opno;
				break;

		case T_RowCompareExpr:
				oidOpNo = LInitialOID(((RowCompareExpr *) pexpr)->opnos);
				break;

		default:
			GPOS_RAISE
				(
				gpdxl::ExmaDXL,
				gpdxl::ExmiPlStmt2DXLConversion,
				GPOS_WSZ_LIT("No index strategy number found for operator")
				);
			return InvalidStrategy;
	}

	SCmptypeStrategy rgcmpsnMap[] =
	{
		{IMDType::EcmptEq, BTEqualStrategyNumber},
		{IMDType::EcmptL, BTLessStrategyNumber},
		{IMDType::EcmptLEq, BTLessEqualStrategyNumber},
		{IMDType::EcmptG, BTGreaterStrategyNumber},
		{IMDType::EcmptGEq, BTGreaterEqualStrategyNumber}
	};

 	const ULONG ulLength = GPOS_ARRAY_SIZE(rgcmpsnMap);

	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(oidOpNo);
	IMDType::ECmpType ecomptype = CUtils::Ecmpt(pmp, pmda, pmdid);
	pmdid->Release();

	StrategyNumber sn = InvalidStrategy;

	for (ULONG ul = 0; ul < ulLength; ul++)
	{
	 	SCmptypeStrategy elem = rgcmpsnMap[ul];
	 	if (ecomptype == elem.ecomptype)
	 	{
	 		sn = elem.sn;
	 	}
	}

	GPOS_ASSERT(InvalidStrategy != sn);

	return sn;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Edxlsetop
//
//	@doc:
//		Return the DXL representation of the set operation
//
//---------------------------------------------------------------------------
EdxlSetOpType
CTranslatorUtils::Edxlsetop
	(
	SetOperation setop,
	BOOL fAll
	)
{
	if (SETOP_UNION == setop && fAll)
	{
		return EdxlsetopUnionAll;
	}

	if (SETOP_INTERSECT == setop && fAll)
	{
		return EdxlsetopIntersectAll;
	}

	if (SETOP_EXCEPT == setop && fAll)
	{
		return EdxlsetopDifferenceAll;
	}

	if (SETOP_UNION == setop)
	{
		return EdxlsetopUnion;
	}

	if (SETOP_INTERSECT == setop)
	{
		return EdxlsetopIntersect;
	}

	if (SETOP_EXCEPT == setop)
	{
		return EdxlsetopDifference;
	}

	GPOS_ASSERT(!"Unrecognized set operator type");
	return EdxlsetopSentinel;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Setoptype
//
//	@doc:
//		Return set operator type
//
//---------------------------------------------------------------------------
SetOperation
CTranslatorUtils::Setoptype
	(
	EdxlSetOpType edxlsetop
	)
{
	if (EdxlsetopUnionAll == edxlsetop || EdxlsetopUnion == edxlsetop)
	{
		return SETOP_UNION;
	}

	if (EdxlsetopIntersect == edxlsetop || EdxlsetopIntersectAll == edxlsetop)
	{
		return SETOP_INTERSECT;
	}

	if (EdxlsetopDifference == edxlsetop || EdxlsetopDifferenceAll == edxlsetop)
	{
		return SETOP_EXCEPT;
	}

	GPOS_ASSERT(!"Unrecognized set operator type");

	return SETOP_NONE;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PtemapCopy
//
//	@doc:
//		Create a copy of the hash map
//
//---------------------------------------------------------------------------
TEMap *
CTranslatorUtils::PtemapCopy
	(
	IMemoryPool *pmp,
	TEMap *ptemap
	)
{
	TEMap *ptemapCopy = New(pmp) TEMap(pmp);

	// iterate over full map
	TEMapIter temapiter(ptemap);
	while (temapiter.FAdvance())
	{
		CMappingElementColIdTE *pmapelement =  const_cast<CMappingElementColIdTE *>(temapiter.Pt());

		const ULONG ulColId = pmapelement->UlColId();
		ULONG *pulKey1 = New(pmp) ULONG(ulColId);
		pmapelement->AddRef();

#ifdef GPOS_DEBUG
		BOOL fres =
#endif
		ptemapCopy->FInsert(pulKey1, pmapelement);

#ifdef GPOS_DEBUG
		GPOS_ASSERT(fres);
#endif

	}
	return ptemapCopy;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Windowexclusion
//
//	@doc:
//		Return the GPDB frame exclusion strategy from its corresponding
//		DXL representation
//
//---------------------------------------------------------------------------
WindowExclusion
CTranslatorUtils::Windowexclusion
	(
	EdxlFrameExclusionStrategy edxlfes
	)
{
	GPOS_ASSERT(EdxlfesSentinel > edxlfes);
	ULONG rgrgulMapping[][2] =
		{
		{EdxlfesNulls, WINDOW_EXCLUSION_NULL},
		{EdxlfesCurrentRow, WINDOW_EXCLUSION_CUR_ROW},
		{EdxlfesGroup, WINDOW_EXCLUSION_GROUP},
		{EdxlfesTies, WINDOW_EXCLUSION_TIES}
		};

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	WindowExclusion we = WINDOW_EXCLUSION_NO_OTHERS;

	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) edxlfes == pulElem[0])
		{
			we = (WindowExclusion) pulElem[1];
			break;
		}
	}
	return we;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Windowboundkind
//
//	@doc:
//		Return the GPDB frame boundary kind from its corresponding
//		DXL representation
//
//---------------------------------------------------------------------------
WindowBoundingKind
CTranslatorUtils::Windowboundkind
	(
	EdxlFrameBoundary edxlfb
	)
{
	GPOS_ASSERT(EdxlfbSentinel > edxlfb);

	ULONG rgrgulMapping[][2] =
			{
			{EdxlfbUnboundedPreceding, WINDOW_UNBOUND_PRECEDING},
			{EdxlfbBoundedPreceding, WINDOW_BOUND_PRECEDING},
			{EdxlfbCurrentRow, WINDOW_CURRENT_ROW},
			{EdxlfbBoundedFollowing, WINDOW_BOUND_FOLLOWING},
			{EdxlfbUnboundedFollowing, WINDOW_UNBOUND_FOLLOWING},
			{EdxlfbDelayedBoundedPreceding, WINDOW_DELAYED_BOUND_PRECEDING},
		    {EdxlfbDelayedBoundedFollowing, WINDOW_DELAYED_BOUND_FOLLOWING}
			};

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	WindowBoundingKind wbk = WINDOW_UNBOUND_PRECEDING;
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) edxlfb == pulElem[0])
		{
			wbk = (WindowBoundingKind) pulElem[1];
			break;
		}
	}
	GPOS_ASSERT(WINDOW_DELAYED_BOUND_FOLLOWING >= wbk && "Invalid window frame boundary");

	return wbk;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgpulGroupingCols
//
//	@doc:
//		Construct a dynamic array of column ids for the given set of grouping
// 		col attnos
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorUtils::PdrgpulGroupingCols
	(
	IMemoryPool *pmp,
	CBitSet *pbsGroupByCols,
	HMIUl *phmiulSortGrpColsColId
	)
{
	DrgPul *pdrgpul = New (pmp) DrgPul(pmp);

	if (NULL != pbsGroupByCols)
	{
		CBitSetIter bsi(*pbsGroupByCols);

		while (bsi.FAdvance())
		{
			const ULONG ulColId = UlColId(bsi.UlBit(), phmiulSortGrpColsColId);
			pdrgpul->Append(New(pmp) ULONG(ulColId));
		}
	}

	return pdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgpbsGroupBy
//
//	@doc:
//		Construct a dynamic array of sets of column attnos corresponding to the
// 		group by clause
//
//---------------------------------------------------------------------------
DrgPbs *
CTranslatorUtils::PdrgpbsGroupBy
	(
	IMemoryPool *pmp,
	List *plGroupClause,
	ULONG ulCols
	)
{
	GPOS_ASSERT(NULL != plGroupClause);
	GPOS_ASSERT(0 < gpdb::UlListLength(plGroupClause));

	Node *pnode = (Node*) LInitial(plGroupClause);

	if (NULL == pnode || IsA(pnode, GroupClause))
	{
		// simple group by
		CBitSet *pbsGroupingSet = PbsGroupingSet(pmp, plGroupClause, ulCols);
		DrgPbs *pdrgpbs = New(pmp) DrgPbs(pmp);
		pdrgpbs->Append(pbsGroupingSet);
		return pdrgpbs;
	}

	if (!IsA(pnode, GroupingClause))
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Group by clause"));
	}

	// grouping sets
	if (1 != gpdb::UlListLength(plGroupClause))
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Multiple grouping sets specifications"));
	}

	GroupingClause *pgrcl = (GroupingClause *) pnode;

	if (GROUPINGTYPE_ROLLUP == pgrcl->groupType)
	{
		return PdrgpbsRollup(pmp, pgrcl, ulCols);
	}

	if (GROUPINGTYPE_GROUPING_SETS != pgrcl->groupType)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Rollup and cube"));
	}

	DrgPbs *pdrgpbs = New(pmp) DrgPbs(pmp);

	ListCell *plcGroupingSet = NULL;
	ForEach (plcGroupingSet, pgrcl->groupsets)
	{
		Node *pnodeGroupingSet = (Node *) lfirst(plcGroupingSet);

		CBitSet *pbs = NULL;
		if (IsA(pnodeGroupingSet, GroupClause))
		{
			// grouping set contains a single grouping column
			pbs = New(pmp) CBitSet(pmp, ulCols);
			pbs->FExchangeSet(((GroupClause *) pnodeGroupingSet)->tleSortGroupRef);
		}
		else if (IsA(pnodeGroupingSet, GroupingClause))
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Rollup and cube"));
		}
		else
		{
			// grouping set contains a list of columns
			GPOS_ASSERT(IsA(pnodeGroupingSet, List));

			List *plGroupingSet = (List *) pnodeGroupingSet;
			pbs = PbsGroupingSet(pmp, plGroupingSet, ulCols);
		}
		pdrgpbs->Append(pbs);
	}

	return pdrgpbs;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgpbsRollup
//
//	@doc:
//		Construct a dynamic array of sets of column attnos for a rollup
//
//---------------------------------------------------------------------------
DrgPbs *
CTranslatorUtils::PdrgpbsRollup
	(
	IMemoryPool *pmp,
	GroupingClause *pgrcl,
	ULONG ulCols
	)
{
	GPOS_ASSERT(NULL != pgrcl);

	// construct an array of columns
	DrgPul *pdrgpulGroupingCols = New(pmp) DrgPul(pmp);
	ListCell *plcGroupingSet = NULL;
	ForEach (plcGroupingSet, pgrcl->groupsets)
	{
		GPOS_ASSERT(IsA((Node *) lfirst(plcGroupingSet), GroupClause));

		GroupClause *pgrpcl = (GroupClause *) lfirst(plcGroupingSet);
		pdrgpulGroupingCols->Append(New(pmp) ULONG(pgrpcl->tleSortGroupRef));
	}

	const ULONG ulGroupingCols = pdrgpulGroupingCols->UlLength();
	DrgPbs *pdrgpbs = New(pmp) DrgPbs(pmp);

	// compute prefixes of the array of columns
	for (ULONG ulPrefix = 0; ulPrefix <= ulGroupingCols; ulPrefix++)
	{
		CBitSet *pbs = New(pmp) CBitSet(pmp, ulCols);
		for (ULONG ulIdx = 0; ulIdx < ulPrefix; ulIdx++)
		{
			ULONG ulCol = *((*pdrgpulGroupingCols)[ulIdx]);
			pbs->FExchangeSet(ulCol);
		}
		pdrgpbs->Append(pbs);
	}
	pdrgpulGroupingCols->Release();
	return pdrgpbs;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PbsGroupingSet
//
//	@doc:
//		Construct a set of column attnos corresponding to a grouping set
//
//---------------------------------------------------------------------------
CBitSet *
CTranslatorUtils::PbsGroupingSet
	(
	IMemoryPool *pmp,
	List *plGroupElems,
	ULONG ulCols
	)
{
	GPOS_ASSERT(NULL != plGroupElems);
	GPOS_ASSERT(0 < gpdb::UlListLength(plGroupElems));

	CBitSet *pbs = New(pmp) CBitSet(pmp, ulCols);

	ListCell *plc = NULL;
	ForEach (plc, plGroupElems)
	{
		Node *pnodeElem = (Node*) lfirst(plc);

		if (NULL == pnodeElem)
		{
			continue;
		}

		if (!IsA(pnodeElem, GroupClause))
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Mixing grouping sets with simple group by lists"));
		}

		pbs->FExchangeSet(((GroupClause *) pnodeElem)->tleSortGroupRef);
	}

	return pbs;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::Pdrgpdxlcd
//
//	@doc:
//		Construct an array of DXL column descriptors for a target list using the
// 		column ids in the given array
//
//---------------------------------------------------------------------------
DrgPdxlcd *
CTranslatorUtils::Pdrgpdxlcd
	(
	IMemoryPool *pmp,
	List *plTargetList,
	DrgPul *pdrgpulColIds
	)
{
	GPOS_ASSERT(NULL != plTargetList);
	GPOS_ASSERT(NULL != pdrgpulColIds);

	ListCell *plcTE = NULL;
	DrgPdxlcd *pdrgpdxlcd = New(pmp) DrgPdxlcd(pmp);
	ULONG ul = 0;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);

		if (pte->resjunk)
		{
			continue;
		}

		CMDName *pmdname = NULL;
		if (NULL == pte->resname)
		{
			CWStringConst strUnnamedCol(GPOS_WSZ_LIT("?column?"));
			pmdname = New(pmp) CMDName(pmp, &strUnnamedCol);
		}
		else
		{
			CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(pmp, pte->resname);
			pmdname = New(pmp) CMDName(pmp, pstrAlias);
			// CName constructor copies string
			delete pstrAlias;
		}

		// create a column descriptor
		OID oidType = gpdb::OidExprType((Node *) pte->expr);
		CMDIdGPDB *pmdidColType = New(pmp) CMDIdGPDB(oidType);
		ULONG ulColId = *(*pdrgpulColIds)[ul];
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdname,
										ulColId,
										ul + 1, /* attno */
										pmdidColType,
										false /* fColDropped */
										);
		pdrgpdxlcd->Append(pdxlcd);
		ul++;
	}

	GPOS_ASSERT(pdrgpdxlcd->UlLength() == pdrgpulColIds->UlLength());

	return pdrgpdxlcd;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdrgpulColIds
//
//	@doc:
//		Construct an array of colids for the given target list
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorUtils::PdrgpulColIds
	(
	IMemoryPool *pmp,
	List *plTargetList,
	HMIUl *phmiulAttnoColId
	)
{
	GPOS_ASSERT(NULL != plTargetList);
	GPOS_ASSERT(NULL != phmiulAttnoColId);

	DrgPul *pdrgpul = New(pmp) DrgPul(pmp);

	ListCell *plcTE = NULL;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		ULONG ulResNo = (ULONG) pte->resno;
		if (!pte->resjunk)
		{
			INT iAttno = (INT) pte->resno;
			const ULONG *pul = phmiulAttnoColId->PtLookup(&iAttno);

			if (NULL == pul)
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLAttributeNotFound, ulResNo);
			}

			pdrgpul->Append(New(pmp) ULONG(*pul));
		}
	}

	return pdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::UlColId
//
//	@doc:
//		Return the corresponding ColId for the given index into the target list
//
//---------------------------------------------------------------------------
ULONG
CTranslatorUtils::UlColId
	(
	INT iIndex,
	HMIUl *phmiulColId
	)
{
	GPOS_ASSERT(0 < iIndex);

	const ULONG *pul = phmiulColId->PtLookup(&iIndex);

	if (NULL == pul)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLAttributeNotFound, iIndex);
	}

	return *pul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::UlColId
//
//	@doc:
//		Return the corresponding ColId for the given varno, varattno and querylevel
//
//---------------------------------------------------------------------------
ULONG
CTranslatorUtils::UlColId
	(
	ULONG ulQueryLevel,
	INT iVarno,
	INT iVarAttno,
	IMDId *pmdid,
	CMappingVarColId *pmapvarcolid
	)
{
	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	Var *pvar = gpdb::PvarMakeVar(iVarno, iVarAttno, oid, -1, 0);
	ULONG ulColId = pmapvarcolid->UlColId(ulQueryLevel, pvar, EpspotNone);
	gpdb::GPDBFree(pvar);

	return ulColId;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FWindowSpec
//
//	@doc:
//		Check if the TargetEntry is a used in the window specification
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FWindowSpec
	(
	const TargetEntry *pte,
	const List *plWindowClause
	)
{
	ListCell *plcWindowCl = NULL;
	ForEach (plcWindowCl, plWindowClause)
	{
		WindowSpec *pwindowspec = (WindowSpec*) lfirst(plcWindowCl);
		if (FSortingColumn(pte, pwindowspec->order) || FSortingColumn(pte, pwindowspec->partition))
		{
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PdxlnInt4Const
//
//	@doc:
// 		Construct a scalar const value expression for the given INT value
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorUtils::PdxlnInt4Const
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	INT iVal
	)
{
	GPOS_ASSERT(NULL != pmp);
	const IMDTypeInt4 *pmdtypeint4 = pmda->PtMDType<IMDTypeInt4>();
	pmdtypeint4->Pmdid()->AddRef();

	CDXLDatumInt4 *pdxldatum = New(pmp) CDXLDatumInt4(pmp, pmdtypeint4->Pmdid(), false /*fConstNull*/, iVal);

	CDXLScalarConstValue *pdxlConst = New(pmp) CDXLScalarConstValue(pmp, pdxldatum);

	return New(pmp) CDXLNode(pmp, pdxlConst);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FSortingColumn
//
//	@doc:
//		Check if the TargetEntry is a sorting column
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FSortingColumn
	(
	const TargetEntry *pte,
	const List *plSortCl
	)
{
	ListCell *plcSortCl = NULL;
	ForEach (plcSortCl, plSortCl)
	{
		Node *pnodeSortCl = (Node*) lfirst(plcSortCl);
		if (IsA(pnodeSortCl, SortClause) && pte->ressortgroupref == ((SortClause *) pnodeSortCl)->tleSortGroupRef)
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FGroupingColumn
//
//	@doc:
//		Check if the TargetEntry is a grouping column
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FGroupingColumn
	(
	const TargetEntry *pte,
	const List *plGrpCl
	)
{
	ListCell *plcGrpCl = NULL;
	ForEach (plcGrpCl, plGrpCl)
	{
		Node *pnodeGrpCl = (Node*) lfirst(plcGrpCl);

		if (NULL == pnodeGrpCl)
		{
			continue;
		}

		if (IsA(pnodeGrpCl, GroupClause) && FGroupingColumn(pte, (GroupClause*) pnodeGrpCl))
		{
			return true;
		}

		if (IsA(pnodeGrpCl, GroupingClause))
		{
			GroupingClause *pgrcl = (GroupingClause *) pnodeGrpCl;

			ListCell *plcGroupingSet = NULL;
			ForEach (plcGroupingSet, pgrcl->groupsets)
			{
				Node *pnodeGroupingSet = (Node *) lfirst(plcGroupingSet);

				if (IsA(pnodeGroupingSet, GroupClause) && FGroupingColumn(pte, ((GroupClause *) pnodeGroupingSet)))
				{
					return true;
				}

				if (IsA(pnodeGroupingSet, List) && FGroupingColumn(pte, (List *) pnodeGroupingSet))
				{
					return true;
				}
			}
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FGroupingColumn
//
//	@doc:
//		Check if the TargetEntry is a grouping column
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FGroupingColumn
	(
	const TargetEntry *pte,
	const GroupClause *pgrcl
	)
{
	GPOS_ASSERT(NULL != pgrcl);

	return (pte->ressortgroupref == pgrcl->tleSortGroupRef);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::FGroupingColumn
//
//	@doc:
//		Check if the sorting column is a grouping column
//---------------------------------------------------------------------------
BOOL
CTranslatorUtils::FGroupingColumn
	(
	const SortClause *psortcl,
	const List *plGrpCl
	)
{
	ListCell *plcGrpCl = NULL;
	ForEach (plcGrpCl, plGrpCl)
	{
		Node *pnodeGrpCl = (Node*) lfirst(plcGrpCl);
		GPOS_ASSERT(IsA(pnodeGrpCl, GroupClause) && "We currently do not support grouping sets.");

		GroupClause *pgrpcl = (GroupClause*) pnodeGrpCl;
		if (psortcl->tleSortGroupRef == pgrpcl->tleSortGroupRef)
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::PlAttnosFromColids
//
//	@doc:
//		Translate an array of colids to a list of attribute numbers using
//		the mappings in the provided context
//---------------------------------------------------------------------------
List *
CTranslatorUtils::PlAttnosFromColids
	(
	DrgPul *pdrgpul,
	CDXLTranslateContext *pdxltrctx
	)
{
	GPOS_ASSERT(NULL != pdrgpul);
	GPOS_ASSERT(NULL != pdxltrctx);
	
	List *plResult = NIL;
	
	const ULONG ulLength = pdrgpul->UlLength();
	for (ULONG ul = 0; ul < ulLength; ul++)
	{
		ULONG ulColId = *((*pdrgpul)[ul]);
		const TargetEntry *pte = pdxltrctx->Pte(ulColId);
		GPOS_ASSERT(NULL != pte);
		plResult = gpdb::PlAppendInt(plResult, pte->resno);
	}
	
	return plResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::IFromStr
//
//	@doc:
//		Parses an integer value from a string
//
//---------------------------------------------------------------------------
INT
CTranslatorUtils::IFromStr
	(
	const CWStringBase *pstr
	)
{
	CHAR *sz = SzFromWsz(pstr->Wsz());
	CHAR *pcEnd = NULL;
	return (INT) gpos::clib::LStrToL(sz, &pcEnd, 10);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorUtils::SzFromWsz
//
//	@doc:
//		Converts a wide character string into a character array
//
//---------------------------------------------------------------------------
CHAR *
CTranslatorUtils::SzFromWsz
	(
	const WCHAR *wsz
	)
{
	GPOS_ASSERT(NULL != wsz);

	ULONG ulMaxLength = GPOS_WSZ_LENGTH(wsz) * GPOS_SIZEOF(WCHAR) + 1;
	CHAR *sz = (CHAR *) gpdb::GPDBAlloc(ulMaxLength);
#ifdef GPOS_DEBUG
	LINT li = (INT)
#endif
	clib::LWcsToMbs(sz, const_cast<WCHAR *>(wsz), ulMaxLength);
	GPOS_ASSERT(0 <= li);

	sz[ulMaxLength - 1] = '\0';

	return sz;
}

// EOF
