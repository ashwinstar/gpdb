//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 EMC Greenplum, Inc.
//
//	@filename:
//		COptTasks.h
//
//	@doc:
//		Tasks that will perform optimization and related tasks
//
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------
#ifndef COptTasks_H
#define COptTasks_H

#include "gpopt/base/CColRef.h"
#include "gpopt/search/CSearchStage.h"


// fwd decl
namespace gpos
{
	class IMemoryPool;
	class CBitSet;
}

namespace gpdxl
{
	class CDXLNode;
}

namespace gpopt
{
	class CExpression;
	class CMDAccessor;
	class CQueryContext;
}

struct PlannedStmt;
struct Query;
struct List;

using namespace gpos;
using namespace gpdxl;
using namespace gpopt;

class COptTasks
{

	private:

		// execute a task given the argument
		static void Execute ( void *(*pfunc) (void *), void *pfuncArg);

		// routines related to serialization of pstmt and query to DXL and back
		struct SOptContext
		{
			char *szQueryDXL;
			Query *pquery;
			char *szPlanDXL;
			PlannedStmt *pplstmt;
			bool fGeneratePlStmt;
		};

		static void* PvDXLFromPlstmtTask(void *pv);
		static void* PvPlstmtFromDXLTask(void *pv);

		static void* PvDXLFromQueryTask(void *pv);
		static void* PvQueryFromDXLTask(void *pv);

		// routines relating to serialization of catalog objects to DXL
		struct SContextRelcacheToDXL
		{
			List *plOids;
			const char *szFilename; // if filename is not null, then output will be written to file
			char *szDXL;	  // if filename is null, then output will be stored here
		};

		// dump relcache info for an object into DXL
		static
		void* PvDXLFromMDObjsTask(void *pv);

		// dump relstats info for an object into DXL
		static
		void* PvDXLFromRelStatsTask(void *pv);

		// optimize a query to a physical DXL
		static
		void* PvOptimizeTask(void *pv);

		// translate a DXL tree into a planned statement
		static
		PlannedStmt *Pplstmt(IMemoryPool *pmp, CMDAccessor *pmda, const CDXLNode *pdxln);

		// load search strategy from given path
		static
		DrgPss *PdrgPssLoad(IMemoryPool *pmp, char *szPath);

	public:

		// Convert Query->DXL->LExpr->Optimize->PExpr->DXL
		static
		char *SzOptimize(Query *pquery);

		// Optimizer Query->DXL->LExpr->Optimize->PExpr->DXL->PlannedStmt
		static
		PlannedStmt *PplstmtOptimize(Query *pquery);

		// Convert query to DXL to xml string.
		static
		char *SzDXL(Query *pquery);

		// Convert planned statement to DXL to xml string.
		static
		char *SzDXL(PlannedStmt *pplstmt);

		// Convert xml string to DXL and to Query
		static
		Query *PqueryFromXML(char *szXmlString);

		// Convert xml string to DXL and to PS
		static
		PlannedStmt *PplstmtFromXML(char *szXmlString);

		// Dump metadata objects from relcache to file in DXL format
		static
		void DumpMDObjs(List *oids, const char *szFilename);

		// Dump metadata objects from relcache to a string in DXL format
		static
		char *SzMDObjs(List *oids);

		// Dump statistics from relcache to a string in DXL format
		static
		char *SzRelStats(List *oids);

		// enable/disable a given xforms
		static
		bool FSetXform(char *szXform, bool fDisable);

};

#endif // COptTasks_H

// EOF
