/*-------------------------------------------------------------------------
 *
 * gp_persistent.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/palloc.h"
#include "storage/fd.h"

#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbsharedoidsearch.h"
#include "storage/itemptr.h"
#include "storage/shmem.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "storage/ipc.h"
#include "utils/builtins.h"

void GpPersistentRelationNode_GetValues(
	Datum						*values,

	Oid							*tablespace,
	Oid							*database,
	Oid							*relation,
	PersistentFileSysState		*state,
	TransactionId				*parentXid,
	int64						*serialNum,
	ItemPointerData				*previousFreeTid)
{
	*tablespace = DatumGetObjectId(values[Anum_gp_persistent_relation_node_tablespace_oid - 1]);

	*database = DatumGetObjectId(values[Anum_gp_persistent_relation_node_database_oid - 1]);

	*relation = DatumGetObjectId(values[Anum_gp_persistent_relation_node_relfilenode_oid - 1]);

	*state = (PersistentFileSysState)DatumGetInt16(values[Anum_gp_persistent_relation_node_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_relation_node_parent_xid - 1]);

	*serialNum = DatumGetInt64(values[Anum_gp_persistent_relation_node_persistent_serial_num - 1]);

	*previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_relation_node_previous_free_tid - 1]));

}

void GpPersistentRelationNode_SetDatumValues(
	Datum					*values,

	Oid						tablespace,
	Oid						database,
	Oid						relation,
	PersistentFileSysState 	state,
	TransactionId			parentXid,
	int64					serialNum,
	ItemPointerData			*previousFreeTid)
{
	values[Anum_gp_persistent_relation_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespace);
	values[Anum_gp_persistent_relation_node_database_oid - 1] = 
									ObjectIdGetDatum(database);
	values[Anum_gp_persistent_relation_node_relfilenode_oid - 1] = 
									ObjectIdGetDatum(relation);

	values[Anum_gp_persistent_relation_node_state - 1] = 
									Int16GetDatum(state);

	values[Anum_gp_persistent_relation_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_relation_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_relation_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentDatabaseNode_GetValues(
	Datum						*values,

	Oid							*tablespace,
	Oid							*database,
	PersistentFileSysState		*state,
	TransactionId				*parentXid,
	int64						*serialNum,
	ItemPointerData				*previousFreeTid)
{
    *tablespace = DatumGetObjectId(values[Anum_gp_persistent_database_node_tablespace_oid - 1]);

    *database = DatumGetObjectId(values[Anum_gp_persistent_database_node_database_oid - 1]);

    *state = DatumGetInt16(values[Anum_gp_persistent_database_node_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_database_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_database_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_database_node_previous_free_tid - 1]));

}

void GpPersistentDatabaseNode_SetDatumValues(
	Datum					*values,

	Oid						tablespace,
	Oid						database,
	PersistentFileSysState 	state,
	TransactionId			parentXid,
	int64					serialNum,
	ItemPointerData			*previousFreeTid)
{
	values[Anum_gp_persistent_database_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespace);
	values[Anum_gp_persistent_database_node_database_oid - 1] = 
									ObjectIdGetDatum(database);

	values[Anum_gp_persistent_database_node_state - 1] = 
									Int16GetDatum(state);

	values[Anum_gp_persistent_database_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_database_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_database_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentTablespaceNode_GetValues(
	Datum						*values,

	Oid							*tablespace,
	PersistentFileSysState		*state,
	TransactionId				*parentXid,
	int64						*serialNum,
	ItemPointerData				*previousFreeTid)
{
    *tablespace = DatumGetObjectId(values[Anum_gp_persistent_tablespace_node_tablespace_oid - 1]);

    *state = DatumGetInt16(values[Anum_gp_persistent_tablespace_node_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_tablespace_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_tablespace_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_tablespace_node_previous_free_tid - 1]));

}

void GpPersistentTablespaceNode_SetDatumValues(
	Datum					*values,

	Oid						tablespace,
	PersistentFileSysState 	state,
	TransactionId			parentXid,
	int64					serialNum,
	ItemPointerData			*previousFreeTid)
{
	values[Anum_gp_persistent_tablespace_node_tablespace_oid - 1] = 
									ObjectIdGetDatum(tablespace);

	values[Anum_gp_persistent_tablespace_node_state - 1] = 
									Int16GetDatum(state);

	values[Anum_gp_persistent_tablespace_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_tablespace_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_tablespace_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}

void GpPersistentFilespaceNode_GetValues(
	Datum						*values,

	Oid							*filespace,
	PersistentFileSysState		*state,
	TransactionId				*parentXid,
	int64						*serialNum,
	ItemPointerData				*previousFreeTid)
{
    *filespace = DatumGetObjectId(values[Anum_gp_persistent_filespace_node_filespace_oid - 1]);

    *state = DatumGetInt16(values[Anum_gp_persistent_filespace_node_state - 1]);

	*parentXid = (TransactionId)DatumGetInt32(values[Anum_gp_persistent_filespace_node_parent_xid - 1]);

    *serialNum = DatumGetInt64(values[Anum_gp_persistent_filespace_node_persistent_serial_num - 1]);

    *previousFreeTid = *((ItemPointer) DatumGetPointer(values[Anum_gp_persistent_filespace_node_previous_free_tid - 1]));

}

void GpPersistentFilespaceNode_SetDatumValues(
	Datum					*values,

	Oid						filespace,
	PersistentFileSysState 	state,
	TransactionId			parentXid,
	int64					serialNum,
	ItemPointerData			*previousFreeTid)
{
	values[Anum_gp_persistent_filespace_node_filespace_oid - 1] = 
									ObjectIdGetDatum(filespace);

	values[Anum_gp_persistent_filespace_node_state - 1] = 
									Int16GetDatum(state);

	values[Anum_gp_persistent_filespace_node_parent_xid - 1] = 
									Int32GetDatum(parentXid);

	values[Anum_gp_persistent_filespace_node_persistent_serial_num - 1] = 
									Int64GetDatum(serialNum);

	values[Anum_gp_persistent_filespace_node_previous_free_tid - 1] =
									PointerGetDatum(previousFreeTid);
}
