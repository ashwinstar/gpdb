/*-------------------------------------------------------------------------
 *
 * cdbagg.h
 *	  definitions for cdbagg.c and users of same
 *
 * Copyright (c) 2004, Metapa
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	 
 *-------------------------------------------------------------------------
 */

#ifndef CDBAGG_H
#define CDBAGG_H

#include "nodes/primnodes.h"
#include "nodes/plannodes.h"

/*
 * A CdbAggStrategy is the composite result of function getAggStrategyList.
 * It characterizes a way of transforming an Agg node (or related node) and 
 * its argument in order to produce a correct result in the CDB parallel 
 * execution environment.
 */
typedef struct CdbAggStrategy
{
	Movement leftmove;
	FlowType resultflowtype;
	bool resultislocal;
	
	List *hashexprs;
	List *hashtypes;

} CdbAggStrategy;

extern List *getAggStrategyList(Agg *agg);
extern CdbAggStrategy *copyAggStrategy(CdbAggStrategy *old);
extern void freeAggStrategyList(List* aslist);

#endif /* CDBAGG_H */