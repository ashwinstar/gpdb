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

#include "postgres.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "cdb/cdblink.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbcopy.h"



/*
 * returns the number of segment databases for a given segment.
 */
int cdbCopyGetDbCount(int total_segs, int seg)
{
	Gang * gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1);
	
	if (gp != NULL && &(gp->db_descriptors[seg]) != NULL)
		return 2;
	else
		return 1;
}

/*
 * starts a copy command on a specific segment database. 
 * Returns 0 for success, -1 for error and an error message.
 */
int cdbCopyStart(int total_segs, int seg, int db, char *copyCmd)
{
	/*
     * the result of sending the COPY command.
     */
    PGresult *res;
    SegmentDatabaseDescriptor *q;

	Gang * gp;
	if (db==0)
		gp = allocateGang(GANGTYPE_PRIMARY_WRITER, total_segs, -1);
	else
		gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1);
	
	q = &(gp->db_descriptors[seg]);
	
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
int cdbCopySendData(int total_segs, int seg, int db, const char *buffer, int nbytes)
{
	SegmentDatabaseDescriptor *q;
	Gang * gp;
	int result;
	if (db==0)
		gp = allocateGang(GANGTYPE_PRIMARY_WRITER, total_segs, -1);
	else
		gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1);
	
	q = &(gp->db_descriptors[seg]);

	/* transmit the COPY data */
    result = PQputCopyData(q->conn, buffer, nbytes);
	
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
 * Returns -1 if the command did not end successfully (I/O error etc). errMessage
 * will include the error msg.
 * Returns 0 if the termination data was successfully sent 
 * Returns 1 if the termination data was successfully sent but there
 * was a data error in the copy command. errMessage will include the error msg.
 */
int cdbCopyEnd(int total_segs, int seg, int db, StringInfoData errMessage)
{
	SegmentDatabaseDescriptor *q;
	Gang * gp;
	int result; /* final result of COPY command execution */
	
	if (db==0)
		gp = allocateGang(GANGTYPE_PRIMARY_WRITER, total_segs, -1);
	else
		gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1);
	
	q = &(gp->db_descriptors[seg]);
 	
	/*
	 * End this COPY command.
	 */
    result = PQputCopyEnd(q->conn, NULL);
	
    if (result < 1)
    {
        /*
		 if(result == 0)  //IO error status
		 sprintf(errMsg, "Failed to end COPY command in seg %d db %d, attempt blocked",seg,db);			
		 
		 else if(result == -1)  //IO error status
		 sprintf(errMsg, "An error occured while trying to end COPY command in seg %d db %d",seg,db);			
		 */
		return -1;
    }
    else //result = 1 and termination data was successfully sent.
    {
        /* Fetch any error status existing at completion of the COPY command */
        PGresult *res = PQgetResult(q->conn);
		
        /* if the COPY command had a data error */
        if ( PQresultStatus(res) == PGRES_FATAL_ERROR )
		{
            appendStringInfo(&errMessage, "Reporting segment %d db %d: %s", seg, db, PQresultErrorMessage(res));
		    return 1;
		}
	}
	
	return 0;	
}


/*
 * Execute a transactional command on a segment database.
 */
int cdbCopyExecTrax(int total_segs, int seg, int db, char *sql)
{
	/*assert(sql == "BEGIN" || sql == "ROLLBACK" || sql == "COMMIT");*/
	
	/* the result of sending the sql trax command */
    PGresult *res;
	SegmentDatabaseDescriptor *q;
	Gang * gp;
	if (db==0)
		gp = allocateGang(GANGTYPE_PRIMARY_WRITER, total_segs, -1);
	else
		gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1);
	
	q = &(gp->db_descriptors[seg]);

    
		
    res = PQexec(q->conn, sql);
	
	if(PQresultStatus(res) != PGRES_COMMAND_OK)
        return -1;
	
	return 0;
}
