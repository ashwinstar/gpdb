/*-------------------------------------------------------------------------
 *
 * cdbdirectopen.c
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "cdb/cdbdirectopen.h"
#include "utils/guc.h"
#include "storage/smgr.h"

Relation DirectOpen_Open(
	DirectOpen *direct,

	Oid relationId,

	Oid tablespace,

	Oid database,

	Oid relfilenode,

	FormData_pg_class *pgClass,

	FormData_pg_attribute *attrArray)
{
	int i;

	if (relationId == -1)
		relationId = pgClass->relfilenode;		// Assume it is ok to use the relfilenode as the relationId in our limited usage.

	if (relfilenode == -1)
		relfilenode = pgClass->relfilenode;

	/*
	 * Lots of Hard-coded construction of the gp_persistent* RelationS and
	 * dependent objects like tuple descriptors, etc.
	 */
	direct->relationData.rd_node.relNode = relfilenode;

	direct->relationData.rd_refcnt = 0;
	direct->relationData.rd_isvalid = true;

	direct->relationData.rd_id = relationId;

	direct->relationData.rd_rel = pgClass;

	// Not much in terms of contraints.
	direct->constrData.has_not_null = true;

	/*
	 * Setup tuple descriptor for columns.
	 */
	direct->descData.natts = pgClass->relnatts;

	// Make the array of pointers.
	direct->descData.attrs = 
			(Form_pg_attribute*)
					palloc(sizeof(Form_pg_attribute*) * pgClass->relnatts);

	for (i = 0; i < pgClass->relnatts; i++)
	{
		direct->descData.attrs[i] = 
					(Form_pg_attribute)
							palloc(sizeof(FormData_pg_attribute));

		memcpy(direct->descData.attrs[i], &(attrArray[i]), sizeof(FormData_pg_attribute));

		// Patch up relation id.
		direct->descData.attrs[i]->attrelid = relationId;
	}

	direct->descData.constr = &direct->constrData;
	direct->descData.tdtypeid = pgClass->reltype;
	direct->descData.tdtypmod = -1;
	direct->descData.tdhasoid = direct->hasOid;
	direct->descData.tdrefcount = 1;

	direct->relationData.rd_att = &direct->descData;

	direct->pgStat.t_id = relationId;
	direct->pgStat.t_shared = 1;

	direct->relationData.pgstat_info = &direct->pgStat;


	direct->relationData.rd_node.spcNode = tablespace;
	direct->relationData.rd_node.dbNode = database;

	direct->relationData.rd_refcnt++;

	RelationOpenSmgr(&direct->relationData);
	
	return &direct->relationData;
}

void DirectOpen_Close(
	DirectOpen 	*direct,
	Relation	rel)
{
	int i;

	Assert(rel == &direct->relationData);
	Assert(direct->descData.attrs != NULL);

	RelationCloseSmgr(&direct->relationData);
	
	direct->relationData.rd_refcnt--;

	for (i = 0; i < direct->relationData.rd_rel->relnatts; i++)
	{
		Assert(direct->descData.attrs[i] != NULL);
		pfree(direct->descData.attrs[i]);
	}
	pfree(direct->descData.attrs);
	direct->descData.attrs = NULL;
}
