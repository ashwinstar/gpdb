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
 * Structure for cdb 2.0 database information
 *
 * The information contained in this structure represents logically a row from  
 * cdb.component_database
 *
 * Storage for instances of this structure are palloc'd.  Storage for the values
 * pointed to by non-NULL char*'s are also palloc'd.
 *
 */
typedef struct cdb_component_database_info 
{
	int4		dbid;			// the dbid of this database
	int4		segindex;		// content indicator: -1 for entry database, 0, ..., n-1 for segment database
    bool		isprimary;  	// true if this is the primary segment, false if it's the secondary
    char		*dbname; 		// segment database name
    char		*hostname;  	// name or ip address of host machine
    int4		port;     		// port that instance is listening on
    char		*datadir;   	// directory where instance was initialized into
    int4		listenerid;		// id of listener for this database.
    char		*cdbsuperuser;	// user name for cdb connection
    char		*cdbsuperpswd;	// password for cdb connection
} CdbComponentDatabaseInfo;

/* --------------------------------------------------------------------------------------------------
 * Structure for return value from getCdbSegmentDatabases()
 *
 * The storage for instances of this structure returned by getCdbSegmentInstances() is palloc'd.
 * freeCdbSegmentDatabases() can be used to release the structure.
 */
typedef struct CdbComponentDatabases
{
    CdbComponentDatabaseInfo	*segment_db_info; 			// array of SegmentDatabaseInfo's for segment databases
    int							total_segment_dbs;			// count of the array 
    CdbComponentDatabaseInfo	*entry_db_info; 			// array of SegmentDatabaseInfo's for entry databases
    int							total_entry_dbs;			// count of the array 
    int							total_segments;				// count of distinct segindexes
    int							my_dbid;					// the dbid of this database
    int							my_segindex;				// the content of this database
    bool						my_isprimary;				// the isprimary flag of this database
} CdbComponentDatabases;

/* --------------------------------------------------------------------------------------------------
 * Structure for row of cdb.cdb table
 *
 * The storage for instances of this structure returned by getCdbSegmentInstances() is palloc'd.
 * freeCdbSegmentDatabases() can be used to release the structure.
 */
typedef struct CdbCdb
{
    char	*cdbname; 		// array of SegmentDatabaseInfo's for segment databases
    int4	numsegments;	// count of distinct segindexes
	int4	dbid;			// the dbid of this database
	int4	segindex;		// content indicator: -1 for entry database, 0, ..., n-1 for segment database
} CdbCdb;

/*
 * Server function declarations
 */

/*
 * performs all necessary setup required for initializing CDB components.
 *
 * This includes cdblink_setup() and initializing the Motion Layer.
 *
 */
extern void cdb_setup(void);


/*
 * performs all necessary cleanup required when cleaning up CDB components
 * when disabling CDB functionality.
 *
 */
extern void cdb_cleanup(void);


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
extern CdbComponentDatabases *getCdbComponentDatabases(void);

/*
 * freeCdbSegmentDatabasess() releases the palloc'd storage returned by
 * getCdbSegmentDatabases().
 */
extern void freeCdbComponentDatabases(CdbComponentDatabases *pDBs);

extern void getCdbCdb(void);
extern void freeCdbCdb(void);

extern CdbCdb CdbIdentity;

#endif   /* CDBUTIL_H */
