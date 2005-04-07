/*--------------------------------------------------------------------------
*
* cdbcopy.c
*    Rrovides routines that executed a COPY command on a CDB cluster. These
*    routines are called from the backend COPY command whenever CDB is in the 
*    default dispatch mode.
*
* Copyright (c) 2005, Metapa
*
* IDENTIFICATION
*    $Id$
*
*--------------------------------------------------------------------------
*/

#include <assert.h>

#include "cdb/cdbcopy.h"
#include "cdb/cdbconn.h"
/*
 * returns the number of segments in CDB.
 */
int cdbCopyGetNumSegs(void)
{
	return CdbClusterDefinition->segment_count;
}

/*
 * returns the number of segment databases for a given segment.
 */
int cdbCopyGetDbCount(int seg_num)
{
	SegmentDescriptor *p = &(CdbClusterDefinition->segment_descriptors[seg_num]);
	return p->db_count;
}

/*
 * starts a copy command on a specific segment database. 
 * Returns 0 for success, -1 for error and an error message.
 */
int cdbCopyStart(int seg, int db, char *copyCmd)
{
	/*
     * the result of sending the COPY command.
     */
    PGresult *res;

	SegmentDescriptor *p = &(CdbClusterDefinition->segment_descriptors[seg]);
	SegmentDatabaseDescriptor *q = &(p->db_descriptors[db]);
	
	/* send COPY command to the segment DB */
    res = PQexec(q->conn, copyCmd);
	
    if(PQresultStatus(res) != PGRES_COPY_IN)
		return -1;
	
    return 0;
}

/*
 * sends data to a copy command on a specific segment database. 
 * Returns 0 for success, -1 for error and an error message.
 */
int cdbCopySendData(int seg, int db, const char *buffer, int nbytes)
{
	SegmentDescriptor *p = &(CdbClusterDefinition->segment_descriptors[seg]);
	SegmentDatabaseDescriptor *q = &(p->db_descriptors[db]);

	/* transmit the COPY data */
    int result = PQputCopyData(q->conn, buffer, nbytes);
	
	/* if an error occured */
    if (result < 1)
    {
        //if(result == 0)
		//	sprintf(errMsg, "Failed to send COPY data to seg %d db %d, attempt blocked",seg,db);			
        
        //else if(result == -1)
		//	sprintf(errMsg, "An error occured while trying to send COPY data to seg %d db %d: %s",seg,db,PQerrorMessage(q->conn));			
			
	    return -1;        
    }
	
	return 0;
}

/*
 * ends the copy command on a specific segment database. 
 * Returns -1 if the command did not end successfully (I/O error etc). errMsg
 * will include the error msg.
 * Returns 0 if the termination data was successfully sent 
 * Returns 1 if the termination data was successfully sent but there
 * was a data error in the copy command. errMessage will include the error msg.
 */
int cdbCopyEnd(int seg, int db, char *errMessage)
{
	SegmentDescriptor *p = &(CdbClusterDefinition->segment_descriptors[seg]);
	SegmentDatabaseDescriptor *q = &(p->db_descriptors[db]);
 
	int result; /* final result of COPY command execution */
	
    result = PQputCopyEnd(q->conn, NULL); /* second argument HAS to be NULL for operation to pass! */
	
    if (result < 1)
    {
        //if(result == 0)  //IO error status
        //    sprintf(errMsg, "Failed to end COPY command in seg %d db %d, attempt blocked",seg,db);			
		
        //else if(result == -1)  //IO error status
		//	sprintf(errMsg, "An error occured while trying to end COPY command in seg %d db %d",seg,db);			
		
		return -1;
    }
    else //result = 1 and termination data was successfully sent.
    {
        /* Fetch any error status existing at completion of the COPY command */
        PGresult *res = PQgetResult(q->conn);
		
        /* if the COPY command had a data error */
        if ( PQresultStatus(res) == PGRES_FATAL_ERROR )
		{
            sprintf(errMessage, "Error in segment %d db %d -- %s", seg, db, PQresultErrorMessage(res));
		    return 1;
		}
	}
	
	return 0;	
}

/*
 * Execute a transactional command on a segment database.
 */
int cdbCopyExecTrax(int seg, int db, char *sql)
{
	/*assert(sql == "BEGIN" || sql == "ROLLBACK" || sql == "COMMIT");*/
	
	SegmentDescriptor *p = &(CdbClusterDefinition->segment_descriptors[seg]);
	SegmentDatabaseDescriptor *q = &(p->db_descriptors[db]);

    /* the result of sending the sql trax command */
    PGresult *res;
		
    res = PQexec(q->conn, sql);
	
	if(PQresultStatus(res) != PGRES_COMMAND_OK)
        return -1;
	
	return 0;
}
