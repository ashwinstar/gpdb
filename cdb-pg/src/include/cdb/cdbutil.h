/*-------------------------------------------------------------------------
 *
 * cdbutil.h
 *	  Header file for routines in cdbutil.c and results returned by
 *	  those routines.
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBUTIL_H
#define CDBUTIL_H

#include "c.h"

/* --------------------------------------------------------------------------------------------------
 * Structure for MPP 2.0 database information
 *
 * The information contained in this structure represents logically a row from
 * gp_configuration.  It is used to describe either an entry
 * database or a segment database.
 *
 * Storage for instances of this structure are palloc'd.  Storage for the values
 * pointed to by non-NULL char*'s are also palloc'd.
 *
 */
typedef struct CdbComponentDatabaseInfo
{
	int2		dbid;			/* the dbid of this database */
	int2		segindex;		/* content indicator: -1 for entry database,
								 * 0, ..., n-1 for segment database */
	bool		isprimary;		/* true if this is the primary segment, false
								 * if it's the secondary */
	bool		valid;			/* true if this segdb is up */
	bool		definedprimary; /* true if this seg db is originally defined
								 * as primary */
	char	   *hostname;		/* name or ip address of host machine */
	char	   *hostip;			/* cached lookup of name */
	int4		port;			/* port that instance is listening on */
	char	   *datadir;		/* directory where instance was initialized
								 * into */
	
} CdbComponentDatabaseInfo;


/* --------------------------------------------------------------------------------------------------
 * Structure for return value from getCdbSegmentDatabases()
 *
 * The storage for instances of this structure returned by getCdbSegmentInstances() is palloc'd.
 * freeCdbSegmentDatabases() can be used to release the structure.
 */
typedef struct CdbComponentDatabases
{
	CdbComponentDatabaseInfo *segment_db_info;	/* array of
												 * SegmentDatabaseInfo's for
												 * segment databases */
	int			total_segment_dbs;		/* count of the array  */
	CdbComponentDatabaseInfo *entry_db_info;	/* array of
												 * SegmentDatabaseInfo's for
												 * entry databases */
	int			total_entry_dbs;	/* count of the array  */
	int			total_segments; /* count of distinct segindexes */
	int			my_dbid;		/* the dbid of this database */
	int			my_segindex;	/* the content of this database */
	bool		my_isprimary;	/* the isprimary flag of this database */
} CdbComponentDatabases;

/*
 * performs all necessary setup required for initializing Greenplum Database components.
 *
 * This includes cdblink_setup() and initializing the Motion Layer.
 *
 */
extern void cdb_setup(void);


/*
 * performs all necessary cleanup required when cleaning up Greenplum Database components
 * when disabling Greenplum Database functionality.
 *
 */
extern void cdb_cleanup(int code, Datum arg);


/*
 * getCdbComponentDatabases() returns a pointer to a CdbComponentDatabases
 * structure. Both the segment_db_info array and the entry_db_info_array are ordered by segindex,
 * isprimary desc.
 *
 * The CdbComponentDatabases structure, and the segment_db_info array and the entry_db_info_array
 * are contained in palloc'd storage allocated from the current storage context.
 * The same is true for pointer-based values in CdbComponentDatabaseInfo.  The caller is responsible
 * for setting the current storage context and releasing the storage occupied the returned values.
 *
 * The function freeCdbComponentDatabases() is used to release the structure
 * returned by getCdbComponentDatabases().
 *
 */
extern CdbComponentDatabases *getCdbComponentDatabases(void);

/*
 * freeCdbComponentDatabases() releases the palloc'd storage returned by
 * getCdbComponentDatabases().
 */
extern void freeCdbComponentDatabases(CdbComponentDatabases *pDBs);

/*
 * getGpId() causes the gp_id table's singleton row to be read into a global structure
 * This is called from cdblink_setup().
 * The global variable is declared in cdbvars.h and defined in cdbvars.c.
 */
extern void getGpId(void);

/*
 * freeGpId() causes the global variable holding the gp_id table's singleton row to be
 * reinitialized to empty, releasing memory that was allocated when it was loaded.
 */
extern void freeGpId(void);

// UNDONE: This was a private procedure... are there any issues in making it public???
/*
 * getCdbComponentDatabases
 *
 *
 * Storage for the SegmentInstances block and all subsidiary
 * strucures are allocated from the caller's context.
 */
extern CdbComponentDatabases *getCdbComponentInfo(void);

/*
 * Given total number of primary segment databases and a number of segments
 * to "skip" - this routine creates a boolean map (array) the size of total
 * number of segments and randomly selects several entries (total number of
 * total_to_skip) to be marked as "skipped". This is used for external tables
 * with the 'gpfdist' protocol where we want to get a number of *random* segdbs
 * to connect to a gpfdist client.
 */
extern bool *makeRandomSegMap(int total_primaries, int total_to_skip);


#endif   /* CDBUTIL_H */
