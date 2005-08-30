/*-------------------------------------------------------------------------
 *
 * cdblink.c
 *
 *	  Setting up libpq connections to qRxecs from Dispatcher
 *
 *
 * ATTRIBUTION
 *
 *	   Concept and initial logic based on contrib/dblink by:
 *
 *	   Joe Conway <mail@joeconway.com>
 *	   And contributors:
 *	   Darko Prenosil <Darko.Prenosil@finteh.hr>
 *	   Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 *	   Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *	   ALL RIGHTS RESERVED;
 *
 *	   Permission to use, copy, modify, and distribute this software and its
 *	   documentation for any purpose, without fee, and without a written agreement
 *	   is hereby granted, provided that the above copyright notice and this
 *	   paragraph and the following two paragraphs appear in all copies.
 *
 *	   IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 *	   DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 *	   LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 *	   DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 *	   POSSIBILITY OF SUCH DAMAGE.
 *
 *	   THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 *	   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 *	   AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 *	   ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 *	   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *
 */
#include "postgres.h"

#include <ctype.h>
#include <pthread.h>

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "nodes/makefuncs.h"

#include "cdb/cdbcat.h"
#include "cdb/cdbconn.h"
#include "cdb/cdblink.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"


/*
 * Called by backend/utils/init/postinit.c to setup stuffe
 *
 * Note -- this function is called with TopTransactionContext *before*
 *		MessageContext and the other statement-oriented contexts are
 *		allocated.	We want to store the SegmentDbArray and the
 *		heap returned by getCdbSegmentInstances() in TopMemoryContext
 *		(basically permanent storage).	TopTransactionContext is
 *		reset shortly after this routine is called.
 */

void
cdblink_setup(void)
{
	/*
	 * If cdb_role is UTILITY, skip this call.
	 */


	if (Cdb_role == CDB_ROLE_UTILITY)
		return;

	getCdbCdb();


	return;
}


/* Alas, Darwin doesn't have this very useful facility. */
#if !defined(__darwin__)

#include <execinfo.h>

static void
log_backtrace(void)
{
	void	   *array[10];
	size_t		size;
	char	  **strings;
	size_t		i;

	size = backtrace(array, 10);
	strings = backtrace_symbols(array, size);
	for (i = 0; i < size; i++)
		elog(DEBUG3, "%s\n", strings[i]);
	free(strings);
}

#else
/* assume that we're using GNU compiler */
#define log_backtrace() elog(DEBUG3, "called by %p", __builtin_return_address(0))
#endif

/*
 * Returns the number of segments
 *
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy	no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int
getCdbSegmentCount(void)
{
	if (Cdb_role == CDB_ROLE_UTILITY)
	{

		if (CdbIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getCdbSegmentCount called when Cdb_role == utility. returning zero segments.");
			return 0;
		}

		elog(DEBUG1, "getCdbSegmentCount called when Cdb_role == utility, but is relying on cdb.cdb info");
		log_backtrace();

	}

	if (Cdb_role == CDB_ROLE_EXECUTE)
	{
		if (CdbIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getCdbSegmentCount called when Cdb_role == execute. returning zero segments.");
			return 0;
		}
		elog(DEBUG1, "getCdbSegmentCount called when Cdb_role == execute, but is relying on cdb.cdb info");
		log_backtrace();
	}
	/* Assert( Cdb_role != CDB_ROLE_EXECUTE ); */

	if (CdbIdentity.numsegments <= 0)
	{
		elog(DEBUG1, "getCdbSegmentCount called BEFORE cdb.cdb record has been read in.");
		log_backtrace();
	}

	/* Assert( Cdb_role != CDB_ROLE_UTILITY ); */

	return CdbIdentity.numsegments;
}





void *
getEntrySegDB(int *count)
{
	static CdbComponentDatabases *cdb_component_dbs = NULL;

	if (cdb_component_dbs == NULL)
		cdb_component_dbs = getCdbComponentDatabases();
	*count = cdb_component_dbs->total_entry_dbs;
	return cdb_component_dbs->entry_db_info;
}
