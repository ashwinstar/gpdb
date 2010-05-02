/*-------------------------------------------------------------------------
 *
 * Copyright (c) 2010 Greenplum
 *
 * Functions to support administrative tasks against gp_persistent_*_node
 * tables, gp_global_sequence and gp_relation_node. These tables do not have
 * normal MVCC semantics and changing them is usually part of a more complex
 * chain of events in the backend. So, we cannot modify them with INSERT, UPDATE
 * or DELETE as we might other catalog tables.
 *
 * So we provide these functions which update the tables and also update the in
 * memory persistent table / file rep data structures.
 */
#include "postgres.h"
#include "miscadmin.h"

#include "utils/builtins.h"

#define NYI elog(ERROR, "not yet implemented")

Datum
gp_add_persistent_filespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_add_persistent_tablespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_add_persistent_database_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_add_persistent_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_add_global_sequence_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_add_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_persistent_filespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_persistent_tablespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_persistent_database_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_persistent_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_global_sequence_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_update_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_persistent_filespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_persistent_tablespace_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_persistent_database_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_persistent_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_global_sequence_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}

Datum
gp_delete_relation_node_entry(PG_FUNCTION_ARGS)
{
	NYI;
	PG_RETURN_BOOL(true);
}
