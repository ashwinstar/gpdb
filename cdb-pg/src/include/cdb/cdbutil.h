/*-------------------------------------------------------------------------
 *
 * cdbutil.h
 * 	  Header file for routines in cdbutil.c and results returned by
 * 	  those routines.
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBUTIL_H
#define CDBUTIL_H

#include "c.h"

/* --------------------------------------------------------------------------------------------------
 * Structure for cdb 2.0 segment database information
 *
 * The information contained in this structure represents logically a row from  
 * cdb.cdb_get_segdbs, cdb.cdb
 *
 * Storage for instances of this structure are palloc'd.  Storage for the values
 * pointed to by non-NULL char*'s are also palloc'd.
 *
 */
typedef struct segment_database_info 
{
	int4		segindex;		// segindex of this segment
    bool		isprimary;  	// true if this is the primary segment, false if it's the secondary
    char		*dbname; 		// segment database name
    char		*hostname;  	// name or ip address of host machine
    int4		port;     		// port that instance is listening on
    char		*datadir;   	// directory where instance was initialized into
    bool		isvalid;    	// flag that tells whether the database is valid
    char		*cdbsuperuser;	// user name for cdb connection
    char		*cdbsuperpswd;	// password for cdb connection
} SegmentDatabaseInfo;

/* --------------------------------------------------------------------------------------------------
 * Structure for return value from getCdbSegmentDatabases()
 *
 * The storage for instances of this structure returned by getCdbSegmentInstances() is palloc'd.
 * freeCdbSegmentDatabases() can be used to release the structure.
 */
typedef struct 
{
    SegmentDatabaseInfo		*segment_db_info; 			// array of SegmentDatabaseInfo's
    int						total_segment_dbs;			// count of the array 
    int						total_segments;				// count of distinct segindexes
    int						my_segindex;				// the segindex of this database
    bool					my_isprimary;				// the isprimary flag of this database
} SegmentDatabases;


/*
 * Server function declarations
 */

/*
 * getCdbSegmentInstances() returns a pointer to a SegmentDatabases
 * structure. The array of SegmentDatabaseInfo's is ordered by segindex, 
 * isprimary desc.   
 * 
 * The SegmentDatabases structure, and the segment_databases array 
 * are contained in palloc'd storage allocated from the current storage context.  
 * The same is true for pointer-based values in SegmentDatabaseInfo.  The caller is responsible 
 * for setting the current storage context and releasing the storage  occupied the returned values.
 * 
 * The function freeCdbSegmentDatabases() is used to release the structure
 * returned by getCdbSegmentDatabases().
 *
 */
extern SegmentDatabases * getCdbSegmentDatabases(void);

/*
 * freeCdbSegmentDatabasess() releases the palloc'd storage returned by
 * getCdbSegmentDatabases().
 */
extern void freeCdbSegmentDatabases(SegmentDatabases *pSegDBs);

#endif   /* CDBUTIL_H */
