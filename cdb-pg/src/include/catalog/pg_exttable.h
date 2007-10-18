/*-------------------------------------------------------------------------
*
* pg_exttable.h
*	  definitions for system wide external relations
*
* Copyright (c) 2007, greenplum inc
*
*-------------------------------------------------------------------------
*/

#ifndef PG_EXTTABLE_H
#define PG_EXTTABLE_H


/*
 * pg_exttable definition.
 */
#define ExtTableRelationId  6040

CATALOG(pg_exttable,6040) BKI_WITHOUT_OIDS
{
	Oid			reloid;			/* refers to this relation's oid in pg_class */
	text		location[1];	/* array of URI strings */
	char		fmttype;		/* 't' (text) or 'c' (csv) */
	text		fmtopts;		/* the data format options */
	text		command;		/* the command string to EXECUTE */
	int4		rejectlimit;	/* error count reject limit per segment */
	char		rejectlimittype; /* 'r' (rows) or 'p' (percent) */
	Oid			fmterrtbl;		/* the data format error table oid in pg_class */
	int4		encoding;		/* character encoding of this external table */

} FormData_pg_exttable;


/* ----------------
*		Form_pg_exttable corresponds to a pointer to a tuple with
*		the format of pg_exttable relation.
* ----------------
*/
typedef FormData_pg_exttable *Form_pg_exttable;

#define Natts_pg_exttable			9
#define Anum_pg_exttable_reloid		1
#define Anum_pg_exttable_location	2
#define Anum_pg_exttable_fmtcode	3
#define Anum_pg_exttable_fmtopts	4
#define Anum_pg_exttable_command    5
#define Anum_pg_exttable_rejectlimit 6
#define Anum_pg_exttable_rejectlimittype 7
#define Anum_pg_exttable_fmterrtbl 8
#define Anum_pg_exttable_encoding 9

/* No initial contents. */

#endif /* PG_EXTTABLE_H */
