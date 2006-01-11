/*-------------------------------------------------------------------------
 *
 * cdbagg.h
 *	  definitions for cdbagg.c and users of same
 *
 * Copyright (c) 2004-2006, Greenplum
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
#include "cdb/cdbmutate.h"		/* Need ApplyMotionState */


/*
 * Declarations of functions defined in cdbagg.c
 */
bool cdbagg_prescan(Agg *agg);


/*
 * Structures: CdbAggTactic, CdbAggPartial, CdbAggFinal
 *
 * These structures work together to define a tactic for computing an
 * aggregate function value across a partitioned set.  The are used by
 * the restructing strategy to restructure the Aggref nodes within an Agg
 * node.
 *
 * CdbAggTactic is the top-level structure.  It contains palloc'd arrays
 * of the other structures and corresponds to the consolidation of the
 * results of final aggregations into a single result.
 *
 * CdbAggPartial corresponds to a partial aggregation in which a subset
 * of the set being aggregated is reduced an intermediate result.
 *
 * CdbAggFinal corresponds to a final aggregation of partial aggregations
 * in which the intermediate results from a set of CbdAggPartials are
 * combined into a set of intermediate results ready for consolidate
 * specified in the controlling CdbAggTactic.
 *
 * The explicit relationships among them are fairly straight-forward:
 * - numFinals and numPartials are the lengths of palloc'd arrays finals
 *	 and partials, respectively, and
 * - argindex values must be valid C indexes into the partials array.
 *
 * The implicit relationships are more subtle in that they must honor
 * the datatypes of arguments and results.	The tactic will be carried
 * out blindly, so the composer of the tactic must do all neccessary
 * checking!
 */

typedef struct
CdbAggPartial
{
	Oid			aggfnoid;		/* The Oid of the partial aggregation
								 * function. */
	Oid			rettype;		/* The Oid of the return type of the partial
								 * aggregration function. */
	Oid			argfnoid;		/* The Oid of a scalar function to apply to
								 * the argument of the original aggregate
								 * function to produce the argument to the
								 * partial, or InvalidOid, if argument
								 * transformation is not neccessary. */
	Oid			argtype;		/* The Oid of of the argument type expected by
								 * the partial aggregate function.	This is
								 * also the return type of the argument
								 * function, if one is specified.  If no
								 * argument function is specified, this field
								 * may be InvalidOid. */

	/*
	 * From here on, fields used for transient storage.  If you didn't set it,
	 * don't use it and don't count on it surviving calls to other routines.
	 */

	Var		   *var;			/* Working storage for the code that
								 * implements the stategy.	This will hold the
								 * Var that by which the outer query may refer
								 * to this partial aggregation. */

} CdbAggPartial;

typedef struct
CdbAggFinal
{
	Oid			aggfnoid;		/* The Oid of the final aggregration function. */
	Oid			rettype;		/* The Oid of the return type of the final
								 * aggregation function. */
	Oid			argfnoid;		/* The Oid of a scalar function to apply to
								 * the result of the partial aggregate
								 * function specified by argindex in order to
								 * produce the argument to the final, or
								 * InvalidOid, if argument transformation is
								 * not neccessary. */
	Oid			argtype;		/* The Oid of of the argument type expected by
								 * the final aggregate function.  This is also
								 * the return type of the argument function,
								 * if one is specified.  If no argument
								 * function is specified, this field must be
								 * InvalidOid or must match the rettype of the
								 * partial that supplies the argument. */
	int			argindex;		/* The index into the CdbAggTactic's partial
								 * array of the CdbAggPartial that supplies
								 * the argument to this final. */

	/*
	 * From here on, fields used for transient storage.  If you didn't set it,
	 * don't use it and don't count on it surviving calls to other routines.
	 */

	Expr	   *expr;			/* Working storage for the code that
								 * implements the stategy.	This will hold the
								 * Expr that provides the corresponding
								 * argument to the tactic's consolidator. */
} CdbAggFinal;

typedef struct
CdbAggTactic
{
	Oid			consolidator;	/* The Oid of a consolidation function with
								 * numFinals arguments or, in case there is a
								 * single final aggregation, InvalidOid to
								 * indicate that its result may be used
								 * directly. */
	int			numFinals;		/* The length of array finals; also the number
								 * of arguments to the consolidator. */
	CdbAggFinal *finals;		/* A palloc'd array of final aggregation
								 * instructions. */
	int			numPartials;	/* The length of the array partials; also the
								 * number of inner query target entries that
								 * will be generated; also the limit on the
								 * values of argindex in an element of finals. */
	CdbAggPartial *partials;	/* A palloc'd array of partial aggregration
								 * instructions. */
} CdbAggTactic;


/*
 * Declarations of functions defined in cdbaggtactic.c
 */
extern CdbAggTactic *getAggTactic(Aggref *aggref, char **msg);
extern void freeAggTactic(CdbAggTactic *tactic);


#endif   /* CDBAGG_H */
