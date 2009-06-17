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
#define AppendOnlyRelationId  6105

CATALOG(pg_appendonly,6105) BKI_WITHOUT_OIDS
{
	Oid				relid;				/* relation id */
	int4			blocksize;			/* the max block size of this relation */
	int4			safefswritesize;	/* min write size in bytes to prevent torn-write */
	int2			compresslevel;		/* the (per seg) total number of varblocks */
	int2			majorversion;		/* major version indicating what's stored in this table  */
	int2			minorversion;		/* minor version indicating what's stored in this table  */
	bool			checksum;			/* true if checksum is stored with data and checked */
    bool            columnstore;        /* true if orientation is column */ 
	text			compresstype;		/* the compressor used (zlib, or quicklz) */
} FormData_pg_appendonly;


/* ----------------
*		Form_pg_appendonly corresponds to a pointer to a tuple with
*		the format of pg_appendonly relation.
* ----------------
*/
typedef FormData_pg_appendonly *Form_pg_appendonly;

#define Natts_pg_appendonly					8
#define Anum_pg_appendonly_relid			1
#define Anum_pg_appendonly_blocksize		2
#define Anum_pg_appendonly_safefswritesize	3
#define Anum_pg_appendonly_compresslevel	4
#define Anum_pg_appendonly_majorversion		5
#define Anum_pg_appendonly_minorversion		6
#define Anum_pg_appendonly_checksum			7
#define Anum_pg_appendonly_columnstore      8	
#define Anum_pg_appendonly_compresstype		9

/*
 * Descriptor of a single AO relation.
 * For now very similar to the catalog row itself but may change in time.
 */
typedef struct AppendOnlyEntry
{
	int		blocksize;
	int		safefswritesize;
	int		compresslevel;
	int		majorversion;
	int		minorversion;
	bool	checksum;
    bool    columnstore;
	char*	compresstype;
} AppendOnlyEntry;

/* No initial contents. */

extern void
InsertAppendOnlyEntry(Oid relid, 
					  int blocksize, 
					  int safefswritesize, 
					  int compresslevel,
					  bool checksum,
                      bool columnstore,
					  char* compresstype);

extern AppendOnlyEntry *
GetAppendOnlyEntry(Oid relid);

extern void
RemoveAppendonlyEntry(Oid relid);

#endif   /* PG_APPENDONLY_H */
