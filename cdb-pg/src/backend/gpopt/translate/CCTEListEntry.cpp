//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CCTEListEntry.cpp
//
//	@doc:
//		Implementation of the class representing the list of common table
//		expression defined at a query level
//	@owner:
//		raghav
//
//	@test:
//
//
//---------------------------------------------------------------------------

#include "postgres.h"
#include "gpopt/translate/CCTEListEntry.h"

#include "nodes/parsenodes.h"

#include "gpos/base.h"
#include "gpopt/gpdbwrappers.h"
using namespace gpdxl;

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::CCTEListEntry
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CCTEListEntry::CCTEListEntry
	(
	ULONG ulQueryLevel,
	List *plist,
	CIdGenerator *pulidgtorCTE
	)
	:
	m_ulQueryLevel(ulQueryLevel),
	m_plist(plist)
{
	GPOS_ASSERT(NULL != plist);
	GPOS_ASSERT(NULL != pulidgtorCTE);

	m_ulCTEStartId = pulidgtorCTE->UlNextId();

	const ULONG ulLen = gpdb::UlListLength(plist);
	for(ULONG ul = 0; ul < ulLen; ul++)
	{
		// increment the cte id
		(void) pulidgtorCTE->UlNextId();
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::UlPos
//
//	@doc:
//		Return the position of the given CTE referenced in the range table entry
//
//---------------------------------------------------------------------------
ULONG
CCTEListEntry::UlPos
	(
	const RangeTblEntry *prte
	)
	const
{
	ListCell *plc;
	ULONG ulCounter = 0;
	CommonTableExpr *pcte = NULL;

	ForEach(plc, m_plist)
	{
		pcte = (CommonTableExpr *) lfirst(plc);

		if (gpos::clib::IStrCmp(pcte->ctename, prte->ctename) == 0)
		{
			return ulCounter;
		}
		ulCounter++;
	}

	return ULONG_MAX;
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::UlCTEId
//
//	@doc:
//		Return the unique id of the CTE referenced in the range table entry
//
//---------------------------------------------------------------------------
ULONG
CCTEListEntry::UlCTEId
	(
	const RangeTblEntry *prte
	)
	const
{
	ULONG ulPos = UlPos(prte);
	GPOS_ASSERT(ULONG_MAX != ulPos);

	return m_ulCTEStartId + ulPos;
}

//---------------------------------------------------------------------------
//	@function:
//		CCTEListEntry::Pquery
//
//	@doc:
//		Return the query of the CTE referenced in the range table entry
//
//---------------------------------------------------------------------------
Query *
CCTEListEntry::Pquery
	(
	const RangeTblEntry *prte
	)
	const
{
	ULONG ulPos = UlPos(prte);
	GPOS_ASSERT(ULONG_MAX != ulPos);

	CommonTableExpr *pcte = (CommonTableExpr *) gpdb::PvListNth(m_plist, ulPos);
	GPOS_ASSERT(NULL != pcte);
	GPOS_ASSERT(!pcte->cterecursive);

	Query *pqueryCte = (Query *) pcte->ctequery;
	GPOS_ASSERT(NULL != pqueryCte);

	return pqueryCte;
}

// EOF
