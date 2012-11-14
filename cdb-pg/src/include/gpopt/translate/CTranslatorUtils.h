//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CTranslatorUtils.h
//
//	@doc:
//		Class providing utility methods for translating GPDB's PlannedStmt/Query
//		into DXL Tree
//
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CTranslatorUtils_H
#define GPDXL_CTranslatorUtils_H
#define GPDXL_SYSTEM_COLUMNS 8

#include "gpopt/translate/CTranslatorScalarToDXL.h"
#include "gpopt/translate/CMappingColIdVarQuery.h"

#include "gpos/base.h"
#include "gpos/common/CBitSet.h"

#include "dxl/operators/dxlops.h"
#include "dxl/CIdGenerator.h"

#include "nodes/parsenodes.h"
#include "access/sdir.h"
#include "access/skey.h"

#include "md/CMDRelationGPDB.h"
#include "md/IMDType.h"

// fwd declarations
namespace gpopt
{
	class CMDAccessor;

	// dynamic array of bitsets
	typedef CDynamicPtrArray<CBitSet, CleanupRelease> DrgPbs;
}

namespace gpdxl
{
	class CDXLTranslateContext;
}

namespace gpdxl
{
	using namespace gpopt;

	// hash maps mapping INT -> ULONG
	typedef CHashMap<INT, ULONG, gpos::UlHash<INT>, gpos::FEqual<INT>,
					CleanupDelete<INT>, CleanupDelete<ULONG> > HMIUl;

	typedef CHashMap<ULONG, ULONG, gpos::UlHash<ULONG>, gpos::FEqual<ULONG>,
					CleanupDelete<ULONG>, CleanupDelete<ULONG> > HMUlUl;

	//---------------------------------------------------------------------------
	//	@class:
	//		CTranslatorUtils
	//
	//	@doc:
	//		Class providing methods for translating GPDB's PlannedStmt/Query
	//		into DXL Tree
	//
	//---------------------------------------------------------------------------
	class CTranslatorUtils
	{
		private:

			// construct a set of column attnos corresponding to a single grouping set
			static
			CBitSet *PbsGroupingSet(IMemoryPool *pmp, List *plGroupElems, ULONG ulCols);

			// create a set of grouping sets for a rollup
			static DrgPbs *PdrgpbsRollup(IMemoryPool *pmp, GroupingClause *pgrcl, ULONG ulCols);

		public:

			typedef struct CContextPreloadMD
			{
				public:
					// memory pool
					IMemoryPool *m_pmp;

					// MD accessor for function names
					CMDAccessor *m_pmda;

					CContextPreloadMD
						(
						IMemoryPool *pmp,
						CMDAccessor *pmda
						)
						: m_pmp(pmp), m_pmda(pmda)
					{}

					~CContextPreloadMD()
					{}

			} CContextPreloadMD;

			struct SCmptypeStrategy
			{
				IMDType::ECmpType ecomptype;
				StrategyNumber sn;

			};

			// get the GPDB scan direction from its corresponding DXL representation
			static
			ScanDirection Scandirection(EdxlIndexScanDirection edxlisd);

			// get the index strategy number for a given expression
			static
			StrategyNumber Strategynumber(IMemoryPool *pmp, CMDAccessor *pmda, Expr* pexpr);


			// return the type for the system column with the given number
			static
			CMDIdGPDB *PmdidSystemColType(IMemoryPool *pmp, AttrNumber attno);

			// find the n-th column descriptor in the table descriptor
			static
			const CDXLColDescr *Pdxlcd(const CDXLTableDescr *pdxltabdesc, ULONG ulPos);

			// return the name for the system column with given number
			static
			const CWStringConst *PstrSystemColName(AttrNumber attno);

			// translate the join type from its GPDB representation into the DXL one
			static
			EdxlJoinType EdxljtFromJoinType(JoinType jt);

			// translate the index scan direction from its GPDB representation into the DXL one
			static
			EdxlIndexScanDirection EdxlIndexDirection(ScanDirection sd);

			// create a DXL index descriptor from an index MD id
			static
			CDXLIndexDescr *Pdxlid(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// translate a RangeTableEntry into a CDXLTableDescr
			static
			CDXLTableDescr *Pdxltabdesc
								(
								IMemoryPool *pmp,
								CMDAccessor *pmda,
								CIdGenerator *pidgtor,
								const RangeTblEntry *prte
								);

			// translate a RangeTableEntry into a CDXLLogicalTVF
			static
			CDXLLogicalTVF *Pdxltvf
								(
								IMemoryPool *pmp,
								CMDAccessor *pmda,
								CIdGenerator *pidgtor,
								const RangeTblEntry *prte
								);

			// get column descriptors from a record type
			static
			DrgPdxlcd *PdrgdxlcdRecord
						(
						IMemoryPool *pmp,
						CIdGenerator *pidgtor,
						List *plColNames,
						List *plColTypes
						);

			// get column descriptors from a record type
			static
			DrgPdxlcd *PdrgdxlcdRecord
						(
						IMemoryPool *pmp,
						CIdGenerator *pidgtor,
						List *plColNames,
						DrgPmdid *pdrgpmdidOutArgTypes
						);

			// get column descriptor from a base type
			static
			DrgPdxlcd *PdrgdxlcdBase
						(
						IMemoryPool *pmp,
						CIdGenerator *pidgtor,
						IMDId *pmdidRetType,
						CMDName *pmdName
						);

			// get column descriptors from a composite type
			static
			DrgPdxlcd *PdrgdxlcdComposite
						(
						IMemoryPool *pmp,
						CMDAccessor *pmda,
						CIdGenerator *pidgtor,
						const IMDType *pmdType
						);

			// expand a composite type into an array of IMDColumns
			static
			DrgPmdcol *ExpandCompositeType
						(
						IMemoryPool *pmp,
						CMDAccessor *pmda,
						const IMDType *pmdType
						);

			// preload metadata for a given type
			static
			void PreloadMDType(CMDAccessor *pmda, const IMDType *pmdtype);

			// preload helpers
			static
			BOOL FPreloadMDStatsWalker(Node *pnode, CContextPreloadMD *pstrtxpreloadmd);

			static
			void PreloadMDStats(IMemoryPool *pmp, CMDAccessor *pmda, OID oidRelation);

			// preload basic information in the MD cache, including base types
			// and MD objects referenced in the given query
			static
			void PreloadMD(IMemoryPool *pmp, CMDAccessor *pmda, CSystemId sysid, Query *pquery);

			// return the dxl representation of the set operation
			static
			EdxlSetOpType Edxlsetop(SetOperation setop, BOOL fAll);

			// make copy of the TE map
			static
			TEMap *PtemapCopy(IMemoryPool *pmp, TEMap *ptemap);

			// return the set operator type
			static
			SetOperation Setoptype(EdxlSetOpType edxlsetop);

			// return the GPDB frame exclusion strategy from its corresponding DXL representation
			static
			WindowExclusion Windowexclusion(EdxlFrameExclusionStrategy edxlfes);

			// return the GPDB frame boundary kind from its corresponding DXL representation
			static
			WindowBoundingKind Windowboundkind(EdxlFrameBoundary edxlfb);

			// construct a dynamic array of sets of column attnos corresponding
			// to the group by clause
			static
			DrgPbs *PdrgpbsGroupBy(IMemoryPool *pmp, List *plGroupClause, ULONG ulCols);


			// construct an array of DXL column descriptors for a target list
			// using the column ids in the given array
			static
			DrgPdxlcd *Pdrgpdxlcd(IMemoryPool *pmp, List *plTargetList, DrgPul *pdrgpulColIds);

			// construct a list of colids corresponding to the given target list
			// using the given attno->colid map
			static
			DrgPul *PdrgpulColIds(IMemoryPool *pmp, List *plTargetList, HMIUl *phmiulAttnoColId);

			// construct an array of column ids for the given group by set
			static
			DrgPul *PdrgpulGroupingCols(IMemoryPool *pmp, CBitSet *pbsGroupByCols, HMIUl *phmiulSortGrpColsColId);

			// return the Colid of column with given index
			static
			ULONG UlColId(INT iIndex, HMIUl *phmiul);

			// return the corresponding ColId for the given varno, varattno and querylevel
			static
			ULONG UlColId(ULONG ulQueryLevel, INT iVarno, INT iVarAttno, IMDId *pmdid, CMappingVarColId *pmapvarcolid);

			// check to see if the target list entry is a sorting column
			static
			BOOL FSortingColumn(const TargetEntry *pte, const List *plSortCl);

			// check to see if the target list entry is used in the window reference
			static
			BOOL FWindowSpec(const TargetEntry *pte, const List *plWindowClause);

			// create a scalar const value expression for the given int4 value
			static
			CDXLNode *PdxlnInt4Const(IMemoryPool *pmp, CMDAccessor *pmda, INT iVal);

			// check to see if the target list entry is a grouping column
			static
			BOOL FGroupingColumn(const TargetEntry *pte, const List *plGrpCl);

			// check to see if the target list entry is a grouping column
			static
			BOOL FGroupingColumn(const TargetEntry *pte, const GroupClause *pgrcl);

			// check to see if the sorting column entry is a grouping column
			static
			BOOL FGroupingColumn(const SortClause *psortcl, const List *plGrpCl);

			// convert a list of column ids to a list of attribute numbers using
			// the provided context with mappings
			static
			List *PlAttnosFromColids(DrgPul *pdrgpul, CDXLTranslateContext *pdxltrctx);
			
			// parse string value into an Integer
			static
			INT IFromStr(const CWStringBase *pstr);


			// create a multi-byte character string from a wide character string
			static
			CHAR *SzFromWsz(const WCHAR *wsz);
	};
}

#endif // !GPDXL_CTranslatorUtils_H

// EOF
