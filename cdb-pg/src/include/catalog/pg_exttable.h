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
	char		fmttype;		/* refers to pg_dataformat */
	text		fmtopts;		/* the data format options */
} FormData_pg_exttable;


/* ----------------
*		Form_pg_exttable corresponds to a pointer to a tuple with
*		the format of pg_exttable relation.
* ----------------
*/
typedef FormData_pg_exttable *Form_pg_exttable;

#define Natts_pg_exttable			4
#define Anum_pg_exttable_reloid		1
#define Anum_pg_exttable_location	2
#define Anum_pg_exttable_fmtcode	3
#define Anum_pg_exttable_fmtopts	4

/* No initial contents. */


/*
 * prototypes for functions in pg_exttable.c
 */
extern void ExternalRelationCreate(Oid tbloid, 
								   Datum locations, 
								   Datum formatopts, 
								   char formattype);


#endif /* PG_EXTTABLE_H */
