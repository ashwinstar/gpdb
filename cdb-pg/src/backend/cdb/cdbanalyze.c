/*-------------------------------------------------------------------------
 *
 * cdbanalyze.c
 *	  Provides routines for performing distributed analyze cdb
 *
 * Copyright (c) 2005-2008, Greenplum inc
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
#include "gp-libpq-fe.h"
#include "gp-libpq-int.h"

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
		ereport(NOTICE, (errcode(ERRCODE_GP_COMMAND_ERROR),
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
	ereport(WARNING, (errcode(ERRCODE_GP_COMMAND_ERROR),
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
	const char	*relname;
	const char *quoted_relname;
	Oid			namespace;
	const char *namespace_name;

	*pages = 0;
	*tuples = 0;

	relname = RelationGetRelationName(rel);
	namespace = RelationGetNamespace(rel);
	namespace_name = get_namespace_name(namespace);

	if (relname == NULL) 
	{
		elog(ERROR, "Unable to collect rel stats from segment databases. Relation probably does not exist.");
		return;
	}
 	 	
	if (namespace_name == NULL) 
	{
		elog(ERROR, "Unable to collect rel stats from segment databases. Schema probably does not exist.");
		return;
	}
	
	quoted_relname = quote_literal_internal(relname);
	
	/*
	 * First, create a select statement
	 */
	initStringInfo(&buffer);
	appendStringInfo(&buffer, "select max(reltuples), max(relpages)");
	appendStringInfo(&buffer, " from pg_class join pg_namespace on relnamespace=pg_namespace.oid");
	appendStringInfo(&buffer, " where relname=%s and nspname='%s'", quoted_relname, namespace_name);

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

extern bool get_attstatsslot_desc(TupleDesc tupdesc, HeapTuple statstuple,
				 Oid atttype, int32 atttypmod,
				 int reqkind, Oid reqop,
				 Datum **values, int *nvalues,
				 float4 **numbers, int *nnumbers);

/*
 * Compute pg_statistic for columns that are accumulated from all
 * QEs.
 */
void
cdbanalyze_get_columnstats(VPgStatisticStats *stats,
						   TupleDesc tupdesc,
						   MemoryContext ctx,
						   Relation onerel)
{
	ListCell *lc, *reltuples_lc;
	int 		tuple_no = 0;
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
	double 		tuples_this_segdb = 0.0;
	double      totalrows = 0.0;
	bool		special_stadistinct_handling = false;
	MemoryContext old_context;
	
	if (stats == NULL)
		return;
	Assert(stats->tuples != NULL);
	Assert(OidIsValid(stats->metadata.relid));

	old_context = MemoryContextSwitchTo(ctx);
	
	forboth (reltuples_lc, stats->reltuples, lc, stats->tuples)
	{
		HeapTuple tuple = (HeapTuple)lfirst(lc);
		Datum *att_values = palloc(sizeof(Datum) * tupdesc->natts);
		bool *isnull = palloc(sizeof(bool) * tupdesc->natts);

		tuples_this_segdb = *((double*)lfirst(reltuples_lc));
		totalrows += tuples_this_segdb;

		heap_deform_tuple(tuple, tupdesc, att_values, isnull);

		if (isnull[Anum_pg_statistic_stanullfrac - 1])
			ereport(NOTICE, (errcode(ERRCODE_GP_COMMAND_ERROR),
				   errmsg("NULL returned instead of column statistics from segment databases.")));
		else			
			nullfrac +=  DatumGetFloat4(att_values[Anum_pg_statistic_stanullfrac - 1]);
			
		// Average width of the column (width of pointer if toasted).
			
		if (!isnull[Anum_pg_statistic_stawidth - 1])
			avgwidth +=  (int)DatumGetInt16(att_values[Anum_pg_statistic_stawidth - 1]);
			
		/*
		 *  stadistinct can be negative, zero, or positive.
		 * So, we really shouldn't just average it, given that it might be negative on some nodes, positive on others
		 */
			
		if (!isnull[Anum_pg_statistic_stadistinct - 1])
		{
			Datum dat = att_values[Anum_pg_statistic_stadistinct - 1];
			
			if ((DatumGetFloat4(dat) > 0 && distinct < 0) ||
				(DatumGetFloat4(dat) < 0 && distinct > 0))
				{
					special_stadistinct_handling = true;
					if (distinct < 0.0)
					{
						// Turn back into # of disinct values, rather than percent
						distinct  = -(distinct * (totalrows/(float)(tuple_no + 1)));
					}
				}
			
			if (special_stadistinct_handling && DatumGetFloat4(dat) < 0)
			{
				if (tuples_this_segdb == 0)
					tuples_this_segdb = 1;
				distinct += -(DatumGetFloat4(dat) * tuples_this_segdb);
			}
			else
				distinct +=  DatumGetFloat4(dat);
		}

		tuple_no++;

		pfree(att_values);
		pfree(isnull);
		
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
			bool found;
			Datum	   *values;
			int			nvalues;
			float4	   *numbers;
			int			nnumbers;
			Form_pg_attribute attr = onerel->rd_att->attrs[stats->attno - 1];
			
			found = get_attstatsslot_desc(tupdesc, tuple,
				 attr->atttypid, attr->atttypmod,
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
			
			found = get_attstatsslot_desc(tupdesc, tuple,
				 attr->atttypid, attr->atttypmod,
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
			
			found = get_attstatsslot_desc(tupdesc, tuple,
				 attr->atttypid, attr->atttypmod,
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
			
		}

	}
	
	/*
	 * Now fill the statistics into the stats structure.
	 */
	
	correlation /= (float)correlationcount;
	
	totalrows /= (float)list_length(stats->tuples);		// actually, the average per segDB
	
	stats->pgstat.stanullfrac = nullfrac/(float)list_length(stats->tuples);
	
	stats->pgstat.stawidth = (short)(avgwidth/list_length(stats->tuples));
	
	stats->pgstat.stadistinct = distinct/(float)list_length(stats->tuples);
	
	/*
	 * If we estimated the number of distinct values at more than 10% of
	 * the total row count (a very arbitrary limit), then assume that
	 * stadistinct should scale with the row count rather than be a fixed
	 * value.
	 */
	if (special_stadistinct_handling && totalrows > 0.0)
	if (stats->pgstat.stadistinct > 0.1 * totalrows)
		stats->pgstat.stadistinct = -(stats->pgstat.stadistinct / totalrows);
	
	/*
	 * Fill in the slots with info gathered above.  We can't do this at the same
	 * time as we fetch rows from the QEs, because not all QE have the same slots filled in with the same
	 * statistic type.  For example, some nodes might have a MCV slot, other nodes might not.
	 */
	
	{
		int slot = 0;
		
		if (mcv_nvalues > 0)
		{
			
			stats->pgstat.stakind[slot] = STATISTIC_KIND_MCV;
			
			stats->pgstat.numvalues[slot]=mcv_nvalues;
			stats->pgstat.numnumbers[slot]=mcv_nvalues;
			stats->pgstat.stavalues[slot] = mcv_values;
			stats->pgstat.stanumbers[slot] = mcv_numbers;
				
			slot++;
		}
		
		
		if (histogram_nvalues > 0)
		{

			stats->pgstat.stakind[slot] = STATISTIC_KIND_HISTOGRAM;
			stats->pgstat.numvalues[slot] = histogram_nvalues;
			stats->pgstat.stavalues[slot] = histogram_values;
			stats->pgstat.numnumbers[slot] = 0;
			
			slot++;
		}
		
		if (correlationcount > 0)
		{

			stats->pgstat.stakind[slot] = STATISTIC_KIND_CORRELATION;
			stats->pgstat.numnumbers[slot]=1;
			stats->pgstat.stanumbers[slot]=palloc(sizeof(float));
			stats->pgstat.stanumbers[slot][0] = correlation;
			stats->pgstat.numvalues[slot]=0;
			
			slot++;
		}
	}
	
	
	elog(DEBUG3, "Final cdbanalyze oid=%d attnum=%d nullfrac=%f width=%d distinct %f correlation %f",
		 stats->metadata.relid, onerel->rd_att->attrs[stats->attno - 1]->attnum,
		 stats->pgstat.stanullfrac, stats->pgstat.stawidth,
		 stats->pgstat.stadistinct, correlation);
	
	stats->pgstat.stats_valid = true;
	
	MemoryContextSwitchTo(old_context);
}	/* cdbanalyze_get_columnstats */

/*
 * cdbanalyze_combine_stats
 * This function combine the stats information sent by QEs to generate
 * the final stats for QD relations.
 *
 * Note that the mirrorResults is ignored by this function.
 */
void
cdbanalyze_combine_stats(CdbDispatchResults *primaryResults,
						 CdbDispatchResults *mirrorResults,
						 void *ctx)
{
	int result_no;
	VacuumStatsContext *stats_context = (VacuumStatsContext *)ctx;

	/*
	 * Store stats info from QEs, one for each relation. Each entry
	 * contains an array of VPgStatisticStats, one for each attribute.
	 */
	List *pgstat_stats_list = NIL;
	List *pgstat_stats_max_attnum = NIL;
		
	Assert(Gp_role == GP_ROLE_DISPATCH);

	if (primaryResults == NULL)
		return;

	/*
	 * Process the dispatch results from the primary. Note that the QE
	 * processes also send back the new stats info, such as stats on
	 * pg_class and/or pg_statistic, for the relevant table and its
	 * indexes. We parse this information, and compute the final stats
	 * for the QD.
	 *
	 * For pg_class stats, we compute the maximum number of tuples and
	 * maximum number of pages after processing the stats from each QE.
	 *
	 * For pg_statistic stats, we group them first based on the attribute,
	 * and then compute the final stats for each attribute.
	 */
	for(result_no = 0; result_no < primaryResults->resultCount; result_no++)
	{
		CdbDispatchResult *result = &(primaryResults->resultArray[result_no]);
		int num_pgresults = cdbdisp_numPGresult(result);
		int pgresult_no;
		VPgClassStats *pgclass_stats = NULL;
		
		for (pgresult_no = 0; pgresult_no < num_pgresults; pgresult_no++)
		{
			struct pg_result *pgresult = cdbdisp_getPGresult(result, pgresult_no);
			int type;
			
			if (pgresult->extras == NULL)
				continue;
			
			Assert(pgresult->extraslen > sizeof(int));
			
			/* Check for the type of stats that are received */
			memcpy(&type, pgresult->extras, sizeof(int));
			Assert(type == PG_CLASS_STATS || type == PG_STATISTIC_STATS);
			
			/*
			 * Process the stats for pg_class. We simple compute the maximum
			 * number of rel_tuples and rel_pages.
			 */
			if (type == PG_CLASS_STATS)
			{
				ListCell *lc = NULL;
				pgclass_stats = (VPgClassStats *)pgresult->extras;
				foreach (lc, stats_context->updated_stats)
				{
					VUpdatedStats *tmp_stats = (VUpdatedStats *)lfirst(lc);
					if (tmp_stats->relid == pgclass_stats->metadata.relid &&
						tmp_stats->type == PG_CLASS_STATS)
					{
						VPgClassStats *tmp_pgclass_stats = (VPgClassStats *)tmp_stats;
						
						tmp_pgclass_stats->rel_pages =
							Max(tmp_pgclass_stats->rel_pages, pgclass_stats->rel_pages);
						tmp_pgclass_stats->rel_tuples =
							Max(tmp_pgclass_stats->rel_tuples, pgclass_stats->rel_tuples);
						
						break;
					}
				}

				if (lc == NULL)
				{
					Assert(pgresult->extraslen == sizeof(VPgClassStats));

					pgclass_stats = palloc(sizeof(VPgClassStats));
					memcpy(pgclass_stats, pgresult->extras, pgresult->extraslen);

					stats_context->updated_stats =
						lappend(stats_context->updated_stats, pgclass_stats);
				}
			}
				
			else if (type == PG_STATISTIC_STATS)
			{
				VUpdatedStats *new_stats = (VUpdatedStats *)pgresult->extras;
				double *reltuples;
				int *new_max_attnum;
				int16 new_attno;
				int *old_max_attnum = NULL;
				ListCell *lc = NULL;
				ListCell *max_attnum_lc = NULL;
				int len = sizeof(VUpdatedStats);
				VPgStatisticStats **pgstat_stats_array = NULL;

				reltuples = (double *)((char *)new_stats + len);
				len += sizeof(double);
				
				new_max_attnum = (int *)((char*)new_stats + len);
				len += sizeof(int);
				
				memcpy(&new_attno, (char *)new_stats + len, sizeof(int16));
				len += sizeof(int);
				
				forboth (lc, pgstat_stats_list, max_attnum_lc, pgstat_stats_max_attnum)
				{
					int attno = 0;
					
					old_max_attnum = (int *)lfirst(max_attnum_lc);
					pgstat_stats_array = (VPgStatisticStats **)lfirst(lc);
					
					/* Find the first non-null pgstat_stats_array */
					for(attno = 0; attno < *old_max_attnum; attno++)
					{
						if (pgstat_stats_array[attno] != NULL)
							break;
					}

					Assert(attno < *old_max_attnum);

					if (attno < *old_max_attnum &&
						pgstat_stats_array[attno]->metadata.relid == new_stats->relid)
						break;
				}
				
				if (lc != NULL)
				{
					Assert(*new_max_attnum == *old_max_attnum);
				}
				else
				{
					pgstat_stats_array = palloc0(*new_max_attnum * sizeof(VPgStatisticStats *));
					pgstat_stats_list = lappend(pgstat_stats_list, pgstat_stats_array);
					pgstat_stats_max_attnum = lappend(pgstat_stats_max_attnum, new_max_attnum);
					Assert(new_attno <= *new_max_attnum);
				}

				if (pgstat_stats_array[new_attno - 1] != NULL)
				{
					HeapTuple tuple;
					VPgStatisticStats *tmp_pgstat_stats = pgstat_stats_array[new_attno - 1];
					Assert(tmp_pgstat_stats->metadata.relid == new_stats->relid);
					
					tuple = (HeapTuple)(((char *)new_stats) + len);
					/* Reset tuple->t_data */
					tuple->t_data = (HeapTupleHeader)(((char *)tuple) + HEAPTUPLESIZE);
					
					tmp_pgstat_stats->tuples =
						lappend(tmp_pgstat_stats->tuples, tuple);
					tmp_pgstat_stats->reltuples =
						lappend(tmp_pgstat_stats->reltuples, reltuples);
				}

				else
				{
					HeapTuple tuple;
					VPgStatisticStats *pgstat_stats;

					Assert(pgresult->extraslen > sizeof(VUpdatedStats));
					pgstat_stats = (VPgStatisticStats *)palloc0(sizeof(VPgStatisticStats));
					pgstat_stats->attno = new_attno;

					tuple = (HeapTuple)((char *)new_stats + len);
					Assert(pgresult->extraslen == len + heaptuple_get_size(tuple));

					/* Reset tuple->t_data */
					tuple->t_data = (HeapTupleHeader)(((char *)tuple) + HEAPTUPLESIZE);

					pgstat_stats->metadata = *(new_stats);
					pgstat_stats->tuples = NIL;
					pgstat_stats->reltuples = NIL;
					pgstat_stats->tuples = lappend(pgstat_stats->tuples, tuple);
					pgstat_stats->reltuples = lappend(pgstat_stats->reltuples, reltuples);

					pgstat_stats_array[new_attno - 1] = pgstat_stats;
				}
			}
		}
	}

	if (list_length(pgstat_stats_list) > 0)
	{
		ListCell *lc;
		ListCell *max_attnum_lc;
		
		Relation stat_rel = heap_open(StatisticRelationId, RowExclusiveLock);
		TupleDesc tupdesc = RelationGetDescr(stat_rel);
		
		/* Compute the final stats for each relation. */
		forboth (lc, pgstat_stats_list, max_attnum_lc, pgstat_stats_max_attnum)
		{
			VPgStatisticStats **pgstat_stats_array = (VPgStatisticStats **)lfirst(lc);
			int *max_attnum = (int *)lfirst(max_attnum_lc);
			int attno;

			for(attno = 0; attno < *max_attnum; attno++)
			{
				VPgStatisticStats *tmp_stats = pgstat_stats_array[attno];
				Relation rel;

				if (tmp_stats == NULL)
					continue;

				/*
				 * If this relation is the one stored in stats_context, use it directly.
				 * Otherwise, we have to open it.
				 */
				if (RelationGetRelid(stats_context->onerel) == tmp_stats->metadata.relid)
				{
					rel = stats_context->onerel;
				}

				else
				{
					rel = relation_open(tmp_stats->metadata.relid, AccessShareLock);
				}
			
				cdbanalyze_get_columnstats(tmp_stats, tupdesc,
										   stats_context->ctx, rel);
				/* We could safely free the space for tuples and reltuples */
				list_free(tmp_stats->tuples);
				list_free(tmp_stats->reltuples);
				tmp_stats->tuples = NIL;
				tmp_stats->reltuples = NIL;
				
				stats_context->updated_stats =
					lappend(stats_context->updated_stats, tmp_stats);

				if (rel != stats_context->onerel)
					relation_close(rel, AccessShareLock);
			}
		}

		heap_close(stat_rel, RowExclusiveLock);

		list_free_deep(pgstat_stats_list);
		list_free(pgstat_stats_max_attnum);
	}
}
