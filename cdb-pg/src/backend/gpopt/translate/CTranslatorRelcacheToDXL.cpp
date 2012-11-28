//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CTranslatorRelcacheToDXL.cpp
//
//	@doc:
//		Class translating relcache entries into DXL objects
//
//	@owner:
//		antovl
//
//	@test:
//
//
//---------------------------------------------------------------------------

#define ALLOW_DatumGetPointer
#define ALLOW_ntohl
#define ALLOW_memset
#define ALLOW_printf
#define ALLOW_offsetof
#define ALLOW_PointerGetDatum
#define ALLOW_Int32GetDatum
#define ALLOW_Int16GetDatum
#define ALLOW_DatumGetInt32
#define ALLOW_MemoryContextAllocImpl
#define ALLOW_pfree
#define ALLOW_MemoryContextFreeImpl
#define ALLOW_CharGetDatum

#include "postgres.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CTranslatorRelcacheToDXL.h"
#include "gpopt/translate/CTranslatorScalarToDXL.h"
#include "gpopt/mdcache/CMDAccessor.h"

#include "utils/array.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/datum.h"
#include "utils/elog.h"
#include "cdb/cdbhash.h"
#include "access/heapam.h"

#include "cdb/cdbpartition.h"
#include "catalog/namespace.h"
#include "catalog/pg_statistic.h"

#undef ALLOW_DatumGetPointer
#undef ALLOW_ntohl
#undef ALLOW_memset
#undef ALLOW_printf
#undef ALLOW_offsetof
#undef ALLOW_PointerGetDatum
#undef ALLOW_Int32GetDatum
#undef ALLOW_Int16GetDatum
#undef ALLOW_DatumGetInt32
#undef ALLOW_CharGetDatum
#undef ALLOW_pfree
#undef ALLOW_MemoryContextAllocImpl
#undef ALLOW_MemoryContextFreeImpl

#include "gpos/base.h"
#include "gpos/error/CException.h"

#include "exception.h"
#include "dxl/CDXLUtils.h"

#include "dxl/xml/dxltokens.h"

#include "md/CMDTypeBoolGPDB.h"
#include "md/CMDTypeGenericGPDB.h"
#include "md/CMDTypeInt4GPDB.h"
#include "md/CMDTypeInt8GPDB.h"
#include "md/CMDTypeOidGPDB.h"
#include "md/CMDIndexGPDB.h"
#include "md/CMDIdRelStats.h"
#include "md/CDXLRelStats.h"
#include "md/CMDIdColStats.h"
#include "md/CDXLColStats.h"

#include "gpopt/base/CUtils.h"

#include "gpopt/gpdbwrappers.h"


using namespace gpdxl;
using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pimdobj
//
//	@doc:
//		Retrieve a metadata object from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::Pimdobj
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	IMDCacheObject *pmdcacheobj = NULL;
	GPOS_ASSERT(NULL != pmda);

	switch(pmdid->Emdidt())
	{
		case CMDIdGPDB::EmdidGPDB:
		{
			pmdcacheobj = PimdobjGPDB(pmp, pmda, pmdid);
			break;
		}
		case CMDIdGPDB::EmdidRelStats:
		{
			pmdcacheobj = PimdobjRelStats(pmp, pmdid);
			break;
		}
		case CMDIdGPDB::EmdidColStats:
		{
			pmdcacheobj = PimdobjColStats(pmp, pmda, pmdid);
			break;
		}
		default:
		{
			break;
		}
	}

	if (NULL == pmdcacheobj)
	{
		// no match found
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	return pmdcacheobj;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjGPDB
//
//	@doc:
//		Retrieve a GPDB metadata object from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjGPDB
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	GPOS_ASSERT(pmdid->Emdidt() == CMDIdGPDB::EmdidGPDB);

	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(0 != oid);

	// find out what type of object this oid stands for

	if (gpdb::FIndexExists(oid))
	{
		return Pmdindex(pmp, pmda, pmdid);
	}

	if (gpdb::FTypeExists(oid))
	{
		return Pmdtype(pmp, pmdid);
	}

	if (gpdb::FRelationExists(oid))
	{
		return Pmdrel(pmp, pmda, pmdid);
	}

	if (gpdb::FOperatorExists(oid))
	{
		return Pmdscop(pmp, pmdid);
	}

	if (gpdb::FAggregateExists(oid))
	{
		return Pmdagg(pmp, pmdid);
	}

	if (gpdb::FFunctionExists(oid))
	{
		return Pmdfunc(pmp, pmdid);
	}

	if (gpdb::FTriggerExists(oid))
	{
		return Pmdtrigger(pmp, pmdid);
	}

	if (gpdb::FCheckConstraintExists(oid))
	{
		return Pmdcheckconstraint(pmp, pmda, pmdid);
	}

	// no match found
	return NULL;

}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdrel
//
//	@doc:
//		Retrieve a relation from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDRelation *
CTranslatorRelcacheToDXL::Pmdrel
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(0 != oid);

	Relation rel = gpdb::RelGetRelation(oid);

	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	// get rel name
	CHAR *szRelName = NameStr(rel->rd_rel->relname);
	CWStringDynamic *pstrRelName = CDXLUtils::PstrFromSz(pmp, szRelName);

	CMDName *pmdname = New(pmp) CMDName(pmp, pstrRelName);

	// CMDName ctor created a copy of the string
	delete pstrRelName;

	// get relation columns
	DrgPmdcol *pdrgpmdcol = Pdrgpmdcol(pmp, pmda, rel->rd_att);

	// add system columns
	if (RELKIND_RELATION == rel->rd_rel->relkind ||
		RELKIND_SEQUENCE == rel->rd_rel->relkind)
	{
		AddSystemColumns(pmp, pdrgpmdcol, rel->rd_att->tdhasoid);
	}

	const ULONG ulMaxCols = GPDXL_SYSTEM_COLUMNS + (ULONG) rel->rd_att->natts + 1;

	ULONG *pulAttnoMapping = PulAttnoMapping(pmp, pdrgpmdcol, ulMaxCols);

	// get storage type
	IMDRelation::Erelstoragetype erelstorage = Erelstorage(rel->rd_rel->relstorage);

	// get distribution policy
	GpPolicy *pgppolicy = rel->rd_cdbpolicy;
	IMDRelation::Ereldistrpolicy ereldistribution = Ereldistribution(pgppolicy);

	// get distribution columns
	DrgPul *pdrpulDistrCols = NULL;
	if (IMDRelation::EreldistrHash == ereldistribution)
	{
		pdrpulDistrCols = PdrpulDistrCols(pmp, pgppolicy, pdrgpmdcol, ulMaxCols);
	}

	// collect relation indices
	DrgPmdid *pdrgpmdidIndices = New(pmp) DrgPmdid(pmp);

	List *plIndexOids = gpdb::PlRelationIndexes(rel);
	ListCell *plc = NULL;

	ForEach (plc, plIndexOids)
	{
		OID oidIndex = lfirst_oid(plc);

		// only add supported indexes
		Relation relIndex = gpdb::RelGetRelation(oidIndex);

		GPOS_ASSERT(NULL != relIndex);
		GPOS_ASSERT (NULL != relIndex->rd_indextuple);

		if (FIndexSupported(relIndex->rd_indextuple))
		{
			CMDIdGPDB *pmdidIndex = New(pmp) CMDIdGPDB(oidIndex);
			pdrgpmdidIndices->Append(pmdidIndex);
		}

		gpdb::CloseRelation(relIndex);
	}

	// collect relation triggers
	if (0 < rel->rd_rel->reltriggers && NULL == rel->trigdesc)
	{
		gpdb::BuildRelationTriggers(rel);
		if (NULL == rel->trigdesc)
		{
			rel->rd_rel->reltriggers = 0;
		}
	}

	DrgPmdid *pdrgpmdidTriggers = New(pmp) DrgPmdid(pmp);
	const ULONG ulTriggers = rel->rd_rel->reltriggers;

	for (ULONG ul = 0; ul < ulTriggers; ul++)
	{
		Trigger trigger = rel->trigdesc->triggers[ul];
		OID oidTrigger = trigger.tgoid;
		CMDIdGPDB *pmdidTrigger = New(pmp) CMDIdGPDB(oidTrigger);
		pdrgpmdidTriggers->Append(pmdidTrigger);
	}

	pmdid->AddRef();

	// get partition keys
	DrgPul *pdrgpulPartKeys = PdrgpulPartKeys(pmp, oid);

	// get key sets
	DrgPdrgPul *pdrgpdrgpulKeys = PdrgpdrgpulKeys(pmp, oid, pulAttnoMapping);

	// collect all check constraints
	DrgPmdid *pdrgpmdidCheckConstraint = New(pmp) DrgPmdid(pmp);
	List *plOidCheckConstraints = gpdb::PlCheckConstraint(oid);

	ListCell *plcOid = NULL;
	ForEach (plcOid, plOidCheckConstraints)
	{
		OID oidCheckConstraint = lfirst_oid(plcOid);
		GPOS_ASSERT(0 != oidCheckConstraint);
		CMDIdGPDB *pmdidCheckConstraint = New(pmp) CMDIdGPDB(oidCheckConstraint);
		pdrgpmdidCheckConstraint->Append(pmdidCheckConstraint);
	}

	CMDRelationGPDB *pmdrel = New(pmp) CMDRelationGPDB
										(
										pmp,
										pmdid,
										pmdname,
										rel->rd_istemp,
										erelstorage,
										ereldistribution,
										pdrgpmdcol,
										pdrpulDistrCols,
										pdrgpulPartKeys,
										pdrgpdrgpulKeys,
										pdrgpmdidIndices,
										pdrgpmdidTriggers,
										pdrgpmdidCheckConstraint
										);

	gpdb::CloseRelation(rel);
	delete [] pulAttnoMapping;

	return pmdrel;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pdrgpmdcol
//
//	@doc:
//		Get relation columns
//
//---------------------------------------------------------------------------
DrgPmdcol *
CTranslatorRelcacheToDXL::Pdrgpmdcol
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	TupleDesc rd_att
	)
{
	DrgPmdcol *pdrgpmdcol = New(pmp) DrgPmdcol(pmp);

	for (ULONG ul = 0;  ul < (ULONG) rd_att->natts; ul++)
	{
		Form_pg_attribute att = rd_att->attrs[ul];

		if (att->attisdropped)
		{
			continue;
		}

		CWStringDynamic *pstrColName = CDXLUtils::PstrFromSz(pmp, NameStr(att->attname));
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pstrColName);
		// CMDName ctor created copies the string
		delete pstrColName;

		// translate the default column value
		CDXLNode *pdxlnDefault = PdxlnDefaultColumnValue(pmp, pmda, rd_att, att->attnum);

		CMDColumn *pmdcol = New(pmp) CMDColumn
										(
										pmdnameCol,
										att->attnum,
										New(pmp) CMDIdGPDB(att->atttypid),
										!att->attnotnull,
										pdxlnDefault /* default value */
										);

		pdrgpmdcol->Append(pmdcol);
	}

	return pdrgpmdcol;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdxlnDefaultColumnValue
//
//	@doc:
//		Return the dxl representation of column's default value
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorRelcacheToDXL::PdxlnDefaultColumnValue
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	TupleDesc rd_att,
	AttrNumber attno
	)
{
	GPOS_ASSERT(attno > 0);

	Node *pnode = NULL;

	// Scan to see if relation has a default for this column
	if (NULL != rd_att->constr && 0 < rd_att->constr->num_defval)
	{
		AttrDefault *defval = rd_att->constr->defval;
		INT	iNumDef = rd_att->constr->num_defval;

		GPOS_ASSERT(NULL != defval);
		for (ULONG ulCounter = 0; ulCounter < (ULONG) iNumDef; ulCounter++)
		{
			if (attno == defval[ulCounter].adnum)
			{
				// found it, convert string representation to node tree.
				pnode = gpdb::Pnode(defval[ulCounter].adbin);
				break;
			}
		}
	}

	if (NULL == pnode)
	{
		// get the default value for the type
		Form_pg_attribute att_tup = rd_att->attrs[attno - 1];
		Oid	oidAtttype = att_tup->atttypid;
		pnode = gpdb::PnodeTypeDefault(oidAtttype);
	}

	if (NULL == pnode)
	{
		return NULL;
	}

	// translate the default value expression
	CTranslatorScalarToDXL sctranslator
							(
							pmp,
							pmda,
							NULL, /* pulidgtorCol */
							NULL, /* pulidgtorCTE */
							0, /* ulQueryLevel */
							true, /* m_fQuery */
							NULL, /* m_pplstmt */
							NULL /* m_pmappv */,
							NULL /* phmulCTEEntries */
							);

	return sctranslator.PdxlnScOpFromExpr
							(
							(Expr *) pnode,
							NULL /* pmapvarcolid --- subquery or external variable are not supported in default expression */
							);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Ereldistribution
//
//	@doc:
//		Return the distribution policy of the relation
//
//---------------------------------------------------------------------------
IMDRelation::Ereldistrpolicy
CTranslatorRelcacheToDXL::Ereldistribution
	(
	GpPolicy *pgppolicy
	)
{
	if (NULL == pgppolicy)
	{
		return IMDRelation::EreldistrMasterOnly;
	}

	if (POLICYTYPE_PARTITIONED == pgppolicy->ptype)
	{
		if (0 == pgppolicy->nattrs)
		{
			return IMDRelation::EreldistrRandom;
		}

		return IMDRelation::EreldistrHash;
	}

	if (POLICYTYPE_ENTRY == pgppolicy->ptype)
	{
		return IMDRelation::EreldistrMasterOnly;
	}

	GPOS_ASSERT(!"Unrecognized distribution policy");
	return IMDRelation::EreldistrSentinel;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrpulDistrCols
//
//	@doc:
//		Get distribution columns
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorRelcacheToDXL::PdrpulDistrCols
	(
	IMemoryPool *pmp,
	GpPolicy *pgppolicy,
	DrgPmdcol *pdrgpmdcol,
	ULONG ulSize
	)
{
	ULONG *pul = New(pmp) ULONG[ulSize];

	for (ULONG ul = 0;  ul < pdrgpmdcol->UlLength(); ul++)
	{
		const IMDColumn *pmdcol = (*pdrgpmdcol)[ul];
		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		pul[ulIndex] = ul;
	}

	DrgPul *pdrpulDistrCols = New(pmp) DrgPul(pmp);

	for (ULONG ul = 0; ul < (ULONG) pgppolicy->nattrs; ul++)
	{
		AttrNumber attno = pgppolicy->attrs[ul];
		pdrpulDistrCols->Append(New(pmp) ULONG(UlPosition(attno, pul)));
	}

	delete[] pul;
	return pdrpulDistrCols;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::AddSystemColumns
//
//	@doc:
//		Adding system columns (oid, tid, xmin, etc) in table descriptors
//
//---------------------------------------------------------------------------
void
CTranslatorRelcacheToDXL::AddSystemColumns
	(
	IMemoryPool *pmp,
	DrgPmdcol *pdrgpmdcol,
	BOOL fHasOid
	)
{
	for (INT i= SelfItemPointerAttributeNumber; i > FirstLowInvalidHeapAttributeNumber; i--)
	{
		AttrNumber attno = AttrNumber(i);
		GPOS_ASSERT(0 != attno);

		if (ObjectIdAttributeNumber == i && !fHasOid)
		{
			continue;
		}
		// get system name for that attribute
		const CWStringConst *pstrSysColName = CTranslatorUtils::PstrSystemColName(attno);
		GPOS_ASSERT(NULL != pstrSysColName);

		// copy string into column name
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pstrSysColName);

		CMDColumn *pmdcol = New(pmp) CMDColumn(pmdnameCol, attno, CTranslatorUtils::PmdidSystemColType(pmp, attno), false, NULL /* default value */);

		pdrgpmdcol->Append(pmdcol);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdindex
//
//	@doc:
//		Retrieve an index from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDIndex *
CTranslatorRelcacheToDXL::Pmdindex
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	OID oid = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	GPOS_ASSERT(0 != oid);
	Relation rel = gpdb::RelGetRelation(oid);

	if (NULL == rel)
	{
		 GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	// get index keys
	Form_pg_index pgIndex = rel->rd_index;
	GPOS_ASSERT (NULL != pgIndex);

	if (!FIndexSupported(rel->rd_indextuple))
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported, GPOS_WSZ_LIT("Index type"));
	}


	// get the index name
	CHAR *szIndexName = NameStr(rel->rd_rel->relname);
	CWStringDynamic *pstrName = CDXLUtils::PstrFromSz(pmp, szIndexName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrName);
	delete pstrName;

	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(pgIndex->indrelid);
	IMDRelation *pmdrel = Pmdrel(pmp, pmda, pmdidRel);

	Relation relTable = gpdb::RelGetRelation(CMDIdGPDB::PmdidConvert(pmdrel->Pmdid())->OidObjectId());
	ULONG ulRgSize = GPDXL_SYSTEM_COLUMNS + (ULONG) relTable->rd_att->natts + 1;

	ULONG *pul = PulAttnoPositionMap(pmp, pmdrel, ulRgSize);

	// TODO: raghav, 3/19/2012; currently we assume that all the columns
	// in the table are available from the index.
	DrgPul *pdrgpulIncludeCols = New(pmp) DrgPul(pmp);
	const ULONG ulIncludedCols = pmdrel->UlColumns();
	for (ULONG ul = 0;  ul < ulIncludedCols; ul++)
	{
		pdrgpulIncludeCols->Append(New(pmp) ULONG(ul));
	}

	// extract the position of the key columns
	DrgPul *pdrgpulKeyCols = New(pmp) DrgPul(pmp);
	ULONG ulKeys = pgIndex->indnatts;
	for (ULONG ul = 0; ul < ulKeys; ul++)
	{
		INT iAttno = pgIndex->indkey.values[ul];
		GPOS_ASSERT(0 != iAttno && "Index expressions not supported");

		pdrgpulKeyCols->Append(New(pmp) ULONG(UlPosition(iAttno, pul)));
	}

	pmdid->AddRef();
	CMDIndexGPDB *pmdindex = New(pmp) CMDIndexGPDB
										(
										pmp,
										pmdid,
										pmdname,
										New(pmp) CMDIdGPDB(pgIndex->indrelid),
										pgIndex->indisclustered,
										pdrgpulKeyCols,
										pdrgpulIncludeCols
										);

	delete [] pul;
	pmdidRel->Release();
	pmdrel->Release();
	gpdb::CloseRelation(rel);
	gpdb::CloseRelation(relTable);
	return pmdindex;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::UlPosition
//
//	@doc:
//		Return the position of a given attribute
//
//---------------------------------------------------------------------------
ULONG
CTranslatorRelcacheToDXL::UlPosition
	(
	INT iAttno,
	ULONG *pul
	)
{
	ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
	ULONG ulPos = pul[ulIndex];
	GPOS_ASSERT(ULONG_MAX != ulPos);

	return ulPos;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PulAttnoPositionMap
//
//	@doc:
//		Populate the attribute to position mapping
//
//---------------------------------------------------------------------------
ULONG *
CTranslatorRelcacheToDXL::PulAttnoPositionMap
	(
	IMemoryPool *pmp,
	IMDRelation *pmdrel,
	ULONG ulSize
	)
{
	GPOS_ASSERT(NULL != pmdrel);
	const ULONG ulIncludedCols = pmdrel->UlColumns();

	GPOS_ASSERT(ulIncludedCols <= ulSize);
	ULONG *pul = New(pmp) ULONG[ulSize];

	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		pul[ul] = ULONG_MAX;
	}

	for (ULONG ul = 0;  ul < ulIncludedCols; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);

		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		GPOS_ASSERT(ulSize > ulIndex);
		pul[ulIndex] = ul;
	}

	return pul;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdtype
//
//	@doc:
//		Retrieve a type from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
IMDType *
CTranslatorRelcacheToDXL::Pmdtype
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidType = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	GPOS_ASSERT(InvalidOid != oidType);

	// check for supported base types
	if (GPDB_INT4_OID == oidType)
	{
		return New(pmp) CMDTypeInt4GPDB(pmp);
	}

	if (GPDB_INT8_OID == oidType)
	{
		return New(pmp) CMDTypeInt8GPDB(pmp);
	}

	if (GPDB_BOOL == oidType)
	{
		return New(pmp) CMDTypeBoolGPDB(pmp);
	}

	if (GPDB_OID_OID == oidType)
	{
		return New(pmp) CMDTypeOidGPDB(pmp);
	}

	// construct a generic type
	INT iFlags = TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				 TYPECACHE_CMP_PROC | TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO | TYPECACHE_TUPDESC;

	TypeCacheEntry *ptce = gpdb::PtceLookup(oidType, iFlags);

	// get type name
	CMDName *pmdname = PmdnameType(pmp, pmdid);

	BOOL fFixedLength = false;
	ULONG ulLength = 0;

	if (0 < ptce->typlen)
	{
		fFixedLength = true;
		ulLength = ptce->typlen;
	}

	BOOL fByValue = ptce->typbyval;

	// collect ids of different comparison operators for types
	CMDIdGPDB *pmdidOpEq = New(pmp) CMDIdGPDB(ptce->eq_opr);
	CMDIdGPDB *pmdidOpNEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->eq_opr));
	CMDIdGPDB *pmdidOpLT = New(pmp) CMDIdGPDB(ptce->lt_opr);
	CMDIdGPDB *pmdidOpLEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->gt_opr));
	CMDIdGPDB *pmdidOpGT = New(pmp) CMDIdGPDB(ptce->gt_opr);
	CMDIdGPDB *pmdidOpGEq = New(pmp) CMDIdGPDB(gpdb::OidInverseOp(ptce->lt_opr));
	CMDIdGPDB *pmdidOpComp = New(pmp) CMDIdGPDB(ptce->cmp_proc);
	BOOL fHashable = gpdb::FOpHashJoinable(ptce->eq_opr);
	BOOL fComposite = gpdb::FCompositeType(oidType);

	// check if type is composite
	CMDIdGPDB *pmdidTypeRelid = NULL;
	if (fComposite)
	{
		pmdidTypeRelid = New(pmp) CMDIdGPDB(gpdb::OidTypeRelid(oidType));
	}

	// get array type mdid
	CMDIdGPDB *pmdidTypeArray = New(pmp) CMDIdGPDB(gpdb::OidArrayType(oidType));

	BOOL fRedistributable = gpdb::FGreenplumDbHashable(oidType);

	pmdid->AddRef();

	return New(pmp) CMDTypeGenericGPDB
						 (
						 pmp,
						 pmdid,
						 pmdname,
						 fRedistributable,
						 fFixedLength,
						 ulLength,
						 fByValue,
						 pmdidOpEq,
						 pmdidOpNEq,
						 pmdidOpLT,
						 pmdidOpLEq,
						 pmdidOpGT,
						 pmdidOpGEq,
						 pmdidOpComp,
						 fHashable,
						 fComposite,
						 pmdidTypeRelid,
						 pmdidTypeArray,
						 ptce->typlen
						 );
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdscop
//
//	@doc:
//		Retrieve a scalar operator from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDScalarOpGPDB *
CTranslatorRelcacheToDXL::Pmdscop
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidOp = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidOp);

	// get operator name
	CHAR *szName = gpdb::SzOpName(oidOp);

	if (NULL == szName)
	{

		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrOpName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrOpName);

	// CMDName ctor created a copy of the string
	delete pstrOpName;

	// check whether operator is an equality
	OID oidLeftSortOp = InvalidOid;
	OID oidRightSortOp = InvalidOid;

	BOOL fEquality = gpdb::FOpMergeJoinable(oidOp, &oidLeftSortOp, &oidRightSortOp);

	OID oidLeft = InvalidOid;
	OID oidRight = InvalidOid;

	// get operator argument types
	gpdb::GetOpInputTypes(oidOp, &oidLeft, &oidRight);

	CMDIdGPDB *pmdidTypeLeft = NULL;
	CMDIdGPDB *pmdidTypeRight = NULL;

	if (InvalidOid != oidLeft)
	{
		pmdidTypeLeft = New(pmp) CMDIdGPDB(oidLeft);
	}

	if (InvalidOid != oidRight)
	{
		pmdidTypeRight = New(pmp) CMDIdGPDB(oidRight);
	}

	// get func oid
	OID oidFunc = gpdb::OidOpFunc(oidOp);
	GPOS_ASSERT(InvalidOid != oidFunc);

	CMDIdGPDB *pmdidFunc = New(pmp) CMDIdGPDB(oidFunc);

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidFunc);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);

	// get commutator and inverse
	CMDIdGPDB *pmdidOpCommute = NULL;

	OID oidCommute = gpdb::OidCommutatorOp(oidOp);

	if(InvalidOid != oidCommute)
	{
		pmdidOpCommute = New(pmp) CMDIdGPDB(oidCommute);
	}

	CMDIdGPDB *pmdidOpInverse = NULL;

	OID oidInverse = gpdb::OidInverseOp(oidOp);

	if(InvalidOid != oidInverse)
	{
		pmdidOpInverse = New(pmp) CMDIdGPDB(oidInverse);
	}

	pmdid->AddRef();
	CMDScalarOpGPDB *pmdscop = New(pmp) CMDScalarOpGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeLeft,
											pmdidTypeRight,
											pmdidTypeResult,
											pmdidFunc,
											pmdidOpCommute,
											pmdidOpInverse,
											fEquality
											);
	return pmdscop;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdfunc
//
//	@doc:
//		Retrieve a function from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDFunctionGPDB *
CTranslatorRelcacheToDXL::Pmdfunc
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidFunc = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidFunc);

	// get func name
	CHAR *szName = gpdb::SzFuncName(oidFunc);

	if (NULL == szName)
	{

		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrFuncName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrFuncName);

	// CMDName ctor created a copy of the string
	delete pstrFuncName;

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidFunc);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);

	// get output argument types if any
	List *plOutArgTypes = gpdb::PlFuncOutputArgTypes(oidFunc);

	DrgPmdid *pdrgpmdidArgTypes = NULL;
	if (NULL != plOutArgTypes)
	{
		ListCell *plc = NULL;
		pdrgpmdidArgTypes = New(pmp) DrgPmdid(pmp);

		ForEach (plc, plOutArgTypes)
		{
			OID oidArgType = lfirst_oid(plc);
			GPOS_ASSERT(InvalidOid != oidArgType);
			CMDIdGPDB *pmdidArgType = New(pmp) CMDIdGPDB(oidArgType);
			pdrgpmdidArgTypes->Append(pmdidArgType);
		}

		gpdb::GPDBFree(plOutArgTypes);
	}

	BOOL fReturnSet = gpdb::FFuncRetset(oidFunc);
	BOOL fStrict = gpdb::FFuncStrict(oidFunc);

	CHAR cFuncStability = gpdb::CFuncStability(oidFunc);
	CMDFunctionGPDB::EFuncStbl efuncstbl = EFuncStability(cFuncStability);

	CHAR cFuncDataAccess = gpdb::CFuncDataAccess(oidFunc);
	CMDFunctionGPDB::EFuncDataAcc efda = EFuncDataAccess(cFuncDataAccess);

	pmdid->AddRef();
	CMDFunctionGPDB *pmdfunc = New(pmp) CMDFunctionGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeResult,
											pdrgpmdidArgTypes,
											fReturnSet,
											efuncstbl,
											efda,
											fStrict
											);
	return pmdfunc;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdagg
//
//	@doc:
//		Retrieve an aggregate from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDAggregateGPDB *
CTranslatorRelcacheToDXL::Pmdagg
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidAgg = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidAgg);

	// get agg name
	CHAR *szName = gpdb::SzFuncName(oidAgg);

	if (NULL == szName)
	{

		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrAggName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrAggName);

	// CMDName ctor created a copy of the string
	delete pstrAggName;

	// get result type
	OID oidResult = gpdb::OidFuncRetType(oidAgg);

	GPOS_ASSERT(InvalidOid != oidResult);

	CMDIdGPDB *pmdidTypeResult = New(pmp) CMDIdGPDB(oidResult);
	IMDId *pmdidTypeIntermediate = PmdidAggIntermediateResultType(pmp, pmdid);

	pmdid->AddRef();
	CMDAggregateGPDB *pmdagg = New(pmp) CMDAggregateGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidTypeResult,
											pmdidTypeIntermediate
											);
	return pmdagg;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdtrigger
//
//	@doc:
//		Retrieve a trigger from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDTriggerGPDB *
CTranslatorRelcacheToDXL::Pmdtrigger
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidTrigger = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidTrigger);

	// get trigger name
	CHAR *szName = gpdb::SzTriggerName(oidTrigger);

	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	CWStringDynamic *pstrTriggerName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrTriggerName);
	delete pstrTriggerName;

	// get relation oid
	OID oidRel = gpdb::OidTriggerRelid(oidTrigger);
	GPOS_ASSERT(InvalidOid != oidRel);
	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(oidRel);

	// get function oid
	OID oidFunc = gpdb::OidTriggerFuncid(oidTrigger);
	GPOS_ASSERT(InvalidOid != oidFunc);
	CMDIdGPDB *pmdidFunc = New(pmp) CMDIdGPDB(oidFunc);

	// get type
	INT iType = gpdb::ITriggerType(oidTrigger);

	// is trigger enabled
	BOOL fEnabled = gpdb::FTriggerEnabled(oidTrigger);

	pmdid->AddRef();
	CMDTriggerGPDB *pmdtrigger = New(pmp) CMDTriggerGPDB
											(
											pmp,
											pmdid,
											pmdname,
											pmdidRel,
											pmdidFunc,
											iType,
											fEnabled
											);
	return pmdtrigger;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pmdcheckconstraint
//
//	@doc:
//		Retrieve a check constraint from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDCheckConstraintGPDB *
CTranslatorRelcacheToDXL::Pmdcheckconstraint
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	OID oidCheckConstraint = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();
	GPOS_ASSERT(InvalidOid != oidCheckConstraint);

	// get name of the check constraint
	CHAR *szName = gpdb::SzCheckConstraintName(oidCheckConstraint);
	if (NULL == szName)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}
	CWStringDynamic *pstrCheckConstraintName = CDXLUtils::PstrFromSz(pmp, szName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrCheckConstraintName);
	delete pstrCheckConstraintName;

	// get relation oid associated with the check constraint
	OID oidRel = gpdb::OidCheckConstraintRelid(oidCheckConstraint);
	GPOS_ASSERT(InvalidOid != oidRel);
	CMDIdGPDB *pmdidRel = New(pmp) CMDIdGPDB(oidRel);

	// translate the check constraint expression
	Node *pnode = gpdb::PnodeCheckConstraint(oidCheckConstraint);
	GPOS_ASSERT(NULL != pnode);

	CTranslatorScalarToDXL sctranslator
							(
							pmp,
							pmda,
							NULL, /* pulidgtorCol */
							NULL, /* pulidgtorCTE */
							0, /* ulQueryLevel */
							true, /* m_fQuery */
							NULL, /* m_pplstmt */
							NULL /* m_pmappv */,
							NULL /* phmulCTEEntries */
							);

	// generate a mock mapping between var to column information
	CMappingVarColId *pmapvarcolid = New(pmp) CMappingVarColId(pmp);
	DrgPdxlcd *pdrgpdxlcd = New(pmp) DrgPdxlcd(pmp);
	const IMDRelation *pmdrel = pmda->Pmdrel(pmdidRel);
	const ULONG ulLen = pmdrel->UlColumns();
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
		CMDIdGPDB *pmdidColType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType());
		pmdidColType->AddRef();

		// create a column descriptor for the column
		CDXLColDescr *pdxlcd = New(pmp) CDXLColDescr
										(
										pmp,
										pmdnameCol,
										ul + 1 /*ulColId*/,
										pmdcol->IAttno(),
										pmdidColType,
										false /* fColDropped */
										);
		pdrgpdxlcd->Append(pdxlcd);
	}
	pmapvarcolid->LoadColumns(0 /*ulQueryLevel */, 1 /* rteIndex */, pdrgpdxlcd);

	// translate the check constraint expression
	CDXLNode *pdxlnScalar = sctranslator.PdxlnScOpFromExpr((Expr *) pnode, pmapvarcolid);

	// cleanup
	pdrgpdxlcd->Release();
	delete pmapvarcolid;

	pmdid->AddRef();

	return New(pmp) CMDCheckConstraintGPDB
						(
						pmp,
						pmdid,
						pmdname,
						pmdidRel,
						pdxlnScalar
						);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdnameType
//
//	@doc:
//		Retrieve a type's name from the relcache given its metadata id.
//
//---------------------------------------------------------------------------
CMDName *
CTranslatorRelcacheToDXL::PmdnameType
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidType = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidType);

	CHAR *szTypeName = gpdb::SzTypeName(oidType);
	GPOS_ASSERT(NULL != szTypeName);

	CWStringDynamic *pstrName = CDXLUtils::PstrFromSz(pmp, szTypeName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrName);

	// cleanup
	delete pstrName;
	return pmdname;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::EFuncStability
//
//	@doc:
//		Get function stability property from the GPDB character representation
//
//---------------------------------------------------------------------------
CMDFunctionGPDB::EFuncStbl
CTranslatorRelcacheToDXL::EFuncStability
	(
	CHAR c
	)
{
	CMDFunctionGPDB::EFuncStbl efuncstbl = CMDFunctionGPDB::EfsSentinel;

	switch (c)
	{
		case 's':
			efuncstbl = CMDFunctionGPDB::EfsStable;
			break;
		case 'i':
			efuncstbl = CMDFunctionGPDB::EfsImmutable;
			break;
		case 'v':
			efuncstbl = CMDFunctionGPDB::EfsVolatile;
			break;
		default:
			GPOS_ASSERT(!"Invalid stability property");
	}

	return efuncstbl;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::EFuncDataAccess
//
//	@doc:
//		Get function data access property from the GPDB character representation
//
//---------------------------------------------------------------------------
CMDFunctionGPDB::EFuncDataAcc
CTranslatorRelcacheToDXL::EFuncDataAccess
	(
	CHAR c
	)
{
	CMDFunctionGPDB::EFuncDataAcc efda = CMDFunctionGPDB::EfdaSentinel;

	switch (c)
	{
		case 'n':
			efda = CMDFunctionGPDB::EfdaNoSQL;
			break;
		case 'c':
			efda = CMDFunctionGPDB::EfdaContainsSQL;
			break;
		case 'r':
			efda = CMDFunctionGPDB::EfdaReadsSQLData;
			break;
		case 'm':
			efda = CMDFunctionGPDB::EfdaModifiesSQLData;
			break;
		default:
			GPOS_ASSERT(!"Invalid data access property");
	}

	return efda;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdidAggIntermediateResultType
//
//	@doc:
//		Retrieve the type id of an aggregate's intermediate results
//
//---------------------------------------------------------------------------
IMDId *
CTranslatorRelcacheToDXL::PmdidAggIntermediateResultType
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	OID oidAgg = CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId();

	GPOS_ASSERT(InvalidOid != oidAgg);

	OID oidTypeIntermediateResult = gpdb::OidAggIntermediateResultType(oidAgg);
	return New(pmp) CMDIdGPDB(oidTypeIntermediateResult);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PmdidFunc
//
//	@doc:
//		Return the id of the function with the given name
//
//---------------------------------------------------------------------------
IMDId *
CTranslatorRelcacheToDXL::PmdidFunc
	(
	IMemoryPool *pmp,
	const WCHAR *wszFuncName
	)
{

	CHAR *szFuncName = CDXLUtils::SzFromWsz(pmp, wszFuncName);
	FuncCandidateList fcl = gpdb::FclFuncCandidates
								(
								ListMake1(gpdb::PvalMakeString(szFuncName)),
								-1 /* nargs */,
								NIL /* args */,
								false /* expand_variadic */,
								false /* expand_defaults */
								);

	if (NULL == fcl)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, wszFuncName);
	}

	delete [] szFuncName;
	return New(pmp) CMDIdGPDB(fcl->oid);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjRelStats
//
//	@doc:
//		Retrieve relation statistics from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjRelStats
	(
	IMemoryPool *pmp,
	IMDId *pmdid
	)
{
	CMDIdRelStats *pmdidRelStats = CMDIdRelStats::PmdidConvert(pmdid);
	IMDId *pmdidRel = pmdidRelStats->PmdidRel();
	OID oidRelation = CMDIdGPDB::PmdidConvert(pmdidRel)->OidObjectId();

	Relation rel = gpdb::RelGetRelation(oidRelation);
	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}
	// get rel name
	CHAR *szRelName = NameStr(rel->rd_rel->relname);
	CWStringDynamic *pstrRelName = CDXLUtils::PstrFromSz(pmp, szRelName);
	CMDName *pmdname = New(pmp) CMDName(pmp, pstrRelName);
	// CMDName ctor created a copy of the string
	delete pstrRelName;

	CDouble dRows(rel->rd_rel->reltuples);
	pmdidRelStats->AddRef();
	CDXLRelStats *pdxlrelstats = New(pmp) CDXLRelStats
												(
												pmp,
												pmdidRelStats,
												pmdname,
												dRows
												);
	gpdb::CloseRelation(rel);

	return pdxlrelstats;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PimdobjColStats
//
//	@doc:
//		Retrieve column statistics from relcache
//
//---------------------------------------------------------------------------
IMDCacheObject *
CTranslatorRelcacheToDXL::PimdobjColStats
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	IMDId *pmdid
	)
{
	CMDIdColStats *pmdidColStats = CMDIdColStats::PmdidConvert(pmdid);
	IMDId *pmdidRel = pmdidColStats->PmdidRel();
	ULONG ulPos = pmdidColStats->UlPos();
	OID oidRelation = CMDIdGPDB::PmdidConvert(pmdidRel)->OidObjectId();

	Relation rel = gpdb::RelGetRelation(oidRelation);
	if (NULL == rel)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDCacheEntryNotFound, pmdid->Wsz());
	}

	IMDRelation *pmdrel = Pmdrel(pmp, pmda, pmdidRel);
	const IMDColumn *pmdcol = pmdrel->Pmdcol(ulPos);
	AttrNumber attrnum = (AttrNumber) pmdcol->IAttno();

	// number of rows from pg_class
	CDouble dRows(rel->rd_rel->reltuples);

	// extract column name and type
	CMDName *pmdnameCol = New(pmp) CMDName(pmp, pmdcol->Mdname().Pstr());
	OID oidAttType = CMDIdGPDB::PmdidConvert(pmdcol->PmdidType())->OidObjectId();
	gpdb::CloseRelation(rel);

	// extract out histogram and mcv information from pg_statistic
	DrgPdxlbucket *pdrgpdxlbucket = New(pmp) DrgPdxlbucket(pmp);
	HeapTuple heaptupleStats = gpdb::HtAttrStats(oidRelation, attrnum);

	// if there is no colstats
	if (!HeapTupleIsValid(heaptupleStats))
	{
		pmdrel->Release();
		pdrgpdxlbucket->Release();
		pmdidColStats->AddRef();
		return CDXLColStats::PdxlcolstatsDummy
								(
								pmp,
								pmdidColStats,
								pmdnameCol
								);
	}


	Datum	   *pdrgdatumMCVValues = NULL;
	int			iNumMCVValues = 0;
	float4	   *pdrgfMCVFrequencies = NULL;
	int			iNumMCVFrequencies = 0;
	Datum		*pdrgdatumHistValues = NULL;
	int			iNumHistValues = 0;

	(void)	gpdb::FGetAttrStatsSlot
			(
					heaptupleStats,
					oidAttType,
					-1,
					STATISTIC_KIND_MCV,
					InvalidOid,
					&pdrgdatumMCVValues, &iNumMCVValues,
					&pdrgfMCVFrequencies, &iNumMCVFrequencies
			);

	GPOS_ASSERT(iNumMCVValues == iNumMCVFrequencies);

	// fix mcv frequencies (sometimes they can add up to more than 1.0)
	NormalizeFrequencies(pdrgfMCVFrequencies, (ULONG) iNumMCVValues);

	(void) gpdb::FGetAttrStatsSlot
			(
					heaptupleStats,
					oidAttType,
					-1,
					STATISTIC_KIND_HISTOGRAM,
					InvalidOid,
					&pdrgdatumHistValues, &iNumHistValues,
					NULL, NULL);

	Form_pg_statistic fpsStats = (Form_pg_statistic) GETSTRUCT(heaptupleStats);

	CDouble dWidth = CDouble(fpsStats->stawidth);

	// calculate total number of distinct values
	CDouble dDistinct(1.0);
	if (fpsStats->stadistinct < 0)
	{
		GPOS_ASSERT(fpsStats->stadistinct > -1.01);
		dDistinct = dRows * CDouble(-fpsStats->stadistinct);
	}
	else
	{
		dDistinct = CDouble(fpsStats->stadistinct);
	}
	dDistinct = dDistinct.FpCeil();

	// number of nulls
	CDouble dNullFrequency(fpsStats->stanullfrac);

	// transform all the bits and pieces from pg_stats
	// to a single bucket structure
	DrgPdxlbucket *pdrgpdxlbucketTransformed =
			PdrgpdxlbucketTransformStats
					(
					pmp,
					oidAttType,
					dDistinct,
					dNullFrequency,
					pdrgdatumMCVValues,
					pdrgfMCVFrequencies,
					ULONG(iNumMCVValues),
					pdrgdatumHistValues,
					ULONG(iNumHistValues)
					);

	CUtils::AddRefAppend(pdrgpdxlbucket, pdrgpdxlbucketTransformed);

	// free up allocated datum and float4 arrays
	gpdb::FreeAttrStatsSlot(oidAttType, pdrgdatumMCVValues, iNumMCVValues, pdrgfMCVFrequencies, iNumMCVFrequencies);
	gpdb::FreeAttrStatsSlot(oidAttType, pdrgdatumHistValues, iNumHistValues, NULL, 0);

	gpdb::FreeHeapTuple(heaptupleStats);

	pmdrel->Release();
	pdrgpdxlbucketTransformed->Release();

	// create col stats object
	pmdidColStats->AddRef();
	CDXLColStats *pdxlcolstats = New(pmp) CDXLColStats
											(
											pmp,
											pmdidColStats,
											pmdnameCol,
											dWidth,
											pdrgpdxlbucket
											);

	return pdxlcolstats;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpdxlbucketTransformStats
//
//	@doc:
//		transform stats from pg_stats form to optimizer's preferred form
//
//---------------------------------------------------------------------------
DrgPdxlbucket *
CTranslatorRelcacheToDXL::PdrgpdxlbucketTransformStats
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
	)
{
	CMDIdGPDB *pmdidAttType = New(pmp) CMDIdGPDB(oidAttType);
	IMDType *pmdtype = Pmdtype(pmp, pmdidAttType);

	// histogram from mcv information
	CHistogram *phistGPDBMCV = PhistTransformGPDBMCV
							(
							pmp,
							pmdtype,
							pdrgdatumMCVValues,
							pdrgfMCVFrequencies,
							ulNumMCVValues,
							dNullFrequency
							);

	CDouble dHistFreq = CDouble(1.0) - phistGPDBMCV->DFrequency();

	(void) phistGPDBMCV->DNormalize();
	GPOS_ASSERT(phistGPDBMCV->FValid());

	CHistogram *phistGPDBHist = NULL;

	// if histogram has any significant information, then extract it
	if (dHistFreq > CStatistics::DEpsilon)
	{
		// histogram from gpdb histogram
		phistGPDBHist = PhistTransformGPDBHist
						(
						pmp,
						pmdtype,
						pdrgdatumHistValues,
						ulNumHistValues,
						dDistinct,
						dHistFreq
						);
		(void) phistGPDBHist->DNormalize();
	}

	// TODO: siva - 02/21/2012 merge mcv and histogram
	// for now, simply use the one that has more information
	DrgPdxlbucket *pdrgpdxlbucket = NULL;
	if (dHistFreq > CDouble(0.5) )
	{
		GPOS_ASSERT(NULL != phistGPDBHist);
		pdrgpdxlbucket = Pdrgpdxlbucket(pmp, pmdtype, phistGPDBHist);
	}
	else
	{
		pdrgpdxlbucket = Pdrgpdxlbucket(pmp, pmdtype, phistGPDBMCV);
	}

	// cleanup
	pmdidAttType->Release();
	pmdtype->Release();
	delete phistGPDBMCV;

	if (NULL != phistGPDBHist)
	{
		delete phistGPDBHist;
	}

	return pdrgpdxlbucket;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PhistTransformGPDBMCV
//
//	@doc:
//		Transform gpdb's mcv info to optimizer histogram
//
//---------------------------------------------------------------------------
CHistogram *
CTranslatorRelcacheToDXL::PhistTransformGPDBMCV
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const Datum *pdrgdatumMCVValues,
	const float4 *pdrgfMCVFrequencies,
	ULONG ulNumMCVValues,
	CDouble dNullFrequency
	)
{
	// first put all IDatums in an array and sort them per datum's
	// stats comparison function
	DrgPdatum *pdrgpdatum = New(pmp) DrgPdatum(pmp);

	for (ULONG ul = 0; ul < ulNumMCVValues; ul++)
	{
		Datum datumMCV = pdrgdatumMCVValues[ul];
		IDatum *pdatum = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false /* fNull */, datumMCV);
		pdrgpdatum->Append(pdatum);

		if (!pdatum->FHasStatsLessThan())
		{
			// if less than operation is not supported on this datum, then no point
			// building a histogram. return an empty histogram
			pdrgpdatum->Release();
			return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp));
		}
	}

	// sort the idatums
	if (ulNumMCVValues > 1)
	{
		pdrgpdatum->Sort(IDatum::IStatsCmp);
	}

	// now put these in buckets
	DrgPbucket *pdrgpbucketMCV = New(pmp) DrgPbucket(pmp);

	// add null element
	if (dNullFrequency > CStatistics::DEpsilon)
	{
		IDatum *pdatumNull = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, true /* fNull*/,  Datum(0) /* Datum */);
		pdatumNull->AddRef();
		CBucket *pbucketNull = New(pmp) CBucket(New(pmp) CPoint(pdatumNull), New(pmp) CPoint(pdatumNull), dNullFrequency, CDouble(1.0));
		pdrgpbucketMCV->Append(pbucketNull);
	}

	for (ULONG ul = 0; ul < ulNumMCVValues; ul++)
	{
		IDatum *pdatum = (*pdrgpdatum)[ul];
		pdatum->AddRef();
		pdatum->AddRef();
		CDouble dBucketFreq = CDouble(pdrgfMCVFrequencies[ul]);
		CBucket *pbucket = New(pmp) CBucket(New(pmp) CPoint(pdatum), New(pmp) CPoint(pdatum), dBucketFreq, CDouble(1.0));
		pdrgpbucketMCV->Append(pbucket);
	}
	CHistogram *phist = New(pmp) CHistogram(pdrgpbucketMCV);
	pdrgpdatum->Release();
	return phist;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PhistTransformGPDBHist
//
//	@doc:
//		Transform GPDB's hist info to optimizer's histogram
//
//---------------------------------------------------------------------------
CHistogram *
CTranslatorRelcacheToDXL::PhistTransformGPDBHist
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const Datum *pdrgdatumHistValues,
	ULONG ulNumHistValues,
	CDouble dDistinctHist,
	CDouble dFreqHist
	)
{
	// handle corner case of no histogram
	if (ulNumHistValues <= 1)
	{
		return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp), false /* fWellFormed */);
	}

	GPOS_ASSERT(ulNumHistValues > 1);
	ULONG ulNumBuckets = ulNumHistValues - 1;
	CDouble dDistinctPerBucket = dDistinctHist / CDouble(ulNumBuckets);
	CDouble dFreqPerBucket = dFreqHist / CDouble(ulNumBuckets);

	// create buckets
	DrgPbucket *pdrgppbucket = New(pmp) DrgPbucket(pmp);
	for (ULONG ul = 0; ul < ulNumHistValues - 1; ul++)
	{
		Datum datumMin = pdrgdatumHistValues[ul];
		IDatum *pdatumMin = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false, datumMin);

		Datum datumMax = pdrgdatumHistValues[ul + 1];
		IDatum *pdatumMax = CTranslatorScalarToDXL::Pdatum(pmp, pmdtype, false, datumMax);

		CBucket *pbucket = New(pmp) CBucket
									(
									New(pmp) CPoint(pdatumMin),
									New(pmp) CPoint(pdatumMax),
									dFreqPerBucket,
									dDistinctPerBucket
									);
		pdrgppbucket->Append(pbucket);

		if (!pdatumMin->FHasStatsLessThan())
		{
			// if less than operation is not supported on this datum, then no point
			// building a histogram. return an empty histogram
			pdrgppbucket->Release();
			return New(pmp) CHistogram(New(pmp) DrgPbucket(pmp));
		}
	}

	CHistogram *phist = New(pmp) CHistogram(pdrgppbucket);
	return phist;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Pdrgpdxlbucket
//
//	@doc:
//		Histogram to array of dxl buckets
//
//---------------------------------------------------------------------------
DrgPdxlbucket *
CTranslatorRelcacheToDXL::Pdrgpdxlbucket
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	const CHistogram *phist
	)
{
	DrgPdxlbucket *pdrgpdxlbucket = New(pmp) DrgPdxlbucket(pmp);
	const DrgPbucket *pdrgpbucket = phist->Pdrgpbucket();
	ULONG ulNumBuckets = pdrgpbucket->UlLength();
	for (ULONG ul = 0; ul < ulNumBuckets; ul++)
	{
		CBucket *pbucket = (*pdrgpbucket)[ul];
		IDatum *pdatumLB = pbucket->PpLower()->Pdatum();
		CDXLDatum *pdxldatumLB = pmdtype->Pdxldatum(pmp, pdatumLB);
		IDatum *pdatumUB = pbucket->PpUpper()->Pdatum();
		CDXLDatum *pdxldatumUB = pmdtype->Pdxldatum(pmp, pdatumUB);
		CDXLBucket *pdxlbucket = New (pmp) CDXLBucket
											(
											pdxldatumLB,
											pdxldatumUB,
											pbucket->DFrequency(),
											pbucket->DDistinct()
											);
		pdrgpdxlbucket->Append(pdxlbucket);
	}
	return pdrgpdxlbucket;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::Erelstorage
//
//	@doc:
//		Get relation storage type
//
//---------------------------------------------------------------------------
IMDRelation::Erelstoragetype
CTranslatorRelcacheToDXL::Erelstorage
	(
	CHAR cStorageType
	)
{
	IMDRelation::Erelstoragetype erelstorage = IMDRelation::ErelstorageSentinel;

	switch (cStorageType)
	{
		case RELSTORAGE_HEAP:
			erelstorage = IMDRelation::ErelstorageHeap;
			break;
		case RELSTORAGE_AOCOLS:
			erelstorage = IMDRelation::ErelstorageAppendOnlyCols;
			break;
		case RELSTORAGE_AOROWS:
			erelstorage = IMDRelation::ErelstorageAppendOnlyRows;
			break;
		case RELSTORAGE_VIRTUAL:
		case RELSTORAGE_EXTERNAL:
			erelstorage = IMDRelation::ErelstorageExternal;
			break;
		default:
			GPOS_ASSERT(!"Unsupported relation type");
	}

	return erelstorage;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpulPartKeys
//
//	@doc:
//		Get partition keys for relation or NULL if relation not partitioned
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorRelcacheToDXL::PdrgpulPartKeys
	(
	IMemoryPool *pmp,
	OID oid
	)
{
	if (!gpdb::FRelPartIsRoot(oid))
	{
		// not a partitioned table
		return NULL;
	}

	// TODO: antovl - Feb 23, 2012; support intermediate levels

	DrgPul *pdrgpulPartKeys = New(pmp) DrgPul(pmp);

	MemoryContext mcxt = CurrentMemoryContext;
	PartitionNode *pn = gpdb::PpnParts(oid, 0 /*level*/, 0 /*parent*/, false /*inctemplate*/, mcxt);

	List *plPartKeys = gpdb::PlPartitionAttrs(pn);

	ListCell *plc = NULL;
	ForEach (plc, plPartKeys)
	{
		INT iAttno = lfirst_int(plc);
		GPOS_ASSERT(0 < iAttno);
		pdrgpulPartKeys->Append(New(pmp) ULONG(iAttno - 1));
	}

	return pdrgpulPartKeys;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PulAttnoMapping
//
//	@doc:
//		Construct a mapping for GPDB attnos to positions in the columns array
//
//---------------------------------------------------------------------------
ULONG *
CTranslatorRelcacheToDXL::PulAttnoMapping
	(
	IMemoryPool *pmp,
	DrgPmdcol *pdrgpmdcol,
	ULONG ulMaxCols
	)
{
	GPOS_ASSERT(NULL != pdrgpmdcol);
	GPOS_ASSERT(0 < pdrgpmdcol->UlLength());
	GPOS_ASSERT(ulMaxCols > pdrgpmdcol->UlLength());

	// build a mapping for attnos->positions
	const ULONG ulCols = pdrgpmdcol->UlLength();
	ULONG *pul = New(pmp) ULONG[ulMaxCols];

	for (ULONG ul = 0;  ul < ulCols; ul++)
	{
		const IMDColumn *pmdcol = (*pdrgpmdcol)[ul];
		INT iAttno = pmdcol->IAttno();

		ULONG ulIndex = (ULONG) (GPDXL_SYSTEM_COLUMNS + iAttno);
		pul[ulIndex] = ul;
	}

	return pul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::PdrgpdrgpulKeys
//
//	@doc:
//		Get key sets for relation
//
//---------------------------------------------------------------------------
DrgPdrgPul *
CTranslatorRelcacheToDXL::PdrgpdrgpulKeys
	(
	IMemoryPool *pmp,
	OID oid,
	ULONG *pulMapping
	)
{
	DrgPdrgPul *pdrgpdrgpul = New(pmp) DrgPdrgPul(pmp);

	List *plKeys = gpdb::PlRelationKeys(oid);

	ListCell *plcKey = NULL;
	ForEach (plcKey, plKeys)
	{
		List *plKey = (List *) lfirst(plcKey);

		DrgPul *pdrgpulKey = New(pmp) DrgPul(pmp);

		ListCell *plcKeyElem = NULL;
		ForEach (plcKeyElem, plKey)
		{
			INT iKey = lfirst_int(plcKeyElem);
			ULONG ulPos = UlPosition(iKey, pulMapping);
			pdrgpulKey->Append(New(pmp) ULONG(ulPos));
		}
		GPOS_ASSERT(0 < pdrgpulKey->UlLength());

		pdrgpdrgpul->Append(pdrgpulKey);
	}

	return pdrgpdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::NormalizeFrequencies
//
//	@doc:
//		Sometimes a set of frequencies can add up to more than 1.0.
//		Fix these cases
//
//---------------------------------------------------------------------------
void
CTranslatorRelcacheToDXL::NormalizeFrequencies
	(
	float4 *pdrgf,
	ULONG ulLength
	)
{
	if (ulLength == 0)
	{
		return;
	}

	CDouble dTotal = CDouble(pdrgf[0]);
	for (ULONG ul=1; ul<ulLength; ul++)
	{
		dTotal = dTotal + CDouble(pdrgf[ul]);
	}

	if (dTotal > CDouble(1.0))
	{
		float4 fDenom = (float4) (dTotal + CStatistics::DEpsilon).DVal();

		// divide all values by the total
		for (ULONG ul=0; ul<ulLength; ul++)
		{
			pdrgf[ul] = pdrgf[ul] / fDenom;
		}
	}

#ifdef GPOS_DEBUG
	// recheck
	CDouble dTotalRecheck = CDouble(pdrgf[0]);
	for (ULONG ul=1; ul<ulLength; ul++)
	{
		dTotalRecheck = dTotalRecheck + CDouble(pdrgf[ul]);
	}
	GPOS_ASSERT(dTotalRecheck <= CDouble(1.0));
#endif
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorRelcacheToDXL::FIndexSupported
//
//	@doc:
//		Check if index type is supported
//
//---------------------------------------------------------------------------
BOOL
CTranslatorRelcacheToDXL::FIndexSupported
	(
	HeapTupleData *pht
	)
{
	// index expressions and index constraints not supported
	return gpdb::FHeapAttIsNull(pht, Anum_pg_index_indexprs) &&
		gpdb::FHeapAttIsNull(pht, Anum_pg_index_indpred);
}


// EOF

