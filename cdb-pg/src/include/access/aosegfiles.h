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

#include "utils/rel.h"

#define Natts_pg_aoseg					4
#define Anum_pg_aoseg_segno				1
#define Anum_pg_aoseg_eof				2
#define Anum_pg_aoseg_tupcount			3
#define Anum_pg_aoseg_varblockcount		4

#define InvalidFileSegNumber			-1

/*
 * GUC variables
 */
extern int MaxAppendOnlyTables;		/* Max # of tables */

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

/*
 * Structure that sums up the field total of all file 'segments'.
 * Note that even though we could actually use FileSegInfo for 
 * this purpose we choose not too since it's likely that FileSegInfo
 * will go back to using int instead of float8 now that each segment
 * has a size limit.
 */
typedef struct FileSegTotals
{
	int			totalfilesegs;  /* total number of file segments */
	float8		totalbytes;		/* the sum of all 'eof' values  */
	float8		totaltuples;	/* the sum of all 'tupcount' values */
	float8		totalvarblocks;	/* the sum of all 'varblockcount' values */	
} FileSegTotals;

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

extern FileSegInfo *
GetAllFileSegInfo(Relation parentrel, int *totalsegs);

extern void
UpdateFileSegInfo(Relation parentrel, 
				  int segno,
				  int64 eof, 
				  float8 tuples_added, 
				  float8 varblocks_added);

extern FileSegTotals *
GetSegFilesTotals(Relation parentrel);

extern int 
gp_update_ao_master_stats__(PG_FUNCTION_ARGS);

#endif   /* AOSEGFILES_H */
