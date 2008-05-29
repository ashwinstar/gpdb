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

/*
 * pg_partition definition.
 */
#define PartitionRuleRelationId  5011

CATALOG(pg_partition_rule,5011)
{
	Oid paroid;
	Oid parrelid;
	Oid parparentrule;
	name parname;
	bool parisdefault;
	int2 parruleord;
	bool parrangestartincl;
	bool parrangeendincl;
	text parrangestart;
	text parrangeend;
	text parrangeevery;
	text parlistvalues;
} FormData_pg_partition_rule;


/* ----------------
 *		Form_pg_partition_rule corresponds to a pointer to a tuple with
 *		the format of pg_partition_rule relation.
 * ----------------
 */
typedef FormData_pg_partition_rule *Form_pg_partition_rule;

#endif   /* PG_PARTITION_RULE_H */
