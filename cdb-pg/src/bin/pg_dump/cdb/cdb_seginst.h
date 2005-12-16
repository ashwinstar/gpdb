/*-------------------------------------------------------------------------
 *
 * cdb_seginst.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_SEGINST_H
#define CDB_SEGINST_H

typedef enum segment_role
{
	ROLE_SEGDB = 0,
	ROLE_MASTER
} SegRole;

/* --------------------------------------------------------------------------------------------------
 * Structure for segment database information
 *
 * The information contained in this structure represents a row from a join of rows from the tables
 * pg_database, mpp.listener, and mpp.host.
 *
 */
typedef struct segment_database 
{
	int4	dbid;             
    SegRole	role;             /* is it a master database or just any segdb? */        
    int4	port;             
    bool	isValid;          /* true iff cdb_seg.primary_inst == contentid */
    char*	pszHost; 
	char*	pszDBName;          
    char*	pszDBUser;        
    char*	pszDBPswd;         		
} SegmentDatabase;

/*----------------------------------------------------------------------------------------------------
 * This structure is helpful when mpp_dump used --mpp-s=p and therefore generated backup
 * files only on the primary nodes. In that case --mpp-l=p should be used in mpp_restore.
 * This structure will hold the segdb information of the segdb that has the backup
 * file (segdb_source) and the mirror or primary segdb we want to dump it to (segdb_target).
 * when the target segdb is a primary, it will be the same as the source segdb. When --mpp-l
 * is not used, the source and target in each pair will always be the same.
 */
typedef struct restore_pair
{
	SegmentDatabase		segdb_source; /* segdb where backup file is located at */
	SegmentDatabase		segdb_target; /* segdb we want to restore. same as target or its mirror */
	
} RestorePair;

/* --------------------------------------------------------------------------------------------------
 * Structure for an array of segment database pairs
 */
typedef struct segment_database_pair_array 
{
    int					count;	
    SegmentDatabase*	pData;	
} SegmentDatabaseArray;

typedef struct segment_database_restore_pair_array 
{
    int				count;	
    RestorePair*	pData;	
} RestorePairArray;


/* --------------------------------------------------------------------------------------------------
 * Structures for instance segment set specification 
 */
 
/* --------------------------------------------------------------------------------------------------
 * Enum representing the actors of a backup or restore operation. 
 */
typedef enum actor_set
{
	SET_ALL,        /* master, primaries and mirrors */
	SET_NO_MIRRORS  
} ActorSet;

typedef enum backup_file_location
{
	FILE_ON_ALL,
	FILE_ON_PRIMARIES
} BackupLoc;
/* --------------------------------------------------------------------------------------------------
 * Enum representing the definition type of inst and seg
 */
/*
typedef enum cdb_defn_type
{
	DEFN_TYPE_ID,
	DEFN_TYPE_ALL,
	DEFN_TYPE_HOST
} CDBDefnType;
*/
/* --------------------------------------------------------------------------------------------------
 * struct for holding an instance definition.  If the val_type is DEFN_TYPE_ID,
 * then the val_id will be meaningful.  If is's DEFN_TYPE_HOST, then the pszHost and
 * port fields will be meaningful.  If it's DEFN_TYPE_ALL, then no fields will be meaningful.
 */
/*
typedef struct cdb_inst_val
{
	CDBDefnType val_type;
	int			val_id;
	char*		val_pszHost;
	int			val_port;
} CDBInstVal;
*/
/* --------------------------------------------------------------------------------------------------
 * struct for holding a segment definition.  If the val_type is DEFN_TYPE_ID,
 * then the val_id will be meaningful. If it's DEFN_TYPE_ALL, then no fields will be meaningful.
 */
/*
typedef struct cdb_seg_val
{
	CDBDefnType val_type;
	int			val_id;
} CDBSegVal;
*/
/* --------------------------------------------------------------------------------------------------
 * Simple representation for an contentid dbid pair.
 */
/*
typedef struct inst_seg
{
	CDBInstVal	instVal;
	CDBSegVal	segVal;
} InstSeg;
*/
/* --------------------------------------------------------------------------------------------------
 * This holds the complete definition of a instance segment pair set
 */
/*
typedef struct cdb_set
{
	SetType type;
	
	// If type == SET_INDIVIDUAL, the following
	// are used to describe the individual instance segment
	// pairs
	
	
	int			count;
	InstSeg*	pData;
} CDBSet;
*/
	
/* counts how many elements in a set use a string to define the instance, instead of a contentid */
//extern int CountInstanceHosts( CDBSet* pSet ); 
/* Read a control file with instance segment pair info */
//extern bool FillCDBSet( CDBSet* pSet, char* pszControlFile );
/* Evaluate a specific contentid and dbid to see whether it is in the set */ 
//extern bool FindInCDBSet( CDBSet* pSet, int contentid, int dbid ); 
/* Frees the memory allocated for the fields in a CDBSet, but not the CDBSet* itself */ 
//extern void FreeCDBSet( CDBSet* pSet );
/* Free the memory allocated inside the SegmentDatabaseArray.  Doesn't free the pSegDBAr itself. */
extern void FreeSegmentDatabaseArray( SegmentDatabaseArray* pSegDBAr );
extern void FreeRestorePairArray( RestorePairArray* restorePairAr );
/* Read all the rows in the join */
extern bool GetDumpSegmentDatabaseArray(PGconn* pConn, SegmentDatabaseArray* pSegDBAr, ActorSet actors, bool dataOnly, bool schemaOnly );
extern bool GetRestoreSegmentDatabaseArray(PGconn* pConn, RestorePairArray* pSegDBPairAr, BackupLoc backupLocation, bool dataOnly );

#endif /* CDB_SEGINST_H */
