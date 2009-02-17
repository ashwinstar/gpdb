/*-------------------------------------------------------------------------
 *
 * cdbanalyze.h
 *	  Provides routines for performing distributed analyze cdb
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBANALYZE_H
#define CDBANALYZE_H

#include "commands/vacuum.h"
#include "cdb/cdbdispatchresult.h" /* CdbDispatchResults */

/*
 * cdbanalyze_get_relstats:
 * This takes the oid of a relation as a parameter, and returns
 * aggregate numbers for the number of pages and number of tuples in out parameters.
 * This must only be called from the dispatcher.
 */
extern void cdbanalyze_get_relstats(Relation rel, BlockNumber *pages, double *tuples);

extern void cdbanalyze_get_columnstats(VPgStatisticStats *stats,
									   TupleDesc tupdesc,
									   double totalrows,
									   MemoryContext ctx,
									   Relation onerel);
/*
 * cdbanalyze_combine_stats
 * This function combine the stats information sent by QEs to generate
 * the final stats for QD relations.
 *
 * This must be called by the dispatcher.
 */
extern void cdbanalyze_combine_stats(CdbDispatchResults *primaryResults,
									 CdbDispatchResults *mirrorResults,
									 void *ctx);

#endif   /* CDBANALYZE_H */
