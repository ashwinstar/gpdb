//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CCTEListEntry.h
//
//	@doc:
//		Class representing the list of common table expression defined at a
//		query level
//
//	@owner:
//		raghav
//
//	@test:
//
//---------------------------------------------------------------------------

#ifndef GPDXL_CCTEListEntry_H
#define GPDXL_CCTEListEntry_H

#include "gpos/base.h"
#include "gpos/common/CDynamicPtrArray.h"
#include "gpos/common/CHashMap.h"

#include "dxl/CIdGenerator.h"

// fwd declaration
struct Query;
struct List;
struct RangeTblEntry;

namespace gpdxl
{

	//---------------------------------------------------------------------------
	//	@class:
	//		CCTEListEntry
	//
	//	@doc:
	//		Class representing the list of common table expression defined at a
	//		query level
	//
	//---------------------------------------------------------------------------
	class CCTEListEntry : public CRefCount
	{
		private:

			// query level where the CTEs are defined
			ULONG m_ulQueryLevel;

			// list of CTEs defined at a particular level
			List *m_plist;

			// the starting CTE id
			ULONG m_ulCTEStartId;

			// the position of the CTE referenced in the range table entry
			ULONG UlPos(const RangeTblEntry *prte) const;

		public:
			// ctor
			CCTEListEntry(ULONG ulQueryLevel, List *plist, CIdGenerator *pulidgtorCTE);

			~CCTEListEntry()
			{};

			// the query level
			ULONG UlQueryLevel() const
			{
				return m_ulQueryLevel;
			}

			// unique id of the CTE referenced in the range table entry
			ULONG UlCTEId(const RangeTblEntry *prte) const;

			// query of the CTE referenced in the range table entry
			Query *Pquery(const RangeTblEntry *prte) const;
	};

	// hash maps mapping ULONG -> CCTEListEntry
	typedef CHashMap<ULONG, CCTEListEntry, gpos::UlHash<ULONG>, gpos::FEqual<ULONG>,
	CleanupDelete<ULONG>, CleanupRelease > HMUlCTEListEntry;

	// iterator
	typedef CHashMapIter<ULONG, CCTEListEntry, gpos::UlHash<ULONG>, gpos::FEqual<ULONG>,
	CleanupDelete<ULONG>, CleanupRelease > HMIterUlCTEListEntry;

	}
#endif // !GPDXL_CCTEListEntry_H

//EOF
