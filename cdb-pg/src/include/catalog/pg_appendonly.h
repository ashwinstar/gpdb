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

#include "catalog/genbki.h"
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
	text			compresstype;		/* the compressor used (zlib, or quicklz) */
    bool            columnstore;        /* true if orientation is column */ 
    Oid             segrelid;           /* OID of aoseg table; 0 if none */
    Oid             segidxid;           /* if aoseg table, OID of segno index */
    Oid             blkdirrelid;        /* OID of aoblkdir table; 0 if none */
    Oid             blkdiridxid;        /* if aoblkdir table, OID of aoblkdir index */
} FormData_pg_appendonly;


/* ----------------
*		Form_pg_appendonly corresponds to a pointer to a tuple with
*		the format of pg_appendonly relation.
* ----------------
*/
typedef FormData_pg_appendonly *Form_pg_appendonly;

#define Natts_pg_appendonly					13
#define Anum_pg_appendonly_relid			1
#define Anum_pg_appendonly_blocksize		2
#define Anum_pg_appendonly_safefswritesize	3
#define Anum_pg_appendonly_compresslevel	4
#define Anum_pg_appendonly_majorversion		5
#define Anum_pg_appendonly_minorversion		6
#define Anum_pg_appendonly_checksum			7
#define Anum_pg_appendonly_compresstype		8
#define Anum_pg_appendonly_columnstore      9
#define Anum_pg_appendonly_segrelid         10
#define Anum_pg_appendonly_segidxid         11
#define Anum_pg_appendonly_blkdirrelid      12
#define Anum_pg_appendonly_blkdiridxid      13

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
	char*	compresstype;
    bool    columnstore;
	Oid     segrelid;
	Oid     segidxid;
	Oid     blkdirrelid;
	Oid     blkdiridxid;
} AppendOnlyEntry;

/* No initial contents. */

extern void
InsertAppendOnlyEntry(Oid relid, 
					  int blocksize, 
					  int safefswritesize, 
					  int compresslevel,
					  bool checksum,
                      bool columnstore,
					  char* compresstype,
					  Oid segrelid,
					  Oid segidxid,
					  Oid blkdirrelid,
					  Oid blkdiridxid);

extern AppendOnlyEntry *
GetAppendOnlyEntry(Oid relid);

/*
 * Get the OIDs of the auxiliary relations and their indexes for an appendonly
 * relation.
 *
 * The OIDs will be retrieved only when the corresponding output variable is
 * not NULL.
 */
void
GetAppendOnlyEntryAuxOids(Oid relid,
						  Oid *segrelid,
						  Oid *segidxid,
						  Oid *blkdirrelid,
						  Oid *blkdiridxid);

/*
 * Update the segrelid and/or blkdirrelid if the input new values
 * are valid OIDs.
 */
extern void
UpdateAppendOnlyEntryAuxOids(Oid relid,
							 Oid newSegrelid,
							 Oid newSegidxid,
							 Oid newBlkdirrelid,
							 Oid newBlkdiridxid);

extern void
RemoveAppendonlyEntry(Oid relid);

#endif   /* PG_APPENDONLY_H */
