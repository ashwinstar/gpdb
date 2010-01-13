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

	Oid tablespace,

	Oid database,

	FormData_pg_class *pgClass,

	FormData_pg_attribute *attrArray,

	Form_pg_attribute *attrPtrArray)
{
	if (!direct->isInit)
	{
		int i;

		/*
		 * Lots of Hard-coded construction of the gp_persistent* RelationS and
		 * dependent objects like tuple descriptors, etc.
		 */
		direct->relationData.rd_node.relNode = direct->relId;

		direct->relationData.rd_refcnt = 0;
		direct->relationData.rd_isvalid = true;

		direct->relationData.rd_rel = pgClass;

		// Not much in terms of contraints.
		direct->constrData.has_not_null = true;

		/*
		 * Setup tuple descriptor for columns.
		 */
		direct->descData.natts = direct->natts;

		// Make the array of pointers.
		direct->descData.attrs = attrPtrArray;
		for (i = 0; i < direct->natts; i++) \
			direct->descData.attrs[i] = &attrArray[i];

		direct->descData.constr = &direct->constrData;
		direct->descData.tdtypeid = direct->relType;
		direct->descData.tdtypmod = -1;
		direct->descData.tdhasoid = direct->hasOid;
		direct->descData.tdrefcount = 1;

		direct->relationData.rd_att = &direct->descData;

		direct->pgStat.t_id = direct->relId;
		direct->pgStat.t_shared = 1;

		direct->relationData.pgstat_info = &direct->pgStat;

		direct->isInit = true;
	}

	direct->relationData.rd_node.spcNode = tablespace;
	direct->relationData.rd_node.dbNode = database;

	direct->relationData.rd_smgr =
					smgropen(direct->relationData.rd_node);

	
	direct->relationData.rd_refcnt++;

	return &direct->relationData;
}

void DirectOpen_Close(
	DirectOpen 	*direct,
	Relation	rel)
{
	Assert(direct->isInit);
	Assert(rel == &direct->relationData);

	smgrclose(direct->relationData.rd_smgr);
	direct->relationData.rd_smgr = NULL;

	direct->relationData.rd_refcnt++;
}
