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
 * Maximum concurrent number of writes into a single append only table.
 * TODO: may want to make this a guc instead (can only be set at gpinit time).
 */
#define MAX_AOREL_CONCURRENCY 128

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

/*
 * Describes the status of a particular file segment number accross an entire
 * AO relation. 
 *
 * This information is kept in a hash table and kept up to date. It represents
 * the global status of this segment number accross the whole array, for example
 * 'tupcount' for segno #4 will show the total rows in all file segments with
 * number 4 for this specific table. 
 * 
 * This data structure is accessible by the master node and it is used to make 
 * decisions regarding file segment allocations for data writes. Note that it 
 * is not used for reads. Durind reads each segdb scanner reads its local 
 * catalog.
 *
 * Note that 'isfull' tries to guess if any of the file segments is full. Since
 * there may be some skew in the data we use a threshold that is a bit lower
 * than the max tuples allowed per segment.
 */
typedef struct AOSegfileStatus
{
	float8		tupcount;  /* Total tupcount for this segno across all segdbs */
	bool		inuse;	   /* if true - segno is assigned right now. use another */
	bool		isfull;	   /* if true - never insert into this segno anymore */
} AOSegfileStatus;

/*
 * Describes the status of all file segments of all AO relations in the system.
 * This data structure is kept in a hash table on the master and kept up to 
 * date.
 */
typedef struct AORelHashEntry
{
	Oid				relid;
	AOSegfileStatus relsegfiles[MAX_AOREL_CONCURRENCY];
	
} AORelHashEntry;

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
GetAllFileSegInfo(Relation parentrel, int totalsegs);

extern void
UpdateFileSegInfo(Relation parentrel, 
				  int segno,
				  int64 eof, 
				  float8 tuples_added, 
				  float8 varblocks_added);

extern int 
GetNumOfSegFiles(Relation parentrel);

extern FileSegTotals *
GetSegFilesTotals(Relation parentrel);

extern void 
UpdateMasterAosegTotals(Relation parentrel, 
						int segno, 
						uint64 tupcount);

extern int 
SetSegnoForWrite(int existingsegno);

extern bool
AOHashTableInit(void);

#endif   /* AOSEGFILES_H */
