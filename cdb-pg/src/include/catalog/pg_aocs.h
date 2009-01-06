/*-------------------------------------------------------------------------
*
* pg_aocs.h
*	  internal specifications of the appendonly column storage relation.
*
* Portions Copyright (c) 2008, Greenplum Inc.
*-------------------------------------------------------------------------
*/
#ifndef PG_AOCS_H
#define PG_AOCS_H

/*
 * pg_appendonly definition.
 */
#define AOCSRelationId  6107

CATALOG(pg_aocs,6107) BKI_WITHOUT_OIDS
{
	Oid			relid;			/* relation id */
	int2 			attnum;			/* Column id */

	int2 			cs_fileno;		/* Column Store file id */
	int2 			cs_attnum;		/* colid within CS file */

	int4 			cs_file_option; 	/* file level option */
	int4 			cs_col_option; 		/* column level option */
} FormData_pg_aocs;

typedef FormData_pg_aocs *Form_pg_aocs;

#define Natts_pg_aocs 				6 
#define Anum_pg_aocs_relid			1
#define Anum_pg_aocs_attnum 			2
#define Anum_pg_aocs_cs_fileno 			3
#define Anum_pg_aocs_cs_attnum 			4
#define Anum_pg_aocs_file_option 		5
#define Anum_pg_aocs_col_option 		6	

#endif   /* PG_AOCS_H */
