/*-------------------------------------------------------------------------
*
* cdbaggtactic.c
*
*	  Generate a tactic for implementing an aggregrate function by
*	  partial aggregation on each part of a partition of the input tuples
*	  followed by final aggregation and consolidation of the partial
*	  results.
*
* Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
* Portions Copyright (c) 2005, Greenplum Inc
*
*
* IDENTIFICATION
*	  $Header$
*
*-------------------------------------------------------------------------
*/
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"	/* Primitive data type OID constants. */
#include "parser/parse_expr.h"	/* exprType() and exprTypmod(). */

#include "cdb/cdbagg.h"

/* Forward declarations */
static CdbAggTactic *makeCdbAggTactic(int npartials, int nfinals);


/*
 * Function: makeCdbAggTactic
 *
 * Helper for function xformAggregate
 *
 * Allocates a "blank" aggregation tactic with the specified number of
 * partial and final aggregation slots.  The tactic must by filled in
 * before use.
 */
CdbAggTactic *
makeCdbAggTactic(int npartials, int nfinals)
{
	CdbAggTactic *tactic;
	CdbAggPartial *partial;
	CdbAggFinal *final;
	int			i;

	Assert(npartials > 0 && nfinals > 0);

	tactic = (CdbAggTactic *) (palloc(sizeof(CdbAggTactic)));

	tactic->consolidator = InvalidOid;

	tactic->numFinals = nfinals;
	tactic->finals = (CdbAggFinal *) (palloc(nfinals * sizeof(CdbAggFinal)));
	for (i = 0; i < nfinals; i++)
	{
		final = &tactic->finals[i];

		final->aggfnoid = InvalidOid;
		final->rettype = InvalidOid;
		final->argfnoid = InvalidOid;
		final->argtype = InvalidOid;
		final->argindex = 0;
	}

	tactic->numPartials = npartials;
	tactic->partials = (CdbAggPartial *) (palloc(npartials * sizeof(CdbAggPartial)));
	for (i = 0; i < npartials; i++)
	{
		partial = &tactic->partials[i];

		partial->aggfnoid = InvalidOid;
		partial->rettype = InvalidOid;
		partial->argfnoid = InvalidOid;
		partial->argtype = InvalidOid;
	}

	return tactic;
}

/*
 * Function: freeAggTactic
 *
 * Release the storage associated with a CdbAggTactic.
 */
void
freeAggTactic(CdbAggTactic * tactic)
{
	if (tactic == NULL)
		return;

	pfree(tactic->finals);
	pfree(tactic->partials);
	pfree(tactic);
}

/*
 * Function: getAggTactic
 *
 * Helper for function xformAggregate
 *
 * Obtains the tactic needed to implement the given aggregate function
 * call on a partitioned set.
 *
 * Eithers returns a pointer to a valid CdbAggTactic, or returns null
 * and sets an apppropriate errmsg in the context.
 *
 * The implementation is unfortunate.  It amounts to a table look-up and
 * should be driven by the PostgreSQL catalog plus some MPP extensions.
 * Instead, it depends on defined constants and various assumptions about
 * the contents of the PostgreSQL catalog.	These assumptions have been
 * validated against the following versions: 7.1, 7.2, 7.3, 8.0.
 *
 * TODO: Replace this approoach with a robust, catalog-driven technique.
 */
CdbAggTactic *
getAggTactic(Aggref *aggref, char **msg)
{
	char	   *nowhere;
	CdbAggTactic *tactic = NULL;

	if (msg == NULL)
		msg = &nowhere;
	*msg = NULL;

	/*
	 * These constants represent the OIDs of the PostgreSQL built-in
	 * aggregrate functions as of 8.0.	The following hack uses them
	 * and the fact that they are sequential to expedite support of
	 * aggregate functions.
	 */

#define CDB_AVG_OID_LO		2100
#define CDB_AVG_OID_HI		2106
#define CDB_SUM_OID_LO		2107
#define CDB_SUM_OID_HI		2114
#define CDB_MAX_OID_LO		2115
#define CDB_MAX_OID_HI		2130
#define CDB_MIN_OID_LO		2131
#define CDB_MIN_OID_HI		2146
#define CDB_COUNT_OID		2147
#define CDB_VARIANCE_OID_LO 2148
#define CDB_VARIANCE_OID_HI 2153
#define CDB_STDDEV_OID_LO	2154
#define CDB_STDDEV_OID_HI	2159

	/*
	 * These constants identify specific functions of interest in
	 * implementing aggregate functions.
	 */

	/* Sums */
#define CDB_SUM_OF_COUNT_OID		2107
#define CDB_SUM_INT8_TO_NUMERIC_OID 2107
#define CDB_SUM_NUMERIC_OID			2114
#define CDB_SUM_INT4_TO_INT8_OID	2108
#define CDB_SUM_INT2_TO_INT8_OID	2109
#define CDB_SUM_FLOAT4_OID			2110
#define CDB_SUM_FLOAT8_OID			2111
#define CDB_SUM_INTERVAL_OID		2113
	/* Divisions */
#define CDB_DIV_NUMERIC				1727
#define CDB_DIV_FLOAT8				217
#define CDB_DIV_INTERVAL			1326
	/* Casts */
#define CDB_NUMERIC_TO_INT8_OID		1779
#define CDB_INT8_TO_FLOAT8_OID		482
#define CDB_FLOAT4_TO_FLOAT8		311


	if (aggref->aggfnoid == CDB_COUNT_OID)
	{
		/* COUNT */
		tactic = makeCdbAggTactic(1, 1);

		tactic->partials[0].aggfnoid = aggref->aggfnoid;
		tactic->partials[0].rettype = INT8OID;	/* COUNT returns int8 */

		tactic->finals[0].aggfnoid = CDB_SUM_OF_COUNT_OID;		/* Takes int8 */
		tactic->finals[0].rettype = NUMERICOID; /* Returns numeric */

		tactic->consolidator = CDB_NUMERIC_TO_INT8_OID;
	}
	else if (CDB_SUM_OID_LO <= aggref->aggfnoid
			 && aggref->aggfnoid <= CDB_SUM_OID_HI)
	{
		/* SUM */

		/*
		 * TODO: We should use the aggfnoid to determine the argument type,
		 * but we go out on a limb by deducing the type from the argument.
		 * Improve this when we adopt a catalog-driven approach.
		 */
		Oid			argTypeOid = exprType((Node *) (aggref->target));

		tactic = makeCdbAggTactic(1, 1);
		tactic->finals[0].argindex = 0;

		/* The partial of a SUM is the same SUM... */
		tactic->partials[0].aggfnoid = aggref->aggfnoid;
		tactic->partials[0].rettype = aggref->aggtype;

		if (argTypeOid == aggref->aggtype)
		{
			/*
			 * Some SUMs return their arg type, like MIN or MAX.  These
			 * include FLOAT4, FLOAT8, CASH, INTERVAL, and NUMERIC.
			 *
			 * TODO: We should use the aggfnoid to determine the argument type,
			 * but we go out on a limb by deducing the type from the argument.
			 */
			tactic->finals[0].aggfnoid = aggref->aggfnoid;
			tactic->finals[0].rettype = aggref->aggtype;
		}
		else if (aggref->aggtype == INT8OID)
		{
			/*
			 * If the desired result of SUM is INT8, then the result of the
			 * partial aggregation, INT8, will sum to NUMERIC.	 We choose
			 * that as the final aggregration and then down cast to INT8.
			 */
			tactic->finals[0].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
			tactic->finals[0].rettype = NUMERICOID;		/* Returns numeric */

			tactic->consolidator = CDB_NUMERIC_TO_INT8_OID;
		}
		else if (aggref->aggtype == NUMERICOID)
		{
			/*
			 * If the desired result of SUM is NUMERIC, the final will just be
			 * the NUMERiC->NUMERIC SUM.
			 */
			tactic->finals[0].aggfnoid = CDB_SUM_NUMERIC_OID;
			tactic->finals[0].rettype = NUMERICOID;
		}
		else
		{
			/*
			 * Any other type expectation is beyond our current ability.
			 */
			freeAggTactic(tactic);
			tactic = NULL;
			*msg = "Cannot SUM partitioned attribute of that datatype yet";
		}
	}
	else if (CDB_AVG_OID_LO <= aggref->aggfnoid
			 && aggref->aggfnoid <= CDB_AVG_OID_HI)
	{
		bool		dataTypesOk = false;

		/* AVG */

		Oid			argTypeOid = exprType((Node *) (aggref->target));

		/* Two partials with corresponding finals. */
		tactic = makeCdbAggTactic(2, 2);
		tactic->finals[0].argindex = 0;
		tactic->finals[1].argindex = 1;


		/* The second partial of AVG is always COUNT */
		tactic->partials[1].aggfnoid = CDB_COUNT_OID;
		tactic->partials[1].rettype = INT8OID;	/* COUNT returns int8 */


		switch (argTypeOid)
		{
			case INT8OID:
				if (aggref->aggtype != NUMERICOID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->partials[0].rettype = NUMERICOID;

				tactic->finals[0].aggfnoid = CDB_SUM_NUMERIC_OID;
				tactic->finals[0].rettype = NUMERICOID;

				tactic->finals[1].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[1].rettype = NUMERICOID;

				tactic->consolidator = CDB_DIV_NUMERIC;

				dataTypesOk = true;
				break;

			case INT4OID:
				if (aggref->aggtype != NUMERICOID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_INT4_TO_INT8_OID;
				tactic->partials[0].rettype = INT8OID;

				tactic->finals[0].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[0].rettype = NUMERICOID;

				tactic->finals[1].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[1].rettype = NUMERICOID;

				tactic->consolidator = CDB_DIV_NUMERIC;

				dataTypesOk = true;
				break;

			case INT2OID:
				if (aggref->aggtype != NUMERICOID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_INT2_TO_INT8_OID;
				tactic->partials[0].rettype = INT8OID;

				tactic->finals[0].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[0].rettype = NUMERICOID;

				tactic->finals[1].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[1].rettype = NUMERICOID;

				tactic->consolidator = CDB_DIV_NUMERIC;

				dataTypesOk = true;
				break;

			case NUMERICOID:
				if (aggref->aggtype != NUMERICOID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_NUMERIC_OID;
				tactic->partials[0].rettype = NUMERICOID;

				tactic->finals[0].aggfnoid = CDB_SUM_NUMERIC_OID;
				tactic->finals[0].rettype = NUMERICOID;

				tactic->finals[1].aggfnoid = CDB_SUM_INT8_TO_NUMERIC_OID;
				tactic->finals[1].rettype = NUMERICOID;

				tactic->consolidator = CDB_DIV_NUMERIC;

				dataTypesOk = true;
				break;

			case FLOAT4OID:
				if (aggref->aggtype != FLOAT8OID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_FLOAT4_OID;
				tactic->partials[0].rettype = FLOAT4OID;

				tactic->finals[0].argfnoid = CDB_FLOAT4_TO_FLOAT8;
				tactic->finals[0].argtype = FLOAT8OID;
				tactic->finals[0].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->finals[0].rettype = FLOAT8OID;

				tactic->finals[1].argfnoid = CDB_INT8_TO_FLOAT8_OID;
				tactic->finals[1].argtype = FLOAT8OID;
				tactic->finals[1].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->finals[1].rettype = FLOAT8OID;

				tactic->consolidator = CDB_DIV_FLOAT8;

				dataTypesOk = true;
				break;

			case FLOAT8OID:
				if (aggref->aggtype != FLOAT8OID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->partials[0].rettype = FLOAT8OID;

				tactic->finals[0].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->finals[0].rettype = FLOAT8OID;

				tactic->finals[1].argfnoid = CDB_INT8_TO_FLOAT8_OID;
				tactic->finals[1].argtype = FLOAT8OID;
				tactic->finals[1].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->finals[1].rettype = FLOAT8OID;

				tactic->consolidator = CDB_DIV_FLOAT8;

				dataTypesOk = true;
				break;

			case INTERVALOID:
				if (aggref->aggtype != INTERVALOID)
					break;

				tactic->partials[0].aggfnoid = CDB_SUM_INTERVAL_OID;
				tactic->partials[0].rettype = INTERVALOID;

				tactic->finals[0].aggfnoid = CDB_SUM_INTERVAL_OID;
				tactic->finals[0].rettype = INTERVALOID;

				tactic->finals[1].argfnoid = CDB_INT8_TO_FLOAT8_OID;
				tactic->finals[1].argtype = FLOAT8OID;
				tactic->finals[1].aggfnoid = CDB_SUM_FLOAT8_OID;
				tactic->finals[1].rettype = FLOAT8OID;

				tactic->consolidator = CDB_DIV_INTERVAL;

				dataTypesOk = true;
				break;

			default:
				break;
		}

		if (!dataTypesOk)
		{
			freeAggTactic(tactic);
			tactic = NULL;
			*msg = "Cannot AVG partitioned attribute of that datatype yet";
		}
	}
	else if ((CDB_MAX_OID_LO <= aggref->aggfnoid
			  && aggref->aggfnoid <= CDB_MAX_OID_HI)
			 ||
			 (CDB_MIN_OID_LO <= aggref->aggfnoid
			  && aggref->aggfnoid <= CDB_MIN_OID_HI))
	{
		/* MIN or MAX */
		tactic = makeCdbAggTactic(1, 1);

		tactic->partials[0].aggfnoid = aggref->aggfnoid;
		tactic->partials[0].rettype = aggref->aggtype;

		tactic->finals[0].aggfnoid = aggref->aggfnoid;
		tactic->finals[0].rettype = aggref->aggtype;
		tactic->finals[0].argindex = 0;
	}

	else if (CDB_VARIANCE_OID_LO <= aggref->aggfnoid
			 && aggref->aggfnoid <= CDB_VARIANCE_OID_HI)
	{
		/* VARIANCE */
		*msg = "Cannot find VARIANCE of partitioned attribute yet";
	}
	else if (CDB_STDDEV_OID_LO <= aggref->aggfnoid
			 && aggref->aggfnoid <= CDB_STDDEV_OID_HI)
	{
		/* STDDEV */
		*msg = "Cannot find STDDEV of partitioned attribute yet";
	}
	else
	{
		*msg = "Cannot aggregate partitioned attribute";
	}
	return tactic;
}
