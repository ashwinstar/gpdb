/*-------------------------------------------------------------------------
*
* pg_partition_rule.h
*
*	  internal specifications of the partition configuration rules.
*
* Portions Copyright (c) 2008, Greenplum Inc.
*-------------------------------------------------------------------------
*/
#ifndef PG_PARTITION_RULE_H
#define PG_PARTITION_RULE_H


#include "catalog/genbki.h"

/*
 * pg_partition definition.
 */
#define PartitionRuleRelationId  5011

CATALOG(pg_partition_rule,5011)
{
	Oid paroid;
	Oid parchildrelid;
	Oid parparentrule;
	NameData parname;
	bool parisdefault;
	int2 parruleord;
	bool parrangestartincl;
	bool parrangeendincl;
	text parrangestart;
	text parrangeend;
	text parrangeevery;
	text parlistvalues;
	text parreloptions[1];
} FormData_pg_partition_rule;


/* ----------------
 *		Form_pg_partition_rule corresponds to a pointer to a tuple with
 *		the format of pg_partition_rule relation.
 * ----------------
 */
typedef FormData_pg_partition_rule *Form_pg_partition_rule;

#define Natts_pg_partition_rule						13
#define Anum_pg_partition_rule_paroid				1
#define Anum_pg_partition_rule_parchildrelid		2
#define Anum_pg_partition_rule_parparentrule		3
#define Anum_pg_partition_rule_parname				4
#define Anum_pg_partition_rule_parisdefault			5
#define Anum_pg_partition_rule_parruleord			6
#define Anum_pg_partition_rule_parrangestartincl	7
#define Anum_pg_partition_rule_parrangeendincl		8
#define Anum_pg_partition_rule_parrangestart		9
#define Anum_pg_partition_rule_parrangeend			10
#define Anum_pg_partition_rule_parrangeevery		11
#define Anum_pg_partition_rule_parlistvalues		12
#define Anum_pg_partition_rule_parreloptions		13

#endif   /* PG_PARTITION_RULE_H */
