/*-------------------------------------------------------------------------
 *
 * pg_foreign_table.h
 *	  definition of the system "foreign table" relation (pg_foreign_table)
 *
 * Portions Copyright (c) 2009, Greenplum Inc.
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_TABLE_H
#define PG_FOREIGN_TABLE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_foreign_table definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_server
 * ----------------
 */
#define ForeignTableRelationId 2879

CATALOG(pg_foreign_table,2879) BKI_WITHOUT_OIDS
{
	Oid			reloid;			/* refers to this relation's oid in pg_class */
	Oid			server;			/* table's foreign server */

	/*
	 * VARIABLE LENGTH FIELDS start here.  These fields may be NULL, too.
	 */
	text		tbloptions[1];	/* foreign table-specific options */
	
} FormData_pg_foreign_table;

/* ----------------
 *		Form_pg_foreign_table corresponds to a pointer to a tuple with
 *		the format of pg_foreign_table relation.
 * ----------------
 */
typedef FormData_pg_foreign_table *Form_pg_foreign_table;

/* ----------------
 *		compiler constants for pg_foreign_table
 * ----------------
 */

#define Natts_pg_foreign_table					3
#define Anum_pg_foreign_table_reloid			1
#define Anum_pg_foreign_table_server			2
#define Anum_pg_foreign_table_tbloptions		3

extern void
InsertForeignTableEntry(Oid relid, char	*servername, List *options);

extern void
RemoveForeignTableEntry(Oid relid);

#endif   /* PG_FOREIGN_TABLE_H */
