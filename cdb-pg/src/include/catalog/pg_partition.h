/*-------------------------------------------------------------------------
*
* pg_partition.h
*	  internal specifications of the partition configuration.
*
* Portions Copyright (c) 2008, Greenplum Inc.
*-------------------------------------------------------------------------
*/
#ifndef PG_PARTITION_H
#define PG_PARTITION_H

/*
 * pg_partition definition.
 */
#define PartitionRelationId  5010

CATALOG(pg_partition,5010)
{
	Oid relid;
	char parkind;
	int2 parlevel;
	bool paristemplate;
	int2 parnatts;
	int2vector paratts;
	oidvector parclass;
} FormData_pg_partition;


/* ----------------
 *		Form_pg_partition corresponds to a pointer to a tuple with
 *		the format of pg_partition relation.
 * ----------------
 */
typedef FormData_pg_partition *Form_pg_partition;

#define Natts_pg_partition					7
#define Anum_pg_partition_relid				1
#define Anum_pg_partition_parkind			2
#define Anum_pg_partition_parlevel			3
#define Anum_pg_partition_paristemplate		4
#define Anum_pg_partition_parnatts			5
#define Anum_pg_partition_paratts			6
#define Anum_pg_partition_parclass			7


#endif   /* PG_PARTITION_H */
