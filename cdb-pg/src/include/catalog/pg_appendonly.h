/*-------------------------------------------------------------------------
*
* pg_appendonly.h
*	  internal specifications of the appendonly relation storage.
*
* Portions Copyright (c) 2008, Greenplum Inc.
*-------------------------------------------------------------------------
*/
#ifndef PG_APPENDONLY_H
#define PG_APPENDONLY_H

/*
 * pg_appendonly definition.
 */
#define AppendOnlyRelationId  5006

CATALOG(pg_appendonly,5006) BKI_WITHOUT_OIDS
{
	Oid			relfilenode;	/* identifier of physical storage file */
	float8		eof;			/* the effective eof for this relfile  */
	float8		tupcount;		/* the (per seg) total number of tuples */
	float8		varblockcount;	/* the (per seg) total number of varblocks */
} FormData_pg_appendonly;


/* ----------------
 *		Form_pg_appendonly corresponds to a pointer to a tuple with
 *		the format of pg_appendonly relation.
 * ----------------
 */
typedef FormData_pg_appendonly *Form_pg_appendonly;

#define Natts_pg_appendonly					4
#define Anum_pg_appendonly_relfilenode		1
#define Anum_pg_appendonly_eof				2
#define Anum_pg_appendonly_tupcount			3
#define Anum_pg_appendonly_varblockcount	4

/*
 * Descriptor of a single AO relation.
 * For now very similar to the catalog row itself but
 * will change in time.
 */
typedef struct AppendOnlyEntry
{
	float8		eof;			/* the effective eof for this relfile  */
	float8		tupcount;		/* the (per seg) total number of tuples */
	float8		varblockcount;	/* the (per seg) total number of varblocks */	
} AppendOnlyEntry;

/* No initial contents. */

extern AppendOnlyEntry *
GetAppendOnlyEntry(Oid relfilenode);

extern void
UpdateAppendOnlyEntry(Oid relfilenode, int64 eof, float8 tuples_added, float8 varblocks_added);

extern void
RemoveAppendonlyEntries(Oid relfilenode);

#endif   /* PG_APPENDONLY_H */
