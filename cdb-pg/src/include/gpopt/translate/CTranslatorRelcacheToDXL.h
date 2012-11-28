//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CTranslatorRelcacheToDXL.h
//
//	@doc:
//		Class for translating GPDB's relcache entries into DXL MD objects
//
//	@owner:
//		antovl
//
//	@test:
//
//
//---------------------------------------------------------------------------



#ifndef GPDXL_CTranslatorRelcacheToDXL_H
#define GPDXL_CTranslatorRelcacheToDXL_H

#include "gpos/base.h"
#include "c.h"
#include "postgres.h"
#include "access/tupdesc.h"
#include "catalog/gp_policy.h"
#include "dxl/gpdb_types.h"

#include "md/IMDId.h"
#include "md/CMDRelationGPDB.h"
#include "md/CMDAggregateGPDB.h"
#include "md/CMDFunctionGPDB.h"
#include "md/CMDTriggerGPDB.h"
#include "md/CMDCheckConstraintGPDB.h"
#include "md/CMDScalarOpGPDB.h"
#include "md/IMDType.h"
#include "md/CDXLColStats.h"
#include "statistics/CHistogram.h"
#include "md/IMDIndex.h"

// fwd decl
struct HeapTupleData;

namespace gpdxl
{
	using namespace gpos;
	using namespace gpmd;

	//---------------------------------------------------------------------------
	//	@class:
	//		CTranslatorRelcacheToDXL
	//
	//	@doc:
	//		Class for translating GPDB's relcache entries into DXL MD objects
	//
	//---------------------------------------------------------------------------
	class CTranslatorRelcacheToDXL
	{
		private:

			// get type name from the relcache
			static
			CMDName *PmdnameType(IMemoryPool *pmp, IMDId *pmdid);

			// get function stability property from the GPDB character representation
			static
			CMDFunctionGPDB::EFuncStbl EFuncStability(CHAR c);

			// get function data access property from the GPDB character representation
			static
			CMDFunctionGPDB::EFuncDataAcc EFuncDataAccess(CHAR c);

			// get type of aggregate's intermediate result from the relcache
			static
			IMDId *PmdidAggIntermediateResultType(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve a GPDB metadata object from the relcache
			static
			IMDCacheObject *PimdobjGPDB(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// retrieve relstats object from the relcache
			static
			IMDCacheObject *PimdobjRelStats(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve column stats object from the relcache
			static
			IMDCacheObject *PimdobjColStats(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// transform mcv information to optimizer's histogram structure
			static
			CHistogram *PhistTransformGPDBMCV
								(
								IMemoryPool *pmp,
								const IMDType *pmdtype,
								const Datum *pdrgdatumMCVValues,
								const float4 *pdrgfMCVFrequencies,
								ULONG ulNumMCVValues,
								CDouble dNullFrequency
								);

			// transform GPDB's hist information to optimizer's histogram structure
			static
			CHistogram *PhistTransformGPDBHist
								(
								IMemoryPool *pmp,
								const IMDType *pmdtype,
								const Datum *pdrgdatumHistValues,
								ULONG ulNumHistValues,
								CDouble dDistinctHist,
								CDouble dFreqHist
								);

			// histogram to array of dxl buckets
			static
			DrgPdxlbucket *Pdrgpdxlbucket
								(
								IMemoryPool *pmp,
								const IMDType *pmdtype,
								const CHistogram *phist
								);

			// transform stats from pg_stats form to optimizer's preferred form
			static
			DrgPdxlbucket *PdrgpdxlbucketTransformStats
								(
								IMemoryPool *pmp,
								OID oidAttType,
								CDouble dDistinct,
								CDouble dNullFrequency,
								const Datum *pdrgdatumMCVValues,
								const float4 *pdrgfMCVFrequencies,
								ULONG ulNumMCVValues,
								const Datum *pdrgdatumHistValues,
								ULONG ulNumHistValues
								);

			// get partition keys for a relation
			static
			DrgPul *PdrgpulPartKeys(IMemoryPool *pmp, OID oid);

			// get keysets for relation
			static
			DrgPdrgPul *PdrgpdrgpulKeys(IMemoryPool *pmp, OID oid, ULONG *pulMapping);

			// storage type for a relation
			static
			IMDRelation::Erelstoragetype Erelstorage(CHAR cStorageType);

			// fix frequencies if they add up to more than 1.0
			static
			void NormalizeFrequencies(float4 *pdrgf, ULONG ulLength);

			// get the relation columns
			static
			DrgPmdcol *Pdrgpmdcol(IMemoryPool *pmp, CMDAccessor *pmda, TupleDesc rd_att);

			// return the dxl representation of the column's default value
			static
			CDXLNode *PdxlnDefaultColumnValue(IMemoryPool *pmp, CMDAccessor *pmda, TupleDesc rd_att, AttrNumber attrno);

			// get the distribution policy of the relation
			static
			IMDRelation::Ereldistrpolicy Ereldistribution(GpPolicy *pgppolicy);

			// get the distribution columns
			static
			DrgPul *PdrpulDistrCols(IMemoryPool *pmp, GpPolicy *pgppolicy, DrgPmdcol *pdrgpmdcol, ULONG ulSize);

			// construct a mapping GPDB attnos -> position in the column array
			static
			ULONG *PulAttnoMapping(IMemoryPool *pmp, DrgPmdcol *pdrgpmdcol, ULONG ulMaxCols);

			// check if index is supported
			static
			BOOL FIndexSupported(HeapTupleData *pht);

		public:
			// retrieve a metadata object from the relcache
			static
			IMDCacheObject *Pimdobj(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// retrieve a relation from the relcache
			static
			IMDRelation *Pmdrel(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// add system columns (oid, tid, xmin, etc) in table descriptors
			static
			void AddSystemColumns(IMemoryPool *pmp, DrgPmdcol *pdrgpmdcol, BOOL fhasOid);

			// retrieve an index from the relcache
			static
			IMDIndex *Pmdindex(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// retrieve a check constraint from the relcache
			static
			CMDCheckConstraintGPDB *Pmdcheckconstraint(IMemoryPool *pmp, CMDAccessor *pmda, IMDId *pmdid);

			// populate the attribute number to position mapping
			static
			ULONG *PulAttnoPositionMap(IMemoryPool *pmp, IMDRelation *pmdrel, ULONG ulRgSize);

			// return the position of a given attribute number
			static
			ULONG UlPosition(INT iAttno, ULONG *pul);

			// retrieve a type from the relcache
			static
			IMDType *Pmdtype(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve a scalar operator from the relcache
			static
			CMDScalarOpGPDB *Pmdscop(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve a function from the relcache
			static
			CMDFunctionGPDB *Pmdfunc(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve an aggregate from the relcache
			static
			CMDAggregateGPDB *Pmdagg(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve a trigger from the relcache
			static
			CMDTriggerGPDB *Pmdtrigger(IMemoryPool *pmp, IMDId *pmdid);

			// retrieve the id of the function with the given name
			static
			IMDId *PmdidFunc(IMemoryPool *pmp, const WCHAR *wszFuncName);
	};
}



#endif // !GPDXL_CTranslatorRelcacheToDXL_H

// EOF
