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
#include "cdb/cdbvars.h"		/* Gp_role */
#include "cdb/cdbanalyze.h"		/* me */
#include "catalog/pg_statistic.h"
#include "utils/lsyscache.h"
#include "fmgr.h"

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
		   Gp_role == GP_ROLE_DISPATCH);

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

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_tuples);
	if (SPI_result != 0 && !tmp)
		goto fail;
		
	/*
	 * CTAS can cause this to happen, because the QD creates it's toast index before the QEs have had time
	 * to initialize their tables, but otherwise we shouldn't see null returned.
	 */
	if (!tmp && strcmp(sqlstmt->data,"pg_toast")==0)
		ereport(NOTICE, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("NULL returned instead of statistics from segment databases."),
					  errdetail("Segment db catalog query : %s",
								sqlstmt->data )));
	if (tmp)
		ctx->tuples = atof(tmp);
	else
		ctx->tuples = 2000;			// Work around aggregate bug
		
	if (ctx->tuples == 0)
		ctx->tuples = 1;			// So we know analyze has been run.

	tmp = SPI_getvalue(tup, tupdesc, Anum_pg_class_max_pages);
	if (SPI_result != 0 && !tmp)
		goto fail;
	if (tmp)
		ctx->pages = atoi(tmp);
	else
		ctx->pages = 50;			// Work around aggregate bug
		
	if (ctx->pages == 0)
		ctx->pages = 1;				// So we know analyze has been run.

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
	appendStringInfo(&buffer, " from pg_class join pg_namespace on relnamespace=pg_namespace.oid");
	appendStringInfo(&buffer, " where relname='%s' and nspname='%s'", relname, get_namespace_name(namespace));

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

	bool		isnull;
	Datum		dat;
	int 		i;
	
	float		nullfrac = 0.0;
	float		distinct = 0.0;
	int			avgwidth = 0;
	float		correlation = 0.0;
	int			correlationcount = 0;
	Datum		*mcv_values = 0;
	float		*mcv_numbers = 0;
	int			mcv_nvalues = 0;
	Datum		*histogram_values = 0;
	int			histogram_nvalues = 0;
	float 		tuples_this_segdb = 0.0;
	float		totalrows = 0.0;
	bool		special_stadistinct_handling = false;
	
	
	Cdbanalyze_columns_Ctx *ctx = (Cdbanalyze_columns_Ctx *) context;
	
	
	Assert(ctx != NULL);
	Assert(ctx->stats != NULL);

	/* Did the query succeed? */
	if (spiresult != SPI_OK_SELECT)
		goto fail;
		
	if (SPI_processed < 1)
		return;		/* no columns stats exist on QEs (empty table?) */


	/* Grab the results of the query and stuff them into the context. */
	tupdesc = SPI_tuptable->tupdesc;
	if (!tupdesc)
		goto fail;
		
		
	for (i=0;i<SPI_processed;i++)
	{
		tup = SPI_tuptable->vals[i];
		if (!tup)
			goto fail;
	

	
		dat = SPI_getbinval(tup, tupdesc, Anum_pg_statistic_stanullfrac, &isnull);
		if (SPI_result != 0)
			goto fail;
			
		if (isnull)
			ereport(NOTICE, (errcode(ERRCODE_MPP_COMMAND_ERROR),
				   errmsg("NULL returned instead of column statistics from segment databases."),
						  errdetail("Segment db catalog query : %s",
									sqlstmt->data )));
			
		if (!isnull)
			nullfrac +=  DatumGetFloat4(dat);
			
		
		dat = SPI_getbinval(tup, tupdesc, Anum_pg_statistic_stavalues4 + 1, &isnull);
		if (SPI_result != 0)
			goto fail;
			
		tuples_this_segdb = DatumGetFloat4(dat);
		totalrows += tuples_this_segdb;
			 
		dat = SPI_getbinval(tup, tupdesc, Anum_pg_statistic_stawidth, &isnull);
		if (SPI_result != 0)
			goto fail;
			
		// Average width of the column (width of pointer if toasted).
			
		if (!isnull)
			avgwidth +=  (int)DatumGetInt16(dat);
			
			
		dat = SPI_getbinval(tup, tupdesc, Anum_pg_statistic_stadistinct, &isnull);
		if (SPI_result != 0)
			goto fail;
		
			
		/*
		 *  stadistinct can be negative, zero, or positive.
		 * So, we really shouldn't just average it, given that it might be negative on some nodes, positive on others
		 */
			
		if (!isnull)
		{
			if ((DatumGetFloat4(dat) > 0 && distinct < 0) ||
				(DatumGetFloat4(dat) < 0 && distinct > 0))
				{
					special_stadistinct_handling = true;
					if (distinct < 0.0)
					{
						// Turn back into # of disinct values, rather than percent
						distinct  = -(distinct * (totalrows/(float)(i+1)));
					}
				}
			
			if (special_stadistinct_handling && DatumGetFloat4(dat) < 0)
			{
				if (tuples_this_segdb == 0)
					tuples_this_segdb = 1;
				distinct += -(distinct * tuples_this_segdb);
			}
			else
				distinct +=  DatumGetFloat4(dat);
		}
		
		/*
		 * Now get the statistic "slots" from the record.  One big issue is: What's a good way to combine the
		 * values from different segDBs?  There doesn't seem to be any ideal solution.
		 * 
		 * On the good side, if the table is large, and the column is NOT part of the distibution columns,
		 * each segDB should return approximately similar results (unless the column has a hidden correlation
		 * with the distribution columns).  So for many case, picking one result arbitrarily works great.
		 * 
		 * And, for unique columns, there is no MCV, so if you have a single unique column that's your 
		 * distribution column (very common primary key case), there isn't any MCV to worry about merging.
		 * 
		 */
		
		{
			MemoryContext old_context;
			bool found;
			Datum	   *values;
			int			nvalues;
			float4	   *numbers;
			int			nnumbers;
			
			old_context = MemoryContextSwitchTo(ctx->stats->anl_context);
			
			found = get_attstatsslot(tup,
				 ctx->stats->attr->atttypid, ctx->stats->attr->atttypmod,
				 STATISTIC_KIND_MCV, InvalidOid,
				 &values, &nvalues,
				 &numbers, &nnumbers);
			if (found)
			{
				Assert(nnumbers == nvalues);
				
				/*
				 * Rather than just replace any MCV values we have already, we 
				 * should try to merge the lists, keeping the most common cases, and
				 * adding together duplicate values.
				 * 
				 * This is a bit complicated, since the "numbers" are percentages,
				 * and you can't just add the percentages or average them.  You need
				 * to convert them back to approximate row counts, by multiplying them
				 * with the number of tuples on that QE, then you can add them together,
				 * then convert back to percentages.
				 * 
				 * 
				 * Right now, we just pick one at random (the last one).
				 */
				mcv_nvalues = nvalues;
				mcv_values = values;
				mcv_numbers = numbers;

			}
			
			found = get_attstatsslot(tup,
				 ctx->stats->attr->atttypid, ctx->stats->attr->atttypmod,
				 STATISTIC_KIND_HISTOGRAM, InvalidOid,
				 &values, &nvalues,
				 NULL, NULL);
			if (found)
			{
				/*
				 * How to merge histograms?  I don't know.  Pick one at random.
				 */
				histogram_nvalues = nvalues;
				histogram_values = values;

			}
			
			found = get_attstatsslot(tup,
				 ctx->stats->attr->atttypid, ctx->stats->attr->atttypmod,
				 STATISTIC_KIND_CORRELATION, InvalidOid,
				 NULL, NULL,
				 &numbers, &nnumbers);
			if (found)
			{
				/*
				 * Let's average the correlation across nodes.  This usually gives a good value,
				 */
				correlation += numbers[0];
				correlationcount++;
			}
			
			MemoryContextSwitchTo(old_context);
		
		}

	}
	
	/*
	 * Now fill the statistics into the stats structure.
	 */
	
	correlation /= (float)correlationcount;
	
	totalrows /= (float)SPI_processed;		// actually, the average per segDB
	
	ctx->stats->stanullfrac = nullfrac/(float)SPI_processed;
	
	ctx->stats->stawidth = (short)(avgwidth/SPI_processed);
	
	ctx->stats->stadistinct = distinct/(float)SPI_processed;
	
	/*
	 * If we estimated the number of distinct values at more than 10% of
	 * the total row count (a very arbitrary limit), then assume that
	 * stadistinct should scale with the row count rather than be a fixed
	 * value.
	 */
	if (special_stadistinct_handling && totalrows > 0.0)
	if (ctx->stats->stadistinct > 0.1 * totalrows)
		ctx->stats->stadistinct = -(ctx->stats->stadistinct / totalrows);
	
	/*
	 * Fill in the slots with info gathered above.  We can't do this at the same
	 * time as we fetch rows from the QEs, because not all QE have the same slots filled in with the same
	 * statistic type.  For example, some nodes might have a MCV slot, other nodes might not.
	 */
	
	{
		MemoryContext old_context;

		int slot = 0;
		
		old_context = MemoryContextSwitchTo(ctx->stats->anl_context);
		
		if (mcv_nvalues > 0)
		{
			
			ctx->stats->stakind[slot] = STATISTIC_KIND_MCV;
			
			ctx->stats->numvalues[slot]=mcv_nvalues;
			ctx->stats->numnumbers[slot]=mcv_nvalues;
			ctx->stats->stavalues[slot] = mcv_values;
			ctx->stats->stanumbers[slot] = mcv_numbers;
				
			slot++;
		}
		
		
		if (histogram_nvalues > 0)
		{

			ctx->stats->stakind[slot] = STATISTIC_KIND_HISTOGRAM;
			ctx->stats->numvalues[slot] = histogram_nvalues;
			ctx->stats->stavalues[slot] = histogram_values;
			ctx->stats->numnumbers[slot] = 0;
			
			slot++;
		}
		
		if (correlationcount > 0)
		{

			ctx->stats->stakind[slot] = STATISTIC_KIND_CORRELATION;
			ctx->stats->numnumbers[slot]=1;
			ctx->stats->stanumbers[slot]=palloc(sizeof(float));
			ctx->stats->stanumbers[slot][0] = correlation;
			ctx->stats->numvalues[slot]=0;
			
			slot++;
		}
		
		MemoryContextSwitchTo(old_context);
		
	}
	
	
	elog(DEBUG3, "Final cdbanalyze oid=%d attnum=%d nullfrac=%f width=%d distinct %f correlation %f",
		 ctx->relid, ctx->stats->attr->attnum, ctx->stats->stanullfrac, ctx->stats->stawidth, ctx->stats->stadistinct, correlation);
	
	ctx->stats->stats_valid = true;
	
	return;

fail:
	ereport(WARNING, (errcode(ERRCODE_MPP_COMMAND_ERROR),
			   errmsg("Unable to obtain column statistics from segment databases."),
					  errdetail("Segment db catalog query failed: %s  %d   %d",
								sqlstmt->data, spiresult, SPI_result )));
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
	appendStringInfo(&buffer, "select starelid,staattnum,stanullfrac,stawidth,stadistinct,");
	appendStringInfo(&buffer, "stakind1,stakind2,stakind3,stakind4, staop1,staop2,staop3,staop4,");
	appendStringInfo(&buffer, "stanumbers1,stanumbers2,stanumbers3,stanumbers4 ");
	appendStringInfo(&buffer, ",stavalues1, stavalues2, stavalues3,stavalues4 ");
	appendStringInfo(&buffer, ",reltuples ");

	appendStringInfo(&buffer, " from pg_statistic JOIN pg_class ON pg_class.oid=pg_statistic.starelid ");
	appendStringInfo(&buffer, " JOIN pg_namespace ON relnamespace=pg_namespace.oid ");
	appendStringInfo(&buffer, " where relname='%s' and nspname='%s' and staattnum=%hd", relname, get_namespace_name(namespace), (stats->attr->attnum));

	

	/*
	 * Next, use SPI to execute it.
	 * (If unsuccessful, throws an error and does not return here.)
	 */
	ctx.relid = RelationGetRelid(rel);

	cdbanalyze_query_local_catalogs(&buffer, cdbanalyze_columns_callback, &ctx);
	

}	/* cdbanalyze_get_columnstats */

