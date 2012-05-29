/*
 * Copyright (c) 2012 EMC Corporation All Rights Reserved
 *
 * This software is protected, without limitation, by copyright law
 * and international treaties. Use of this software and the intellectual
 * property contained therein is expressly limited to the terms and
 * conditions of the License Agreement under which it is provided by
 * or on behalf of EMC.
 *
 * ---------------------------------------------------------------------
 *
 * Interface to functions related to dynamic partition selection in GPDB.
 *
 * This is used to expose these functions in a dynamicly linked library
 * so that they can be referenced by using CREATE FUNCTION command in sql,
 * like below:
 *
 * CREATE OR REPLACE FUNCTION gp_partition_propagation(int, oid)
 *   RETURNS void
 *   AS '$libdir/gp_partition_functions.so', 'gp_partition_propagation_wrapper' LANGUAGE C STRICT;
 *
 * CREATE OR REPLACE FUNCTION gp_partition_selection(oid, anyelement)
 *   RETURNS oid
 *   AS '$libdir/gp_partition_functions.so', 'gp_partition_selection_wrapper' LANGUAGE C STRICT;
 *
 * CREATE OR REPLACE FUNCTION gp_partition_expansion(oid)
 *   RETURNS setof oid
 *   AS '$libdir/gp_partition_functions.so', 'gp_partition_expansion_wrapper' LANGUAGE C STRICT;
 */

#include "postgres.h"
#include <string.h>
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern Datum gp_partition_propagation(PG_FUNCTION_ARGS);
extern Datum gp_partition_selection(PG_FUNCTION_ARGS);
extern Datum gp_partition_expansion(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gp_partition_propagation_wrapper);
PG_FUNCTION_INFO_V1(gp_partition_selection_wrapper);
PG_FUNCTION_INFO_V1(gp_partition_expansion_wrapper);

extern Datum gp_partition_propagation_wrapper(PG_FUNCTION_ARGS);
extern Datum gp_partition_selection_wrapper(PG_FUNCTION_ARGS);
extern Datum gp_partition_expansion_wrapper(PG_FUNCTION_ARGS);

/*
 * gp_partition_propagation_wrapper
 *    The wrapper for creating a user-defined C function for gp_partition_propagation.
 */
Datum
gp_partition_propagation_wrapper(PG_FUNCTION_ARGS)
{
	return gp_partition_propagation(fcinfo);
}

/*
 * gp_partition_selection_wrapper
 *    The wrapper for creating a user-defined C function for gp_partition_selection.
 */
Datum
gp_partition_selection_wrapper(PG_FUNCTION_ARGS)
{
	return gp_partition_selection(fcinfo);
}

/*
 * gp_partition_expansion_wrapper
 *    The wrapper for creating a user-defined C function for gp_partition_expansion.
 */
Datum
gp_partition_expansion_wrapper(PG_FUNCTION_ARGS)
{
	return gp_partition_expansion(fcinfo);
}
