/*-------------------------------------------------------------------------
*
* aosegfiles.h
*	  internal specifications of the pg_aoseg_* Append Only file segment
*	  list relation.
*
* Portions Copyright (c) 2008, Greenplum Inc.
*-------------------------------------------------------------------------
*/
#ifndef AOSEGFILES_H
#define AOSEGFILES_H

#define Natts_pg_aoseg					4
#define Anum_pg_aoseg_segno				1
#define Anum_pg_aoseg_eof				2
#define Anum_pg_aoseg_tupcount			3
#define Anum_pg_aoseg_varblockcount		4

/*
 * Descriptor of a single AO relation file segment.
 */
typedef struct FileSegInfo
{
	int			segno;			/* the file segment number */
	float8		eof;			/* the effective eof for this segno  */
	float8		tupcount;		/* total number of tuples in this fileseg */
	float8		varblockcount;	/* total number of varblocks in this fileseg */	
} FileSegInfo;

extern FileSegInfo *
GetBlankFileSegInfo(void);

extern void 
InsertFileSegInfo(Relation parentrel, 
				  int segno, 
				  float8 eof,
				  float8 tupcount,
				  float8 varblockcount);

extern FileSegInfo *
GetFileSegInfo(Relation parentrel, int segno);

extern void
UpdateFileSegInfo(Relation parentrel, 
				  int segno,
				  int64 eof, 
				  float8 tuples_added, 
				  float8 varblocks_added);

#endif   /* AOSEGFILES_H */
