/*-------------------------------------------------------------------------
 *
 * cdbanalyze.c
 *	  Provides routines for performing distributed analyze cdb
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "cdb/cdbpathlocus.h"	/* cdbpathlocus_querysegmentcatalogs */
#include "cdb/cdbvars.h"		/* Mpp_role */
#include "cdb/cdbanalyze.h"		/* me */

/*
 * Flag used to tell "get_table_policy" to report that local tables are partitioned.
 * Initially, and normally, false.
 * When set to true, queries against catalog tables will be parallelized.
 * This must never be left at true.  Once the query is finished, it must be set back to false.
 */
extern bool bTreatLocalsAsPartitioned;

/*
 * cdbanalyze_query_local_catalogs
 *
 * Execute a DML statement upon the segment databases' local catalogs.
 * This must only be called from the dispatcher.
 */
static void
cdbanalyze_query_local_catalogs(StringInfo sqlstmt,
								void (*callback) (StringInfo sqlstmt,
												  int spiresult,
												  void *context),
								void *context)
{
	int			spiresult;
	volatile bool connected = false;

	Assert(callback &&
		   Mpp_role == MPP_ROLE_DISPATCH);

	PG_TRY();
	{
		/* Establish an SPI session as a client of myself. */
		if (SPI_OK_CONNECT != SPI_connect())
		{
			ereport(ERROR, (errcode(ERRCODE_CDB_INTERNAL_ERROR),
			   errmsg("Unable to obtain statistics from segment databases."),
							errdetail("SPI_connect failed in cdbanalyze_query_local_catalogs")));
		}
		connected = true;

		elog(DEBUG2, "Special execute: %s", sqlstmt->data);

		/*
		 * Make the query access QE local catalogs instead of global catalog,
		 * by treating catalog tables as if they are partitioned user tables.
		 */
		cdbpathlocus_querysegmentcatalogs = true;		/* (kludgy trick) */

		/* Do the query. */
		spiresult = SPI_execute(sqlstmt->data, false, 0);

		/* Restore catalog access to normal. */
		cdbpathlocus_querysegmentcatalogs = false;

		/* Invoke caller's callback function to process the result. */
		callback(sqlstmt, spiresult, context);

		/* End recursive session. */
		connected = false;
		SPI_finish();
	}

	/* Clean up in case of error. */
	PG_CATCH();
	{
		/* Restore catalog queries to normal. */
		cdbpathlocus_querysegmentcatalogs = false;

		/* End recursive session. */
		if (connected)
			SPI_finish();

		/* Carry on with error handling. */
		PG_RE_THROW();
	}
	PG_END_TRY();
}	/* cdbanalyze_query_local_catalogs */


/*
 * These defines represent the column positions of the fields in the query we
 * create to select from pg_class on the qExecs.
 */
#define Anum_pg_class_max_tuples	1
#define Anum_pg_class_max_pages		2


/*
 * cdbanalyze_callback
 *
 * Called by cdbanalyze_query_local_catalogs to process the result of the
 * segdb local catalog query.
 */
typedef struct Cdbanalyze_Ctx
{
	Oid			relid;
	BlockNumber pages;
	double		tuples;
} Cdbanalyze_Ctx;

static void
cdbanalyze_callback(StringInfo sqlstmt,
					int spiresult,
					void *context)
{
	HeapTuple	tup;
	TupleDesc	tupdesc;
	char	   *tmp;
	Cdbanalyze_Ctx *ctx = (Cdbanalyze_Ctx *) context;

	/* Did the query succeed? */
	if (spiresult != SPI_OK_SELECT ||
		SPI_processed != 1)
		goto fail;

	/* Grab the results of the query and stuff them into the context. */
	tup = SPI_tuptable->vals[0];
	tupdesc = SPI_tuptable->tupdesc;
	if (!tup ||
		!tupdesc)
		goto fail;

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_tuples);
	if (SPI_result != 0 && !tmp)
		goto fail;
	if (!tmp)
		ereport(NOTICE, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("NULL returned instead of statistics from segment databases."),
					  errdetail("Segment db catalog query : %s",
								sqlstmt->data )));
	if (tmp)
		ctx->tuples = atof(tmp);
	else
		ctx->tuples = 2000;			// Work around aggregate bug

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_pages);
	if (SPI_result != 0 && !tmp)
		goto fail;
	if (tmp)
		ctx->pages = atoi(tmp);
	else
		ctx->pages = 50;			// Work around aggregate bug

	elog(DEBUG3, "cdbanalyze oid=%d pages=%d tuples=%f",
		 ctx->relid, ctx->pages, ctx->tuples);

	return;

fail:
	ereport(WARNING, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("Unable to obtain statistics from segment databases."),
					  errdetail("Segment db catalog query failed: %s  %d",
								sqlstmt->data, SPI_result )));
}	/* cdbanalyze_callback */


/*
 * cdbanalyze_get_relstats:
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
void
cdbanalyze_get_relstats(Relation rel, BlockNumber *pages, double *tuples)
{
	StringInfoData buffer;
	Cdbanalyze_Ctx ctx;
	char	   *relname;
	Oid			namespace;

	*pages = 0;
	*tuples = 0;

	relname = RelationGetRelationName(rel);
	namespace = RelationGetNamespace(rel);

	/*
	 * First, create a select statement
	 */
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "select max(reltuples), max(relpages)");
	appendStringInfo(&buffer, " from pg_class");
	appendStringInfo(&buffer, " where relname='%s' and relnamespace=%u", relname, namespace);

	/*
	 * Next, use SPI to execute it.
	 * (If unsuccessful, throws an error and does not return here.)
	 */
	ctx.relid = RelationGetRelid(rel);
	cdbanalyze_query_local_catalogs(&buffer, cdbanalyze_callback, &ctx);

	/*
	 * Grab the results of the query and stuff them into the out parameters.
	 */
	*tuples = ctx.tuples;
	*pages = ctx.pages;
}	/* cdbanalyze_get_relstats */


#if 0
/*
 * cdbanalyze_callback
 *
 * Called by cdbanalyze_query_local_catalogs to process the result of the
 * segdb local catalog query.
 */
typedef struct Cdbanalyze_columns_Ctx
{
	Oid			relid;
	BlockNumber pages;
	double		tuples;
} Cdbanalyze_columns_Ctx;

static void
cdbanalyze_columns_callback(StringInfo sqlstmt,
					int spiresult,
					void *context)
{
	HeapTuple	tup;
	TupleDesc	tupdesc;
	char	   *tmp;
	Cdbanalyze_columns_Ctx *ctx = (Cdbanalyze_columns_Ctx *) context;

	/* Did the query succeed? */
	if (spiresult != SPI_OK_SELECT ||
		SPI_processed != 1)
		goto fail;

	/* Grab the results of the query and stuff them into the context. */
	tup = SPI_tuptable->vals[0];
	tupdesc = SPI_tuptable->tupdesc;
	if (!tup ||
		!tupdesc)
		goto fail;

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_tuples);
	if (SPI_result != 0 && !tmp)
		goto fail;
	if (!tmp)
		ereport(NOTICE, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("NULL returned instead of statistics from segment databases."),
					  errdetail("Segment db catalog query : %s",
								sqlstmt->data )));


	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_pages);
	if (SPI_result != 0 && !tmp)
		goto fail;


	elog(DEBUG3, "cdbanalyze oid=%d pages=%d tuples=%f",
		 ctx->relid, ctx->pages, ctx->tuples);

	return;

fail:
	ereport(WARNING, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("Unable to obtain statistics from segment databases."),
					  errdetail("Segment db catalog query failed: %s  %d",
								sqlstmt->data, SPI_result )));
}	/* cdbanalyze_callback */


void
cdbanalyze_get_columnstats(Relation rel, BlockNumber *pages, double *tuples)
{
	StringInfoData buffer;
	Cdbanalyze_columns_Ctx ctx;
	char	   *relname;
	Oid			namespace;

	*pages = 0;
	*tuples = 0;

	relname = RelationGetRelationName(rel);
	namespace = RelationGetNamespace(rel);

	/*
	 * First, create a select statement
	 */
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "select staattnum,avg(stanullfrac),avg(stawidth),avg(stadistinct)");
	appendStringInfo(&buffer, " from pg_statistic, pg_class");
	appendStringInfo(&buffer, " where relname='%s' and relnamespace=%u", relname, namespace);
	appendStringInfo(&buffer, " group by staattnum order by staattnum");

	/*
	 * Next, use SPI to execute it.
	 * (If unsuccessful, throws an error and does not return here.)
	 */
	ctx.relid = RelationGetRelid(rel);
	cdbanalyze_query_local_catalogs(&buffer, cdbanalyze_columns_callback, &ctx);

	/*
	 * Grab the results of the query and stuff them into the out parameters.
	 */
	//*tuples = ctx.tuples;
	//*pages = ctx.pages;
}	/* cdbanalyze_get_columnstats */
#endif 
