//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CContextDXLToPlStmt.cpp
//
//	@doc:
//		Implementation of the functions that provide
//		access to CIdGenerators (needed to number initplans, motion
//		nodes as well as params), list of RangeTableEntires and Subplans
//		generated so far during DXL-->PlStmt translation.
//
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "postgres.h"
#include "nodes/makefuncs.h"

#include "gpopt/translate/CContextDXLToPlStmt.h"
#include "gpopt/gpdbwrappers.h"
#include "gpos/base.h"
using namespace gpdxl;

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::CContextDXLToPlStmt
//
//	@doc:
//		Constructor
//
//---------------------------------------------------------------------------
CContextDXLToPlStmt::CContextDXLToPlStmt
	(
	IMemoryPool *pmp,
	CIdGenerator *pulidgtorPlan,
	CIdGenerator *pulidgtorMotion,
	CIdGenerator *pulidgtorParam,
	List **plRTable,
	List **plSubPlan
	)
	:m_pmp(pmp),
	m_pidgtorPlan(pulidgtorPlan),
	m_pulidgtorMotion(pulidgtorMotion),
	m_pulidgtorParam(pulidgtorParam),
	m_pplRTable(plRTable),
	m_plPartitionTables(NULL),
	m_pplSubPlan(plSubPlan),
	m_ulResultRelation(0)
{
	m_phmuldxltrctxSharedScan = New(m_pmp) HMUlDxltrctx(m_pmp);
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::~CContextDXLToPlStmt
//
//	@doc:
//		Destructor
//
//---------------------------------------------------------------------------
CContextDXLToPlStmt::~CContextDXLToPlStmt()
{
	m_phmuldxltrctxSharedScan->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::UlNextPlanId
//
//	@doc:
//		Get the next plan id
//
//---------------------------------------------------------------------------
ULONG
CContextDXLToPlStmt::UlNextPlanId()
{
	return m_pidgtorPlan->UlNextId();
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::UlCurrentMotionId
//
//	@doc:
//		Get the current motion id
//
//---------------------------------------------------------------------------
ULONG
CContextDXLToPlStmt::UlCurrentMotionId()
{
	return m_pulidgtorMotion->UlCurrentId();
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::UlNextMotionId
//
//	@doc:
//		Get the next motion id
//
//---------------------------------------------------------------------------
ULONG
CContextDXLToPlStmt::UlNextMotionId()
{
	return m_pulidgtorMotion->UlNextId();
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::UlNextParamId
//
//	@doc:
//		Get the next plan id
//
//---------------------------------------------------------------------------
ULONG
CContextDXLToPlStmt::UlNextParamId()
{
	return m_pulidgtorParam->UlNextId();
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::UlCurrentParamId
//
//	@doc:
//		Get the current param id
//
//---------------------------------------------------------------------------
ULONG
CContextDXLToPlStmt::UlCurrentParamId()
{
	return m_pulidgtorParam->UlCurrentId();
}


//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::PlPrte
//
//	@doc:
//		Return the list of RangeTableEntries
//
//---------------------------------------------------------------------------
List *
CContextDXLToPlStmt::PlPrte()
{
	return (*(m_pplRTable));
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::PlPplanSubplan
//
//	@doc:
//		Return the list of subplans generated so far
//
//---------------------------------------------------------------------------
List *
CContextDXLToPlStmt::PlPplanSubplan()
{
	return (*(m_pplSubPlan));
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::AddRTE
//
//	@doc:
//		Add a RangeTableEntries
//
//---------------------------------------------------------------------------
void
CContextDXLToPlStmt::AddRTE
	(
	RangeTblEntry *prte,
	BOOL fResultRelation
	)
{
	(* (m_pplRTable)) = gpdb::PlAppendElement((*(m_pplRTable)), prte);

	prte->inFromCl = true;

	if (fResultRelation)
	{
		GPOS_ASSERT(0 == m_ulResultRelation && "Only one result relation supported");
		prte->inFromCl = false;
		m_ulResultRelation = gpdb::UlListLength(*(m_pplRTable));
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::AddPartitionedTable
//
//	@doc:
//		Add a partitioned table oid
//
//---------------------------------------------------------------------------
void
CContextDXLToPlStmt::AddPartitionedTable
	(
	OID oid
	)
{
	m_plPartitionTables = gpdb::PlAppendOid(m_plPartitionTables, oid);
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::AddSubplan
//
//	@doc:
//		Add a subplan
//
//---------------------------------------------------------------------------
void
CContextDXLToPlStmt::AddSubplan(Plan *pplan)
{
	(* (m_pplSubPlan)) = gpdb::PlAppendElement((*(m_pplSubPlan)), pplan);
}


//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::PdxltrctxForSharedScan
//
//	@doc:
//		Returns the context for the shared scan with the given spool id
//
//---------------------------------------------------------------------------
const CDXLTranslateContext *
CContextDXLToPlStmt::PdxltrctxForSharedScan
	(
	ULONG ulSpoolId
	)
{
	const CDXLTranslateContext *pdxltrctx = m_phmuldxltrctxSharedScan->PtLookup(&ulSpoolId);
	return pdxltrctx;
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::AddSharedScanTranslationContext
//
//	@doc:
//		Stores the context for the shared scan with the given spool id
//
//---------------------------------------------------------------------------
void
CContextDXLToPlStmt::AddSharedScanTranslationContext
	(
	ULONG ulSpoolId,
	CDXLTranslateContext *pdxltrctx
	)
{
	ULONG *pul = New(m_pmp) ULONG(ulSpoolId);

#ifdef GPOS_DEBUG
	BOOL fInserted =
#endif
	m_phmuldxltrctxSharedScan->FInsert(pul, pdxltrctx);

	GPOS_ASSERT(fInserted);
}

// EOF
