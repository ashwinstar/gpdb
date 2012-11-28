//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 Greenplum, Inc.
//
//	@filename:
//		CGPOptimizer.h
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
#ifndef CGPOptimizer_H
#define CGPOptimizer_H

#include "postgres.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"

class CGPOptimizer
{
	private:

		// touch library initializers to enforce linker to include them
		static
		void TouchLibraryInitializers();

	public:

		// optimize given query using GP optimizer
		static
		PlannedStmt *PplstmtOptimize(Query *pquery);
};

#endif // CGPOptimizer_H
