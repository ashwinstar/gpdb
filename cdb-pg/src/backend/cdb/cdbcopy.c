/*--------------------------------------------------------------------------
*
* cdbcopy.c
*    Rrovides routines that executed a COPY command on a CDB cluster. These
*    routines are called from the backend COPY command whenever CDB is in the 
*    default dispatch mode.
*
* Copyright (c) 2005, Greenplum
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
#include "cdb/cdbtm.h"
#include "cdb/cdbcopy.h"

/*
 * Create a cdbCopy object that includes all the cdb
 * information and state needed by the backend COPY.
 */
CdbCopy *makeCdbCopy(void)
{
	CdbCopy *c;
	int      seg;	
	
	c = palloc(sizeof(CdbCopy));

	/* fresh start */
	c->total_segs = 0;
	c->mirror_map = NULL;
	c->primary_writer = NULL;
	c->mirror_writer = NULL;
	c->data_errors = false;
	c->io_errors = false;
	initStringInfo(&(c->err_msg));

	
	/* init total_segs */
	c->total_segs = getCdbSegmentCount();
	
    if(c->total_segs < 1)
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Got an invalid segment database count of 0.")));
	
    /*
	 * Initialize mirror (secondary) database count for each segment.
	 * mirror_map is indexed by segment number and will contain the
	 * number of databases for this segment (1 if there is only a 
     * primary or 2 if a mirror exists).
	 */
    c->mirror_map = (int *) palloc(c->total_segs * sizeof(int));
	
    for(seg = 0 ; seg < c->total_segs ; seg++)
    {
		c->mirror_map[seg] = cdbCopyGetDbCount(c->total_segs, seg);
	}
	
	/* init gangs */
	c->primary_writer = allocateGang(GANGTYPE_PRIMARY_WRITER, c->total_segs, -1);
	c->mirror_writer = allocateGang(GANGTYPE_MIRROR_WRITER, c->total_segs, -1);
	
	return c;	
}

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
int cdbCopyStart(CdbCopy *c, char *copyCmd)
{
    PGresult *res;
    SegmentDatabaseDescriptor *q;
    Gang * gp;
	int    seg;
	int    db;
	
	for(seg = 0 ; seg < c->total_segs ; seg++)
	{
		for(db = 0 ; db < c->mirror_map[seg] ; db++)
		{
			if (db == 0)
				gp = c->primary_writer;
			else
				gp = c->mirror_writer;			
			
			q = &(gp->db_descriptors[seg]);
			
			/* send COPY command to the segment DB */
			res = PQexec(q->conn, copyCmd);
			
			if(PQresultStatus(res) != PGRES_COPY_IN)
				/* TODO: try to end COPY in prev segs */
				/*       before returning error code  */
				return -1;
			
			PQclear(res);
		}
	}
	
    return 0;
}

/*
 * sends data to a copy command on a specific segment (usually
 * the hash result of the data value).
 */
void cdbCopySendData(CdbCopy *c, int target_seg, const char *buffer, 
					int nbytes)
{
	SegmentDatabaseDescriptor *q;
	Gang  *gp;
	int    db;
	int    result;

	/* clean err message */
	c->err_msg.len = 0;		
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
	
	for(db = 0 ; db < c->mirror_map[target_seg] ; db++)
	{
		
		if (db == 0)
			gp = c->primary_writer;
		else
			gp = c->mirror_writer;			
		
		q = &(gp->db_descriptors[target_seg]);
		
		/* transmit the COPY data */
		result = PQputCopyData(q->conn, buffer, nbytes);
		
		if(result != 1)
		{
			if(result == 0)
				appendStringInfo(&(c->err_msg), 
								 "\n\nCDBCOPY ERROR: Failed to send data to segment %d db %d, attempt blocked", 
								 target_seg, db);
				
			if(result == -1)
				appendStringInfo(&(c->err_msg), 
								 "\n\nCDBCOPY ERROR: Failed to send data to segment %d db %d: %s", 
								 target_seg, db, PQerrorMessage(q->conn));
			
			c->io_errors = true;
		}
	}
	
	
}

/*
 * ends the copy command on all segment databases. 
 * Returns -1 if the command did not end successfully (I/O error etc). 
 * c->err_msg will include the error msg(s) if any.
 */
void cdbCopyEnd(CdbCopy *c)
{
	SegmentDatabaseDescriptor *q;
	Gang     *gp;
	PGresult *res;
	int       result; /* final result of COPY command execution */
	int       seg;
	int       db;
	
	/* clean err message */
	c->err_msg.len = 0;		
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
	
	for(seg = 0 ; seg < c->total_segs ; seg++)
	{
		for(db = 0 ; db < c->mirror_map[seg] ; db++)
		{
			
			if (db == 0)
				gp = c->primary_writer;
			else
				gp = c->mirror_writer;			
	
			q = &(gp->db_descriptors[seg]);
			
			/* end this COPY command */
			result = PQputCopyEnd(q->conn, NULL);
			
			
			/* get command end status */
			if(result == 0) 
			{
				/* attempt blocked */
				appendStringInfo(&(c->err_msg), 
								 "\n\nCDBCOPY ERROR: Failed to end COPY command in seg %d db %d, attempt blocked", 
								 seg, db);
				c->io_errors = true;
			}
			else if(result == -1)  
			{
				/* command error */
				appendStringInfo(&(c->err_msg), 
								 "\n\nCDBCOPY ERROR: Reporting segment %d db %d: %s", 
								 seg, db, PQerrorMessage(q->conn));
				c->io_errors = true;
			}	 
			else if(result == 1) 
			{
				/* termination data successfully sent */
				
			   /*
				* Fetch any error status existing on completion of the COPY command.
				* It is critical that for any connection that had an asynchronous
				* command sent thru it, we call PQgetResult until it returns NULL.
				* Otherwise, the next time a command is sent to that connection,
				* it will return an error that there's a command pending.
				*/		
				while ( NULL != (res = PQgetResult(q->conn) ) )
				{
					/* if the COPY command had a data error */
					if ( PQresultStatus(res) == PGRES_FATAL_ERROR )
					{
						appendStringInfo(&(c->err_msg), "\n\nCDBCOPY ERROR: Reporting segment %d db %d: %s", 
										 seg, db, PQresultErrorMessage(res));
						c->data_errors = true;
					}
				}
				
				PQclear(res);				
			}
		}
	}
}


/*
 * start a global transaction for COPY.
 */
void cdbCopyStartTransaction(void)
{
    startDtxTransaction();
}
