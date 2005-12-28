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
#include "commands/vacuum.h"
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




#define Anum_staattnum		1
#define Anum_stanullfrac	2
#define Anum_stawidth		3
#define Anum_stadistinct	4

/*
#define Anum_pg_statistic_stakind1		6
#define Anum_pg_statistic_stakind2		7
#define Anum_pg_statistic_stakind3		8
#define Anum_pg_statistic_stakind4		9
#define Anum_pg_statistic_staop1		10
#define Anum_pg_statistic_staop2		11
#define Anum_pg_statistic_staop3		12
#define Anum_pg_statistic_staop4		13
#define Anum_pg_statistic_stanumbers1	14
#define Anum_pg_statistic_stanumbers2	15
#define Anum_pg_statistic_stanumbers3	16
#define Anum_pg_statistic_stanumbers4	17
#define Anum_pg_statistic_stavalues1	18
#define Anum_pg_statistic_stavalues2	19
#define Anum_pg_statistic_stavalues3	20
#define Anum_pg_statistic_stavalues4	21
*/

/*
 * cdbanalyze_callback
 *
 * Called by cdbanalyze_query_local_catalogs to process the result of the
 * segdb local catalog query.
 */
typedef struct Cdbanalyze_columns_Ctx
{
	Oid			relid;
	VacAttrStats  * stats;
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
	
	
	Assert(ctx != NULL);
	Assert(ctx->stats != NULL);

	/* Did the query succeed? */
	if (spiresult != SPI_OK_SELECT)
		goto fail;
		
	if (SPI_processed < 1)
		goto fail;

	/* Grab the results of the query and stuff them into the context. */
	tup = SPI_tuptable->vals[0];
	tupdesc = SPI_tuptable->tupdesc;
	if (!tup ||
		!tupdesc)
		goto fail;

	tmp = SPI_getvalue(tup, tupdesc, Anum_stanullfrac);
	if (SPI_result != 0)
		goto fail;
	if (!tmp)
		ereport(NOTICE, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("NULL returned instead of statistics from segment databases."),
					  errdetail("Segment db catalog query : %s",
								sqlstmt->data )));
								
	// Fraction that are null
	if (tmp)
		ctx->stats->stanullfrac = atof(tmp);
	 


	tmp = SPI_getvalue(tup, tupdesc, Anum_stawidth);
	if (SPI_result != 0)
		goto fail;
		
	// Average width of the column (width of pointer if toasted).
	if (tmp)
		ctx->stats->stawidth = atoi(tmp);
		
	tmp = SPI_getvalue(tup, tupdesc, Anum_stadistinct);
	if (SPI_result != 0)
		goto fail;
		
	// stadistinct can be negative, zero, or positive.
	// So, we really shouldn't average it, given that it might be negative on some nodes, positive on others
	if (tmp)
		ctx->stats->stadistinct = atof(tmp);
		
	ctx->stats->stats_valid = true;


	elog(DEBUG3, "cdbanalyze oid=%d attnum=%d nullfrac=%f width=%d distinct %f",
		 ctx->relid, ctx->stats->attr->attnum, ctx->stats->stanullfrac, ctx->stats->stawidth, ctx->stats->stadistinct);

	return;

fail:
	ereport(WARNING, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("Unable to obtain column statistics from segment databases."),
					  errdetail("Segment db catalog query failed: %s  %d",
								sqlstmt->data, SPI_result )));
}	/* cdbanalyze_callback */


void
cdbanalyze_get_columnstats(Relation rel, VacAttrStats * stats)
{
	StringInfoData buffer;
	Cdbanalyze_columns_Ctx ctx;
	char	   *relname;
	Oid			namespace;
	
	Assert(stats != NULL);
	
	if (stats == NULL)
		return;
	
	ctx.stats = stats;

	relname = RelationGetRelationName(rel);
	namespace = RelationGetNamespace(rel);

	/*
	 * First, create a select statement
	 */
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "select staattnum,avg(stanullfrac),avg(stawidth),avg(stadistinct)");
	appendStringInfo(&buffer, " from pg_statistic JOIN pg_class ON relfilenode=starelid ");
	appendStringInfo(&buffer, " where relname='%s' and relnamespace=%u and staattnum=%hd", relname, namespace, (stats->attr->attnum));
	appendStringInfo(&buffer, " group by staattnum order by staattnum");

	/*
	 * Next, use SPI to execute it.
	 * (If unsuccessful, throws an error and does not return here.)
	 */
	ctx.relid = RelationGetRelid(rel);

	cdbanalyze_query_local_catalogs(&buffer, cdbanalyze_columns_callback, &ctx);
	

}	/* cdbanalyze_get_columnstats */

