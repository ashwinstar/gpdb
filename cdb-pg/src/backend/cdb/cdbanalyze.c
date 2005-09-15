/*-------------------------------------------------------------------------
 *
 * cdbanalyze.c
 *	  Provides routines for performing distributed analyze cdb
 *
 * Copyright (c) 2003-2004, Greenplum
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "commands/vacuum.h" /* For vac_update_relstats */
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "utils/relcache.h"  /* For RelationGetIndexList */
#include "cdb/cdbvars.h"
#include "cdb/cdbanalyze.h"

/*
 * Flag used to tell "get_table_policy" to report that local tables are partitioned.
 * Initially, and normally, false.
 * When set to true, queries against catalog tables will be parallelized.
 * This must never be left at true.  Once the query is finished, it must be set back to false.
 */
extern bool bTreatLocalsAsPartitioned;

/*
 * These defines represent the column positions of the fields in the query we
 * create to select from pg_class on the qExecs.
 */
#define Anum_pg_class_avg_tuples	1
#define Anum_pg_class_max_tuples	2
#define Anum_pg_class_avg_pages		3
#define Anum_pg_class_max_pages		4

/*
 * cdb_get_relstats:
 * This takes the oid of a relation as a parameter, and returns
 * aggregate numbers for the number of pages and number of tuples in out parameters.
 * These are obtained by running a query, using SPI, against pg_class
 * on the qExecs.  Since we want the query to be parallelized, we use
 * the kludgy trick of setting bTreatLocalsAsPartitioned to true
 * before we execute the query.
 * The out parameters are computed to be the max of the number of pages
 * and number of tuples over the qExecs.
 * This must only be called from the dispatcher.
 */
void cdb_get_relstats(Oid relid, BlockNumber *pages, double *tuples)
{
	StringInfoData		buffer;
	int					rtn;
	HeapTuple			tup;
	TupleDesc			tupdesc;
	char				*tmp;

	Assert( Mpp_role == MPP_ROLE_DISPATCH );
	Assert( pages != NULL );
	Assert( tuples != NULL );

	/*
	 * Get aggregate pages and tuples from qExecs
	 */

	/*
	 * First, create a select statment
	 */
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "select avg(reltuples), max(reltuples), avg(relpages), max(relpages)" );
	appendStringInfo(&buffer, " from pg_class");
	appendStringInfo(&buffer, " where oid=%u", relid);

	/*
	 * Next, use SPI to execute it.
	 */
	if (SPI_OK_CONNECT != SPI_connect())
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("SPI_connect failed in cdb_update_relstats" )));
	}

	/*
	 * Flip the switch to turn local tables into partitioned for the parallelizer.
	 * Do the query.
	 * Flip the switch back.
	 */
	bTreatLocalsAsPartitioned = true;
	rtn = SPI_execute( buffer.data, false, 0 );
	bTreatLocalsAsPartitioned = false;

	if (SPI_OK_SELECT != rtn)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("SPI_execute failed in cdb_update_relstats" )));
	}

	/*
	 * Grab the results of the query and stuff them into the out parameters.
	 */
	Assert( SPI_processed == 1 );
	tup = SPI_tuptable->vals[0];
	tupdesc = SPI_tuptable->tupdesc;
	Assert(tup != NULL && tupdesc != NULL);

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_tuples);
	if (tmp == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("cdb_update_relstats: invalid tuple count string")));
	}
	*tuples = atof(tmp);

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_pages);
	if (tmp == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("cdb_update_relstats: invalid page count string")));
	}
	*pages = atoi(tmp);

	/*
	 * Clean up.
	 */
	SPI_finish();

}
