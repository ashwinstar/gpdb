/*-------------------------------------------------------------------------
 *
 * cdb_table.h
 *
 * Structures and functions to read the cbd tables using libpq
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_TABLE_H
#define CDB_TABLE_H

/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_assignment
 */
/*
typedef struct cdb_assignment 
{
    int		tbloid;
    char*	pszTableName;
    char*	pszNspName;
	int		partno;
	int		segid;
} CDBAssignment;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBAssignments
 */
/*
typedef struct cdb_assignment_array 
{
    int				count;		// count
    CDBAssignment*	pData;		// array of count CDBAssignments
} CDBAssignmentArray;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_basetable
 */

typedef struct cdb_basetable 
{
    int		tbloid;
    char*	pszTableName;
    char*	pszNspName;
	bool	partitioned;
} CDBBaseTable;

/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBBaseTables
 */

typedef struct cdb_basetable_array 
{
    int				count;		// count
    CDBBaseTable*	pData;		// array of count CDBBaseTable
} CDBBaseTableArray;

/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_instance
 */
/*
typedef struct cdb_instance 
{
    int		instid;			
    bool	valid;
    int		port;
	char*	pszHost;
	char*	pszDataDir;
} CDBInstance;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBInstances
 */
/*
typedef struct cdb_instance_array 
{
    int				count;		// count
    CDBInstance*	pData;		// array of count CDBInstances
} CDBInstanceArray;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_policy
 */
/*
typedef struct cdb_policy 
{
    int		tbloid;
    char*	pszTableName;
    char*	pszNspName;
	int		seqno;
	int		attnum;
	int		nparts;
	char	type;
	char*	pszParameters;
} CDBPolicy;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBPolicies
 */
/*
typedef struct cdb_policy_array 
{
    int			count;		// count
    CDBPolicy*	pData;		// array of count CDBPolicies
} CDBPolicyArray;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_seg
 */
/*
typedef struct cdb_seg 
{
    int		segid;			
    int		primary_inst;			
    bool	valid;
	char*	pszDBName;
	char*	pszDBUser;
	char*	pszDBPswd;
} CDBSeg;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBSegs
 */
/*
typedef struct cdb_seg_array 
{
    int		count;		// count
    CDBSeg*	pData;		// array of count CDBSegs
} CDBSegArray;
*/
/* --------------------------------------------------------------------------------------------------
 * Structure for reading cdb_segment_instance
 */
typedef struct cdb_segment_instance 
{
    int		segid;
    bool     isValid;			
    char    *dbname;
    char    *dbhostname;
    int      dbport;
    char    *dbdatadir;
    char    *dbsuperuser;			
} CDBSegmentInstance;

/* --------------------------------------------------------------------------------------------------
 * Structure for an array of CDBSegmentInstances
 */
typedef struct cdb_segment_instance_array 
{
    int					count;		// count
    CDBSegmentInstance*	pData;		// array of count CDBSegmentInstances
} CDBSegmentInstanceArray;


/* Free the memory allocated inside the CDBAssignmentArray.  Doesn't free the pAssignmentAr itself. */
//extern void FreeCDBAssignmentArray( CDBAssignmentArray* pAssignmentAr );
/* Free the memory allocated inside the CDBBaseTableArray.  Doesn't free the pTableAr itself. */
extern void FreeCDBBaseTableArray( CDBBaseTableArray* pTableAr );
/* Free the memory allocated inside the CDBInstanceArray.  Doesn't free the pInstanceAr itself. */
//extern void FreeCDBInstanceArray( CDBInstanceArray* pInstanceAr );
/* Free the memory allocated inside the CDBPolicyArray.  Doesn't free the pPolicyAr itself. */
//extern void FreeCDBPolicyArray( CDBPolicyArray* pPolicyAr );
/* Free the memory allocated inside the CDBSegArray.  Doesn't free the pSegAr itself. */
//extern void FreeCDBSegArray( CDBSegArray* pSegAr );
/* Free the memory allocated inside the CDBSegmentInstanceArray.  Doesn't free the pSegmentInstanceAr itself. */
extern void FreeCDBSegmentInstanceArray( CDBSegmentInstanceArray* pSegmentInstanceAr );
/* Read all the rows in the join of cdb_assignment, pg_class, and pg_namespace */
//extern bool GetCDBAssignmentArray(PGconn* pConn, const char* pszDBName, CDBAssignmentArray* pAssignmentAr );
/* Read all the rows in the join of cdb_basetable, pg_class, and pg_namespace */
extern bool GetCDBBaseTableArray(PGconn* pConn, const char* pszDBName, CDBBaseTableArray* pTableAr, bool* pbPartitioned );
/* Read all the rows in the cdb_instance table */
//extern bool GetCDBInstanceArray(PGconn* pConn, const char* pszDBName, CDBInstanceArray* pInstanceAr );
/* Read all the rows in the join of cdb_policy, pg_class, and pg_namespace */
//extern bool GetCDBPolicyArray(PGconn* pConn, const char* pszDBName, CDBPolicyArray* pPolicyAr );
/* Read all the rows in the cdb_seg table */
//extern bool GetCDBSegArray(PGconn* pConn, const char* pszDBName, CDBSegArray* pSegAr );
/* Read all the rows in the cdb_segment_instance table */
extern bool GetCDBSegmentInstanceArray(PGconn* pConn, const char* pszDBName, CDBSegmentInstanceArray* pSegmentInstanceAr );

#endif /* CDB_TABLE_H */
