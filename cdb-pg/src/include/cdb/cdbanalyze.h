/*-------------------------------------------------------------------------
 *
 * cdbanalyze.h
 *	  Provides routines for performing distributed analyze cdb
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBANALYZE_H
#define CDBANALYZE_H

/*
 * cdbanalyze_get_relstats:
 * This takes the oid of a relation as a parameter, and returns
 * aggregate numbers for the number of pages and number of tuples in out parameters.
 * This must only be called from the dispatcher.
 */
extern void cdbanalyze_get_relstats(Oid relid, BlockNumber *pages, double *tuples);


#endif	/* CDBANALYZE_H */

