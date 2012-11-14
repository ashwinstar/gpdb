//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Greenplum, Inc.
//
//	@filename:
//		COptTasks.cpp
//
//	@doc:
//		Routines to perform optimization related tasks using the gpos framework
//
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "gpopt/utils/gpdbdefs.h"
#include "gpopt/utils/COptTasks.h"
#include "gpopt/relcache/CMDProviderRelcache.h"
#include "gpopt/config/CConfigParamMapping.h"
#include "gpopt/translate/CStateDXLToQuery.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CTranslatorQueryToDXL.h"
#include "gpopt/translate/CTranslatorPlStmtToDXL.h"
#include "gpopt/translate/CTranslatorDXLToPlStmt.h"
#include "gpopt/translate/CTranslatorDXLToQuery.h"
#include "gpopt/translate/CContextDXLToPlStmt.h"
#include "gpopt/translate/CTranslatorRelcacheToDXL.h"

#include "cdb/cdbvars.h"
#include "utils/guc.h"

#include "gpos/base.h"
#undef setstate

#include "gpos/_api.h"
#include "gpos/common/CAutoP.h"
#include "gpos/error/CErrorHandlerStandard.h"
#include "gpos/error/CException.h"
#include "gpos/error/CLoggerStream.h"
#include "gpos/io/COstreamFile.h"
#include "gpos/io/COstreamString.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/task/CWorkerPoolManager.h"
#include "gpos/task/CAutoTaskProxy.h"
#include "gpos/task/CTaskContext.h"
#include "gpos/common/CAutoP.h"

#include "gpopt/translate/CTranslatorDXLToExpr.h"
#include "gpopt/translate/CTranslatorExprToDXL.h"

#include "gpopt/base/CAutoOptCtxt.h"
#include "gpopt/base/CQueryContext.h"
#include "gpopt/engine/CEngine.h"
#include "gpopt/mdcache/CAutoMDAccessor.h"
#include "gpopt/mdcache/CMDCache.h"
#include "gpopt/minidump/CMiniDumperDXL.h"
#include "gpopt/minidump/CMinidumperUtils.h"
#include "gpopt/minidump/CSerializableStackTrace.h"
#include "gpopt/minidump/CSerializableQuery.h"
#include "gpopt/minidump/CSerializablePlan.h"
#include "gpopt/minidump/CSerializableTraceFlags.h"
#include "gpopt/minidump/CSerializableMDAccessor.h"
#include "gpopt/optimizer/COptimizer.h"
#include "gpopt/search/CSearchStage.h"
#include "gpopt/xforms/CXformFactory.h"

#include "base/CQueryToDXLResult.h"

#include "md/IMDId.h"
#include "md/CMDRelationGPDB.h"
#include "md/CMDIdRelStats.h"
#include "md/CMDIdColStats.h"

#include "md/CSystemId.h"
#include "md/IMDRelStats.h"
#include "md/IMDColStats.h"

#include "dxl/operators/CDXLNode.h"
#include "dxl/parser/CParseHandlerDXL.h"
#include "dxl/CDXLUtils.h"
#include "dxl/CIdGenerator.h"


#include "gpopt/gpdbwrappers.h"

using namespace gpos;
using namespace gpopt;
using namespace gpdxl;

// size of error buffer
#define GPOPT_ERROR_BUFFER_SIZE 65536

// default id for the source system
const CSystemId sysidDefault(IMDId::EmdidGPDB, GPOS_WSZ_STR_LENGTH("GPDB"));

//---------------------------------------------------------------------------
//	@function:
//		SzFromWsz
//
//	@doc:
//		Return regular string from wide-character string
//
//---------------------------------------------------------------------------
char *SzFromWsz(const WCHAR *wsz)
{
	ULONG ulMaxLength = GPOS_WSZ_LENGTH(wsz) * GPOS_SIZEOF(WCHAR) + 1;
	CHAR *sz = (CHAR *) gpdb::GPDBAlloc(ulMaxLength);
	gpos::clib::LWcsToMbs(sz, const_cast<WCHAR *>(wsz), ulMaxLength);
	sz[ulMaxLength - 1] = '\0';
	return sz;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::Execute
//
//	@doc:
//		Execute a task using GPOS. TODO extend gpos to provide
//		this functionality
//
//---------------------------------------------------------------------------
void
COptTasks::Execute
	(
	void *(*pfunc) (void *) ,
	void *pfuncArg
	)
{
	Assert(pfunc);

	if (gpos_set_threads(4, 4))
	{
		elog(ERROR, "unable to set number of threads in gpos");
		return;
	}

	bool abort_flag = false;

	char err_buf[GPOPT_ERROR_BUFFER_SIZE];

	gpos_exec_params params;
	params.func = pfunc;
	params.arg = pfuncArg;
	params.stack_start = &params;
	params.error_buffer = err_buf;
	params.error_buffer_size = sizeof(err_buf);
	params.abort_requested = &abort_flag;

	// execute task and send log message to server log
	(void) gpos_exec(&params);

	if ('\0' != err_buf[0])
	{
		elog(LOG, "%s", SzFromWsz((WCHAR *)err_buf));
	}

	return;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvDXLFromQueryTask
//
//	@doc:
//		task that does the translation from query to XML
//
//---------------------------------------------------------------------------
void*
COptTasks::PvDXLFromQueryTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(ptr);

	SOptContext *poctx = (SOptContext *) ptr;

	GPOS_ASSERT(poctx->pquery);
	GPOS_ASSERT(poctx->szQueryDXL == NULL);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab, GPOS_FTRACE(EopttraceParallel) /*fThreadSafe*/);
	IMemoryPool *pmp = amp.Pmp();

	// ColId generator
	CIdGenerator *pidgtorCol = New(pmp) CIdGenerator(GPDXL_COL_ID_START);
	CIdGenerator *pidgtorCTE = New(pmp) CIdGenerator(GPDXL_CTE_ID_START);

	// map that stores gpdb att to optimizer col mapping
	CMappingVarColId *pmapvarcolid = New(pmp) CMappingVarColId(pmp);

	// relcache MD provider
	CMDProviderRelcache *pmdpr = New(pmp) CMDProviderRelcache(pmp);

	{
		CAutoMDAccessor amda(pmp, pmdpr, sysidDefault);

		CTranslatorQueryToDXL trquerytodxl(pmp, amda.Pmda(), pidgtorCol, pidgtorCTE, pmapvarcolid, (Query*)poctx->pquery, 0);
		CDXLNode *pdxlnQuery = trquerytodxl.PdxlnFromQuery();
		DrgPdxln *pdrgpdxlnQueryOutput = trquerytodxl.PdrgpdxlnQueryOutput();
		GPOS_ASSERT(NULL != pdrgpdxlnQueryOutput);

		delete pidgtorCol;
		delete pidgtorCTE;

		CWStringDynamic str(pmp);
		COstreamString oss(&str);

		CWStringDynamic *pstrDXL = CDXLUtils::PstrSerializeQuery(pmp, pdxlnQuery, pdrgpdxlnQueryOutput, true /*fSerializeHeaderFooter*/, true /*fIndent*/);
		poctx->szQueryDXL = SzFromWsz(pstrDXL->Wsz());

		// clean up
		pdxlnQuery->Release();
		delete pstrDXL;
	}

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::Pplstmt
//
//	@doc:
//		Translate a DXL tree into a planned statement
//
//---------------------------------------------------------------------------
PlannedStmt *
COptTasks::Pplstmt
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	const CDXLNode *pdxln
	)
{

	GPOS_ASSERT(NULL != pmda);
	GPOS_ASSERT(NULL != pdxln);

	CIdGenerator idgtorPlanId(1 /* ulStartId */);
	CIdGenerator idgtorMotionId(1 /* ulStartId */);
	CIdGenerator idgtorParamId(0 /* ulStartId */);

	List *plRTable = NULL;
	List *plSubplans = NULL;

	CContextDXLToPlStmt ctxdxltoplstmt(pmp, &idgtorPlanId, &idgtorMotionId, &idgtorParamId, &plRTable, &plSubplans);

	// translate DXL -> PlannedStmt
	CTranslatorDXLToPlStmt trdxltoplstmt(pmp, pmda, &ctxdxltoplstmt);
	return trdxltoplstmt.PplstmtFromDXL(pdxln);
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PdrgPssLoad
//
//	@doc:
//		Load search strategy from given file
//
//---------------------------------------------------------------------------
DrgPss *
COptTasks::PdrgPssLoad
	(
	IMemoryPool *pmp,
	char *szPath
	)
{
	DrgPss *pdrgpss = NULL;
	CParseHandlerDXL *pphDXL = NULL;

	GPOS_TRY
	{
		if (NULL != szPath)
		{
			pphDXL = CDXLUtils::PphdxlParseDXLFile(pmp, szPath, NULL);
			if (NULL != pphDXL)
			{
				elog(LOG, "\n[OPT]: Using search strategy in (%s)", szPath);

				pdrgpss = pphDXL->Pdrgpss();
				pdrgpss->AddRef();
			}
		}
	}
	GPOS_CATCH_EX(ex)
	{
		GPOS_RESET_EX;

		elog(LOG, "\n[OPT]: Using default search strategy");
	}
	GPOS_CATCH_END;

	delete pphDXL;

	return pdrgpss;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvOptimizeTask
//
//	@doc:
//		task that does the optimizes query to physical DXL
//
//---------------------------------------------------------------------------
void*
COptTasks::PvOptimizeTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(ptr);
	SOptContext *poctx = (SOptContext *) ptr;

	GPOS_ASSERT(poctx->pquery);
	GPOS_ASSERT(poctx->szPlanDXL == NULL);
	GPOS_ASSERT(poctx->pplstmt == NULL);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab, GPOS_FTRACE(EopttraceParallel) /*fThreadSafe*/);
	IMemoryPool *pmp = amp.Pmp();

	// initialize metadata cache
	CMDCache::Init();

	// load search strategy
	DrgPss *pdrgpss = PdrgPssLoad(pmp, gp_opt_search_strategy_path);

	CBitSet *pbsTraceFlags = NULL;
	CDXLNode *pdxlnPlan = NULL;

	GPOS_TRY
	{
		// set trace flags
		pbsTraceFlags = CConfigParamMapping::PbsPack(pmp, CXform::ExfSentinel);

		// set up relcache MD provider
		CMDProviderRelcache *pmdpRelcache = New(pmp) CMDProviderRelcache(pmp);

		{
			// scope for MD accessor
			CMDAccessor mda(pmp, CMDCache::Pcache(), sysidDefault, pmdpRelcache);

			// ColId generator
			CIdGenerator idgtorColId(GPDXL_COL_ID_START);
			CIdGenerator idgtorCTE(GPDXL_CTE_ID_START);

			// map that stores gpdb att to optimizer col mapping
			CMappingVarColId *pmapvarcolid = New(pmp) CMappingVarColId(pmp);

			CTranslatorQueryToDXL trquerytodxl(pmp, &mda, &idgtorColId, &idgtorCTE, pmapvarcolid, (Query*) poctx->pquery, 0 /* ulQueryLevel */);

			// preload metadata if optimizer uses multiple threads
			if (gp_opt_parallel)
			{
				// install opt context in TLS
				CAutoOptCtxt aoc(pmp, &mda);
				CTranslatorUtils::PreloadMD(pmp, &mda, sysidDefault, (Query*) poctx->pquery);
			}

			CDXLNode *pdxlnQuery = trquerytodxl.PdxlnFromQuery();
			DrgPdxln *pdrgpdxlnQueryOutput = trquerytodxl.PdrgpdxlnQueryOutput();
			GPOS_ASSERT(NULL != pdrgpdxlnQueryOutput);

			ULONG ulSegments = gpdb::UlSegmentCountGP();

			pdxlnPlan = COptimizer::PdxlnOptimize
									(
									pmp,
									CMDCache::Pcache(),
									pbsTraceFlags,
									pdxlnQuery,
									pdrgpdxlnQueryOutput,
									pmdpRelcache,
									sysidDefault,
									ulSegments,
									gp_session_id,
									gp_command_count,
									pdrgpss
									);

			// serialize DXL to xml
			CWStringDynamic *pstrPlan = CDXLUtils::PstrSerializePlan(pmp, pdxlnPlan, true /*fSerializeHeaderFooter*/, true /*fIndent*/);
			poctx->szPlanDXL = SzFromWsz(pstrPlan->Wsz());

			// translate DXL->PlStmt only when needed
			if (poctx->fGeneratePlStmt)
			{
				poctx->pplstmt = (PlannedStmt *) gpdb::PvCopyObject(Pplstmt(pmp, &mda, pdxlnPlan));
				
				if (gp_opt_print_plan)
				{
					elog(NOTICE, "Plstmt: %s", gpdb::SzNodeToString(poctx->pplstmt));
				}
			}

			delete pstrPlan;
			pdxlnQuery->Release();
		}
	}
	GPOS_CATCH_EX(ex)
	{
		if (gpdxl::ExmaGPDB == ex.UlMajor() && gpdxl::ExmiGPDBError == ex.UlMinor())
		{
			GPOS_RESET_EX;
			elog(INFO, "GPDB Exception. Please check log for more information.");
		}
		else
		{
			CMDCache::Shutdown();
			GPOS_RETHROW(ex);
		}
	}
	GPOS_CATCH_END;

	// cleanup
	pbsTraceFlags->Release();
	CRefCount::SafeRelease(pdxlnPlan);
	CMDCache::Shutdown();

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvDXLFromPlstmtTask
//
//	@doc:
//		task that does the translation from planned stmt to XML
//
//---------------------------------------------------------------------------
void*
COptTasks::PvDXLFromPlstmtTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(ptr);

	SOptContext *poctx = (SOptContext *) ptr;

	GPOS_ASSERT(poctx->pplstmt);
	GPOS_ASSERT(poctx->szPlanDXL == NULL);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab);
	IMemoryPool *pmp = amp.Pmp();

	CIdGenerator idgtor(1);

	// relcache MD provider
	CMDProviderRelcache *pmdpr = New(pmp) CMDProviderRelcache(pmp);

	{
		CAutoMDAccessor amda(pmp, pmdpr, sysidDefault);

		CMappingParamIdScalarId mapps(pmp);

		CTranslatorPlStmtToDXL tplstmtdxl(pmp, amda.Pmda(), &idgtor, (PlannedStmt*) poctx->pplstmt, &mapps);
		CDXLNode *pdxlnPlan = tplstmtdxl.PdxlnFromPlstmt();

		GPOS_ASSERT(NULL != pdxlnPlan);

		CWStringDynamic str(pmp);
		COstreamString oss(&str);

		CWStringDynamic *pstrDXL = CDXLUtils::PstrSerializePlan(pmp, pdxlnPlan, true /*fSerializeHeaderFooter*/, true /*fIndent*/);

		poctx->szPlanDXL = SzFromWsz(pstrDXL->Wsz());

		// cleanup
		delete pstrDXL;
		pdxlnPlan->Release();
	}

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvPlstmtFromDXLTask
//
//	@doc:
//		task that does the translation from xml to dxl to pplstmt
//
//---------------------------------------------------------------------------
void*
COptTasks::PvPlstmtFromDXLTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(ptr);

	SOptContext *poctx = (SOptContext *) ptr;

	GPOS_ASSERT(poctx->pquery == NULL);
	GPOS_ASSERT(poctx->szPlanDXL);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab);
	IMemoryPool *pmp = amp.Pmp();

	CWStringDynamic str(pmp);
	COstreamString oss(&str);

	CDXLNode *pdxlnOriginal =
		CDXLUtils::PdxlnParseDXL(pmp, poctx->szPlanDXL, NULL /*XSD location*/);

	CIdGenerator idgtorPlanId(1);
	CIdGenerator idgtorMotionId(1);
	CIdGenerator idgtorParamId(0);

	List *plRTable = NULL;
	List *plSubplans = NULL;

	CContextDXLToPlStmt ctxdxlplstmt(pmp, &idgtorPlanId, &idgtorMotionId, &idgtorParamId, &plRTable, &plSubplans);

	// relcache MD provider
	CMDProviderRelcache *pmdpr = New(pmp) CMDProviderRelcache(pmp);

	{
		CAutoMDAccessor amda(pmp, pmdpr, sysidDefault);

		// translate DXL -> PlannedStmt
		CTranslatorDXLToPlStmt trdxltoplstmt(pmp, amda.Pmda(), &ctxdxlplstmt);
		PlannedStmt *pplstmt = trdxltoplstmt.PplstmtFromDXL(pdxlnOriginal);
		if (gp_opt_print_plan)
		{
			elog(NOTICE, "Plstmt: %s", gpdb::SzNodeToString(pplstmt));
		}

		GPOS_ASSERT(NULL != pplstmt);
		GPOS_ASSERT(CurrentMemoryContext);

		poctx->pplstmt = (PlannedStmt *) gpdb::PvCopyObject(pplstmt);
	}

	// cleanup
	pdxlnOriginal->Release();

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvQueryFromDXLTask
//
//	@doc:
//		task that does the translation from xml to dxl to pquery
//
//---------------------------------------------------------------------------
void*
COptTasks::PvQueryFromDXLTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(ptr);

	SOptContext *poctx = (SOptContext *) ptr;

	GPOS_ASSERT(poctx->pquery == NULL);
	GPOS_ASSERT(poctx->szQueryDXL);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab, GPOS_FTRACE(EopttraceParallel) /*fThreadSafe*/);
	IMemoryPool *pmp = amp.Pmp();

	CWStringDynamic str(pmp);
	COstreamString oss(&str);

	// parse the DXL
	CQueryToDXLResult *ptroutput = CDXLUtils::PdxlnParseDXLQuery(pmp, poctx->szQueryDXL, NULL);

	GPOS_ASSERT(NULL != ptroutput->Pdxln());

	// relcache MD provider
	CMDProviderRelcache *pmdpr = New(pmp) CMDProviderRelcache(pmp);

	{
		CAutoMDAccessor amda(pmp, pmdpr, sysidDefault);

		// initialize hash table that maintains the mapping between ColId and Var
		TEMap *ptemap = New (pmp) TEMap(pmp);

		CTranslatorDXLToQuery trdxlquery(pmp, amda.Pmda());
		CStateDXLToQuery statedxltoquery(pmp);

		Query *pquery = trdxlquery.PqueryFromDXL(ptroutput->Pdxln(), ptroutput->PdrgpdxlnOutputCols(), &statedxltoquery, ptemap, GPDXL_QUERY_LEVEL);

		CRefCount::SafeRelease(ptemap);
		delete ptroutput;

		GPOS_ASSERT(NULL != pquery);
		GPOS_ASSERT(CurrentMemoryContext);

		poctx->pquery = pquery;
	}

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvDXLFromMDObjsTask
//
//	@doc:
//		task that does dumps the relcache info for an object into DXL
//
//---------------------------------------------------------------------------
void*
COptTasks::PvDXLFromMDObjsTask
	(
	void *pv
	)
{
	GPOS_ASSERT(NULL != pv);

	SContextRelcacheToDXL *pctxrelcache = (SContextRelcacheToDXL *) pv;
	GPOS_ASSERT(NULL != pctxrelcache);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab);
	IMemoryPool *pmp = amp.Pmp();

	DrgPimdobj *pdrgpmdobj = New(pmp) DrgPimdobj(pmp, 1024, 1024);

	// relcache MD provider
	CMDProviderRelcache *pmdp = New(pmp) CMDProviderRelcache(pmp);
	{
		CAutoMDAccessor amda(pmp, pmdp, sysidDefault);
		ListCell *plc = NULL;
		ForEach (plc, pctxrelcache->plOids)
		{
			Oid oid = lfirst_oid(plc);
			// get object from relcache
			CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(oid, 1 /* major */, 0 /* minor */);

			IMDCacheObject *pimdobj = CTranslatorRelcacheToDXL::Pimdobj(pmp, amda.Pmda(), pmdid);
			GPOS_ASSERT(NULL != pimdobj);

			pdrgpmdobj->Append(pimdobj);
			pmdid->Release();
		}

		if (pctxrelcache->szFilename)
		{
			COstreamFile cofs(pctxrelcache->szFilename);

			CDXLUtils::PstrSerializeMetadata(pmp, pdrgpmdobj, cofs, true /*fSerializeHeaderFooter*/, true /*fIndent*/);
		}
		else
		{
			CWStringDynamic str(pmp);
			COstreamString oss(&str);

			CDXLUtils::PstrSerializeMetadata(pmp, pdrgpmdobj, oss, true /*fSerializeHeaderFooter*/, true /*fIndent*/);

			CHAR *sz = SzFromWsz(str.Wsz());

			GPOS_ASSERT(NULL != sz);

			pctxrelcache->szDXL = sz;
		}
	}
	// cleanup
	pdrgpmdobj->Release();

	return NULL;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PvDXLFromRelStatsTask
//
//	@doc:
//		task that dumps relstats info for a table in DXL
//
//---------------------------------------------------------------------------
void*
COptTasks::PvDXLFromRelStatsTask
	(
	void *pv
	)
{
	GPOS_ASSERT(NULL != pv);

	SContextRelcacheToDXL *pctxrelcache = (SContextRelcacheToDXL *) pv;
	GPOS_ASSERT(NULL != pctxrelcache);

	CAutoMemoryPool amp(CAutoMemoryPool::ElcExc, CMemoryPoolManager::EatSlab);
	IMemoryPool *pmp = amp.Pmp();

	// relcache MD provider
	CMDProviderRelcache *pmdpr = New(pmp) CMDProviderRelcache(pmp);
	CAutoMDAccessor amda(pmp, pmdpr, sysidDefault);
	CAutoOptCtxt aoc(pmp, amda.Pmda());

	DrgPimdobj *pdrgpmdobj = New(pmp) DrgPimdobj(pmp);

	ListCell *plc = NULL;
	ForEach (plc, pctxrelcache->plOids)
	{
		Oid oidRelation = lfirst_oid(plc);

		// get object from relcache
		CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(oidRelation, 1 /* major */, 0 /* minor */);

		// generate mdid for relstats
		CMDIdRelStats *pmdidRelStats = New(pmp) CMDIdRelStats(pmdid);
		IMDRelStats *pimdobjrelstats = const_cast<IMDRelStats *>(amda.Pmda()->Pmdrelstats(pmdidRelStats));
		pdrgpmdobj->Append(dynamic_cast<IMDCacheObject *>(pimdobjrelstats));

		// extract out column stats for this relation
		Relation rel = gpdb::RelGetRelation(oidRelation);
		ULONG ulPosCounter = 0;
		for (ULONG ul = 0; ul < ULONG(rel->rd_att->natts); ul++)
		{
			if (!rel->rd_att->attrs[ul]->attisdropped)
			{
				pmdid->AddRef();
				CMDIdColStats *pmdidColStats = New(pmp) CMDIdColStats(pmdid, ulPosCounter);
				ulPosCounter++;
				IMDColStats *pimdobjcolstats = const_cast<IMDColStats *>(amda.Pmda()->Pmdcolstats(pmdidColStats));
				pdrgpmdobj->Append(dynamic_cast<IMDCacheObject *>(pimdobjcolstats));
				pmdidColStats->Release();
			}
		}
		gpdb::CloseRelation(rel);
		pmdidRelStats->Release();
	}

	if (pctxrelcache->szFilename)
	{
		COstreamFile cofs(pctxrelcache->szFilename);

		CDXLUtils::PstrSerializeMetadata(pmp, pdrgpmdobj, cofs, true /*fSerializeHeaderFooter*/, true /*fIndent*/);
	}
	else
	{
		CWStringDynamic str(pmp);
		COstreamString oss(&str);

		CDXLUtils::PstrSerializeMetadata(pmp, pdrgpmdobj, oss, true /*fSerializeHeaderFooter*/, true /*fIndent*/);

		CHAR *sz = SzFromWsz(str.Wsz());

		GPOS_ASSERT(NULL != sz);

		pctxrelcache->szDXL = sz;
	}

	// cleanup
	pdrgpmdobj->Release();

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SzOptimize
//
//	@doc:
//		optimizes a query to physical DXL
//
//---------------------------------------------------------------------------
char *
COptTasks::SzOptimize
	(
	Query *pquery
	)
{
	Assert(pquery);

	SOptContext octx;
	octx.pquery = pquery;
	octx.szPlanDXL = NULL;
	octx.pplstmt = NULL;
	octx.fGeneratePlStmt = false;
	Execute(&PvOptimizeTask, &octx);

	return octx.szPlanDXL;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PplstmtOptimize
//
//	@doc:
//		optimizes a query to plannedstmt
//
//---------------------------------------------------------------------------
PlannedStmt *
COptTasks::PplstmtOptimize
	(
	Query *pquery
	)
{
	Assert(pquery);


	SOptContext octx;
	octx.pquery = pquery;
	octx.szPlanDXL = NULL;
	octx.pplstmt = NULL;
	octx.fGeneratePlStmt= true;
	Execute(&PvOptimizeTask, &octx);

	return octx.pplstmt;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SzDXL
//
//	@doc:
//		serializes query to DXL
//
//---------------------------------------------------------------------------
char *
COptTasks::SzDXL
	(
	Query *pquery
	)
{
	Assert(pquery);

	SOptContext octx;
	octx.pquery = pquery;
	octx.szQueryDXL = NULL;
	octx.fGeneratePlStmt = false;
	Execute(&PvDXLFromQueryTask, &octx);

	return octx.szQueryDXL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SzDXL
//
//	@doc:
//		serializes planned stmt to DXL
//
//---------------------------------------------------------------------------
char *COptTasks::SzDXL
	(
	PlannedStmt *pplstmt
	)
{
	Assert(pplstmt);

	SOptContext octx;
	octx.pplstmt = pplstmt;
	octx.szPlanDXL = NULL;
	octx.fGeneratePlStmt = false;
	Execute(&PvDXLFromPlstmtTask, &octx);

	return octx.szPlanDXL;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PqueryFromXML
//
//	@doc:
//		deserializes query from DXL
//
//---------------------------------------------------------------------------
Query *COptTasks::PqueryFromXML(char *szDXL)
{
	Assert(szDXL);

	SOptContext octx;
	octx.pquery = NULL;
	octx.szQueryDXL = szDXL;
	octx.fGeneratePlStmt = false;
	Execute(&PvQueryFromDXLTask, &octx);

	return (Query *) octx.pquery;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::PplstmtFromXML
//
//	@doc:
//		deserializes planned stmt from DXL
//
//---------------------------------------------------------------------------
PlannedStmt *
COptTasks::PplstmtFromXML
	(
	char *szDXL
	)
{
	Assert(szDXL);

	SOptContext octx;
	octx.pquery = NULL;
	octx.pplstmt = NULL;
	octx.szPlanDXL = szDXL;
	octx.fGeneratePlStmt = false;

	Execute(&PvPlstmtFromDXLTask, &octx);

	return octx.pplstmt;
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::DumpMDObjs
//
//	@doc:
//		Dump relcache objects into DXL file
//
//---------------------------------------------------------------------------
void
COptTasks::DumpMDObjs
	(
	List *plOids,
	const char *szFilename
	)
{
	SContextRelcacheToDXL ctxrelcache;
	ctxrelcache.plOids = plOids;
	ctxrelcache.szFilename = szFilename;
	Execute(&PvDXLFromMDObjsTask, &ctxrelcache);
}


//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SzMDObjs
//
//	@doc:
//		Dump relcache objects into DXL string
//
//---------------------------------------------------------------------------
char *
COptTasks::SzMDObjs
	(
	List *plistOids
	)
{
	SContextRelcacheToDXL ctxrelcache;
	ctxrelcache.plOids = plistOids;
	ctxrelcache.szFilename = NULL;
	Execute(&PvDXLFromMDObjsTask, &ctxrelcache);
	return ctxrelcache.szDXL;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SzRelStats
//
//	@doc:
//		Dump statistics objects into DXL string
//
//---------------------------------------------------------------------------
char *
COptTasks::SzRelStats
	(
	List *plistOids
	)
{
	SContextRelcacheToDXL ctxrelcache;
	ctxrelcache.plOids = plistOids;
	ctxrelcache.szFilename = NULL;
	Execute(&PvDXLFromRelStatsTask, &ctxrelcache);
	return ctxrelcache.szDXL;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::FSetXform
//
//	@doc:
//		Enable/Disable a given xform
//
//---------------------------------------------------------------------------
bool
COptTasks::FSetXform
	(
	char *szXform,
	bool fDisable
	)
{
	CXform *pxform = CXformFactory::Pxff()->Pxf(szXform);
	if (NULL != pxform)
	{
		gp_opt_xforms[pxform->Exfid()] = fDisable;

		return true;
	}

	return false;
}

// EOF
