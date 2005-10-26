/*-------------------------------------------------------------------------
 *
 * cdb_seginst.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_SEGINST_H
#define CDB_SEGINST_H
 
/* --------------------------------------------------------------------------------------------------
 * Structure for segment/instance pair information
 *
 * The information contained in this structure represents a row from a join of rows from the tables
 * cdb_seg, mpp_segment_instance, and cdb_instance.
 *
 */
typedef struct segment_instance 
{
	int4	segid;              // segment id for this pair
    int4	instid;             // instance id for this pair
    int4	port;               // copy of cdb_instance.port
    bool	isValid;          // true iff cdb_seg.primary_inst == instid
    char*	pszHost;            // copy of cdb_instance.host
    char*	pszDBName;          // copy of cdb_seg.dbname
} SegmentInstance;

/* --------------------------------------------------------------------------------------------------
 * Structure for an array of segment/instance pairs
 */
typedef struct segment_instance_pair_array 
{
    int					count;	// count
    SegmentInstance*	pData;	// array of count SegmentInstancePairs
} SegmentInstanceArray;


/* --------------------------------------------------------------------------------------------------
 * Structures for instance segment set specification 
 */
 
/* --------------------------------------------------------------------------------------------------
 * Enum representing the set definition type of a backup or restore operation. 
 */
typedef enum set_type
{
	SET_ALL,
	SET_TAILS_ONLY,
	SET_HEAD_ONLY,
	SET_INDIVIDUAL,
} SetType;

/* --------------------------------------------------------------------------------------------------
 * Enum representing the definition type of inst and seg
 */
typedef enum cdb_defn_type
{
	DEFN_TYPE_ID,
	DEFN_TYPE_ALL,
	DEFN_TYPE_HOST
} CDBDefnType;

/* --------------------------------------------------------------------------------------------------
 * struct for holding an instance definition.  If the val_type is DEFN_TYPE_ID,
 * then the val_id will be meaningful.  If is's DEFN_TYPE_HOST, then the pszHost and
 * port fields will be meaningful.  If it's DEFN_TYPE_ALL, then no fields will be meaningful.
 */
typedef struct cdb_inst_val
{
	CDBDefnType val_type;
	int			val_id;
	char*		val_pszHost;
	int			val_port;
} CDBInstVal;

/* --------------------------------------------------------------------------------------------------
 * struct for holding a segment definition.  If the val_type is DEFN_TYPE_ID,
 * then the val_id will be meaningful. If it's DEFN_TYPE_ALL, then no fields will be meaningful.
 */
typedef struct cdb_seg_val
{
	CDBDefnType val_type;
	int			val_id;
} CDBSegVal;

/* --------------------------------------------------------------------------------------------------
 * Simple representation for an instid segid pair.
 */
typedef struct inst_seg
{
	CDBInstVal	instVal;
	CDBSegVal	segVal;
} InstSeg;

/* --------------------------------------------------------------------------------------------------
 * This holds the complete definition of a instance segment pair set
 */
typedef struct cdb_set
{
	SetType type;
	
	/* If type == SET_INDIVIDUAL, the following
	* are used to describe the individual instance segment
	* pairs
	*/
	
	int			count;
	InstSeg*	pData;
} CDBSet;

/* counts how many elements in a set use a string to define the instance, instead of an instid */
extern int CountInstanceHosts( CDBSet* pSet ); 
/* Read a control file with instance segment pair info */
extern bool FillCDBSet( CDBSet* pSet, char* pszControlFile );
/* Evaluate a specific instid and segid to see whether it is in the set */ 
extern bool FindInCDBSet( CDBSet* pSet, int instID, int segID ); 
/* Frees the memory allocated for the fields in a CDBSet, but not the CDBSet* itself */ 
extern void FreeCDBSet( CDBSet* pSet );
/* Free the memory allocated inside the SegmentInstanceArray.  Doesn't free the pSegInstAr itself. */
extern void FreeSegmentInstanceArray( SegmentInstanceArray* pSegInstAr );
/* Read all the rows in the join */
extern bool GetSegmentInstanceArray(PGconn* pConn, char* pszDBName, CDBSet* pSet, SegmentInstanceArray* pSegInstAr, bool bExcludeHead );

#endif /* CDB_SEGINST_H */
