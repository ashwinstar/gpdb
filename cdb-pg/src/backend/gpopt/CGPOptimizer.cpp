//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 Greenplum, Inc.
//
//	@filename:
//		CGPOptimizer.cpp
//
//	@doc:
//		Entry point to GP optimizer
//
//	@owner:
//		solimm1
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "gpopt/CGPOptimizer.h"
#include "gpopt/utils/COptTasks.h"

// the following headers are needed to reference optimizer library initializers
#include "init.h"
#include "gpopt/init.h"
#include "gpos/_api.h"

//---------------------------------------------------------------------------
//	@function:
//		CGPOptimizer::TouchLibraryInitializers
//
//	@doc:
//		Touch library initializers to enforce linker to include them
//
//---------------------------------------------------------------------------
void
CGPOptimizer::TouchLibraryInitializers()
{
	void (*gpos)() = gpos_init;
	void (*dxl)() = gpdxl_init;
	void (*opt)() = gpopt_init;
}


//---------------------------------------------------------------------------
//	@function:
//		CGPOptimizer::PlstmtOptimize
//
//	@doc:
//		Optimize given query using GP optimizer
//
//---------------------------------------------------------------------------
PlannedStmt *
CGPOptimizer::PplstmtOptimize
	(
	Query *pquery
	)
{
	return COptTasks::PplstmtOptimize(pquery);
}


//---------------------------------------------------------------------------
//	@function:
//		PplstmtOptimize
//
//	@doc:
//		Expose GP optimizer API to C files
//
//---------------------------------------------------------------------------
extern "C"
{
PlannedStmt *PplstmtOptimize
	(
	Query *pquery
	)
{
	return CGPOptimizer::PplstmtOptimize(pquery);
}
}

// EOF
