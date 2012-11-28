//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CContextDXLToPlStmt.h
//
//	@doc:
//		Class providing access to CIdGenerators (needed to number initplans, motion
//		nodes as well as params), list of RangeTableEntires and Subplans
//		generated so far during DXL-->PlStmt translation.
//
//	@owner:
//		raghav
//
//	@test:
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CContextDXLToPlStmt_H
#define GPDXL_CContextDXLToPlStmt_H

#include "gpopt/translate/CDXLTranslateContext.h"
#include "gpopt/translate/CDXLTranslateContextBaseTable.h"
#include "gpos/base.h"

#include "dxl/operators/CDXLScalarIdent.h"
#include "dxl/gpdb_types.h"

#include "dxl/CIdGenerator.h"

// fwd decl
struct RangeTblEntry;
struct Plan;

struct List;
struct Var;

namespace gpdxl
{

	// fwd decl
	class CDXLTranslateContext;

	typedef CHashMap<ULONG, CDXLTranslateContext, gpos::UlHash<ULONG>, gpos::FEqual<ULONG>,
			CleanupDelete<ULONG>, CleanupDelete<CDXLTranslateContext> > HMUlDxltrctx;




	//---------------------------------------------------------------------------
	//	@class:
	//		CContextDXLToPlStmt
	//
	//	@doc:
	//		Class providing access to CIdGenerators (needed to number initplans, motion
	//		nodes as well as params), list of RangeTableEntires and Subplans
	//		generated so far during DXL-->PlStmt translation.
	//
	//---------------------------------------------------------------------------
	class CContextDXLToPlStmt
	{
		private:

			IMemoryPool *m_pmp;

			// mappings for share scan nodes
			HMUlDxltrctx *m_phmuldxltrctxSharedScan;

			// counter for generating plan ids
			CIdGenerator *m_pidgtorPlan;

			// counter for generating motion ids
			CIdGenerator *m_pulidgtorMotion;

			// counter for generating unique param ids
			CIdGenerator *m_pulidgtorParam;

			// list of all rtable entries
			List **m_pplRTable;

			// list of oids of partitioned tables
			List *m_plPartitionTables;

			// list of all subplan entries
			List **m_pplSubPlan;

			// index of the target relation in the rtable or 0 if not a DML statement
			ULONG m_ulResultRelation;

		public:
			// ctor/dtor
			CContextDXLToPlStmt
						(
						IMemoryPool *pmp,
						CIdGenerator *,
						CIdGenerator *,
						CIdGenerator *,
						List **plRTable,
						List **plSubPlan
						)
						;

			// dtor
			~CContextDXLToPlStmt();

			const CDXLTranslateContext *PdxltrctxForSharedScan(ULONG ulSpoolId);

			void AddSharedScanTranslationContext(ULONG ulSpoolId, CDXLTranslateContext *pdxltrctx);

			ULONG UlNextPlanId();

			ULONG UlCurrentMotionId();

			ULONG UlNextMotionId();

			ULONG UlCurrentParamId();

			ULONG UlNextParamId();

			// return list of range table entries
			List *PlPrte();

			// return list of partitioned table indexes
			List *PlPartitionedTables() const
			{
				return m_plPartitionTables;
			}

			List *PlPplanSubplan();

			// index of result relation in the rtable
			ULONG UlResultRelation() const
			{
				return m_ulResultRelation;
			}

			// add a range table entry
			void AddRTE(RangeTblEntry *prte, BOOL fResultRelation = false);

			// add a partitioned table index
			void AddPartitionedTable(OID oid);

			void AddSubplan(Plan * );
	};

	}
#endif // !GPDXL_CContextDXLToPlStmt_H

//EOF
