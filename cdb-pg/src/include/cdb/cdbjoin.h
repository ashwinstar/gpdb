/*-------------------------------------------------------------------------
 *
 * cdbjoin.h
 *	  definitions for cdbjoin.c and users of same
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

#ifndef CDBJOIN_H
#define CDBJOIN_H

#include "nodes/primnodes.h"
#include "nodes/plannodes.h"

/*
 * A CdbJoinStrategy is the composite result of function getJoinStrategyList.
 * It characterizes a way of transforming a Join node and its arguments
 * in order to produce a correct result in the CDB parallel execution
 * environment.
 */
typedef struct CdbJoinStrategy
{
	Movement leftmove;
	Movement rightmove;
	FlowType resultflowtype;
	double est_ams_cost;

} CdbJoinStrategy;

extern List *getJoinStrategyList(Join *join);
extern CdbJoinStrategy *copyJoinStrategy(CdbJoinStrategy *old);
extern void freeJoinStrategyList(List* jslist);
extern bool extractHashExprFromQual( List *joinqual, List **pHashExpr, List **pHashOp );

#endif /* CDBJOIN_H */
