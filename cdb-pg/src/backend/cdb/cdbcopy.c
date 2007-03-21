/*--------------------------------------------------------------------------
*
* cdbcopy.c
*	 Rrovides routines that executed a COPY command on an MPP cluster. These
*	 routines are called from the backend COPY command whenever MPP is in the
*	 default dispatch mode.
*
* Copyright (c) 2005, Greenplum
*
* IDENTIFICATION
*	 $Id$
*
*--------------------------------------------------------------------------
*/

#include "postgres.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "cdb/cdblink.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbcopy.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbdispatchresult.h"


/*
 * Create a cdbCopy object that includes all the cdb
 * information and state needed by the backend COPY.
 */
CdbCopy *
makeCdbCopy(bool is_copy_in)
{
	CdbCopy    *c;
	int			seg;

	c = palloc(sizeof(CdbCopy));

	/* fresh start */
	c->total_segs = 0;
	c->mirror_map = NULL;
	c->primary_writer = NULL;
	c->mirror_writer = NULL;
	c->local_data_err = false;
	c->remote_data_err = false;	
	c->io_errors = false;
	c->copy_in = is_copy_in;
	c->outseglist = NIL;
	initStringInfo(&(c->err_msg));
	initStringInfo(&(c->err_context));
	initStringInfo(&(c->copy_out_buf));	
	
	/* init total_segs */
	c->total_segs = getgpsegmentCount();

	if (c->total_segs < 1)
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("MPPCOPY - Got an invalid segment database count of 0.")));

	/*
	 * Initialize mirror (secondary) database count for each segment.
	 * mirror_map is indexed by segment number and will contain the
	 * number of databases for this segment (1 if there is only a
	 * primary or 2 if a mirror exists).
	 */
	c->mirror_map = (int *) palloc(c->total_segs * sizeof(int));

	for (seg = 0; seg < c->total_segs; seg++)
	{
		c->mirror_map[seg] = cdbCopyGetDbCount(c->total_segs, seg);
	}

	/* Initialize the state of each segment database */
	c->segdb_state = (SegDbState **) palloc((c->total_segs) * sizeof(SegDbState *));

	for (seg = 0; seg < c->total_segs; seg++)
	{
		c->segdb_state[seg] = (SegDbState *) palloc(2 * sizeof(SegDbState));
		c->segdb_state[seg][0] = SEGDB_IDLE;	/* Primary can't be OUT */
		
		/* we are interested in mirrors only for copy_in */
		if(c->copy_in)
			c->segdb_state[seg][1] = (c->mirror_map[seg] == 1 ? SEGDB_OUT : SEGDB_IDLE);
		else
			c->segdb_state[seg][1] = SEGDB_OUT;
	}

	/* init gangs */
	c->primary_writer = allocateGang(GANGTYPE_PRIMARY_WRITER, c->total_segs, -1, NULL);
	c->mirror_writer = allocateGang(GANGTYPE_MIRROR_WRITER, c->total_segs, -1, NULL);
	
	/* init seg list for copy out */
	if(!c->copy_in)
	{
		int i;
		for (i = 0; i < c->total_segs; i++)
			c->outseglist = lappend_int(c->outseglist, i);
	}

	return c;
}

/*
 * returns the number of segment databases for a given segment.
 */
int
cdbCopyGetDbCount(int total_segs, int seg)
{
	int			i;

	Gang	   *gp = allocateGang(GANGTYPE_MIRROR_WRITER, total_segs, -1, NULL);

	if (gp == NULL)
		return 1;

	for (i = 0; i < gp->size; i++)
	{
		if (gp->db_descriptors[i].segindex == seg)
			return 2;
	}

	return 1;
}


/*
 * starts a copy command on a specific segment database.
 * Returns 0 for success, -1 for error and an error message.
 */
int
cdbCopyStart(CdbCopy *c, char *copyCmd)
{
	PGresult   *res;
	SegmentDatabaseDescriptor *q;
	Gang	   *gp;
	int			seg;
	int			db;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;
	
	for (seg = 0; seg < c->total_segs; seg++)
	{
		for (db = 0; db < c->mirror_map[seg]; db++)
		{
			if (db == 0)
				gp = c->primary_writer;
			else
			{
				gp = c->mirror_writer;
				
				/* skip mirror if COPY OUT */
				if(!c->copy_in)
					continue;
			}
			
			q = getSegmentDescriptorFromGang(gp, seg);

			/* send COPY command to the segment DB */
			res = PQexec(q->conn, copyCmd);

			if ( (c->copy_in && (PQresultStatus(res) != PGRES_COPY_IN)) ||
				 (!c->copy_in && (PQresultStatus(res) != PGRES_COPY_OUT)) )
			{
				char   *pri = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
				char   *dtl = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
				char   *ctx = PQresultErrorField(res, PG_DIAG_CONTEXT);
				
				appendStringInfo(&(c->err_msg), "Failed to start COPY - %s", pri);
				
				if (q->whoami)
					appendStringInfo(&(c->err_msg),"  (%s)", q->whoami);
				
				if (dtl)
					appendStringInfo(&(c->err_msg), "\n%s", dtl);
				
				/*
				 * note that due to cdb_tidy_message() in elog.c "If more than 
				 * one line, move lines after the first to errdetail" so we save
				 * the context in another stringInfo and fetch it in the error
				 * callback in copy.c, so it wouldn't appear as DETAIL...
				 */
				if (ctx)
					appendStringInfo(&(c->err_context), "%s", ctx);
					
				PQclear(res);
				
				return -1;
			}
			else
				c->segdb_state[seg][db] = SEGDB_COPY;	/* we be jammin! */

			PQclear(res);
		}

	}

	return 0;
}

/*
 * sends data to a copy command on a specific segment (usually
 * the hash result of the data value).
 */
void
cdbCopySendData(CdbCopy *c, int target_seg, const char *buffer,
				int nbytes)
{
	SegmentDatabaseDescriptor *q;
	Gang	   *gp;
	int			db;
	int			result;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;

	for (db = 0; db < c->mirror_map[target_seg]; db++)
	{

		if (db == 0)
			gp = c->primary_writer;
		else
			gp = c->mirror_writer;

		q = getSegmentDescriptorFromGang(gp, target_seg);

		/* transmit the COPY data */
		result = PQputCopyData(q->conn, buffer, nbytes);

		if (result != 1)
		{
			if (result == 0)
				appendStringInfo(&(c->err_msg),
								 "Failed to send data to %s segment %d, attempt blocked\n",
								 (db == 0 ? "primary" : "mirror"), target_seg);

			if (result == -1)
				appendStringInfo(&(c->err_msg),
								 "Failed to send data to %s segment %d: %s\n",
								 (db == 0 ? "primary" : "mirror"), target_seg, PQerrorMessage(q->conn));

			c->io_errors = true;
		}
	}
}

/*
 * gets a chunk of rows of data from a copy command.
 * returns boolean true if done. Caller should still
 * empty the leftovers in the outbuf in that case.
 */
bool cdbCopyGetData(CdbCopy *c, bool copy_cancel, uint64 *rows_processed)
{
	SegmentDatabaseDescriptor *q;
	Gang	    *gp;
	PGresult   *res;
	int			nbytes;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;

	/* clean out buf data */
	c->copy_out_buf.len = 0;
	c->copy_out_buf.data[0] = '\0';
	c->copy_out_buf.cursor = 0;
	
	gp = c->primary_writer;
	
	/*
	 * Collect data rows from the segments that still have rows to 
	 * give until chunk minimum size is reached
	 */
	while (c->copy_out_buf.len < COPYOUT_CHUNK_SIZE)
	{
		ListCell   *cur;
		
		/* iterate through all the segments that still have data to give */
		foreach(cur, c->outseglist)
		{
			int			source_seg = lfirst_int(cur);
			char		*buffer;
					
			q = getSegmentDescriptorFromGang(gp, source_seg);
			
			/* send a query cancel request to that segdb */
			if(copy_cancel)
				PQrequestCancel(q->conn);
			
			/* get 1 row of COPY data */
			nbytes = PQgetCopyData(q->conn, &buffer, false);
			
			/* 
			 * SUCCESS -- got a row of data 
			 */
			if (nbytes > 0 && buffer) 
			{
				/* append the data row to the data chunk */
				appendBinaryStringInfo(&(c->copy_out_buf), buffer, nbytes);
				
				/* increment the rows processed counter for the end tag */
				(*rows_processed)++;
				
				PQfreemem(buffer);
			}
			
			/* 
			 * DONE -- Got all the data rows from this segment, or a cancel request. 
			 */
			else if (nbytes == -1)
			{
				/*
				 * Fetch any error status existing on completion of the COPY command.
				 */
				while (NULL != (res = PQgetResult(q->conn)))
				{
					/* if the COPY command had an error */
					if (PQresultStatus(res) == PGRES_FATAL_ERROR)
					{
						appendStringInfo(&(c->err_msg), "MPPCOPY - Error from segment %d: %s\n",
										 source_seg, PQresultErrorMessage(res));
						c->remote_data_err = true;
					}
					
					/* free the PGresult object */
					PQclear(res);
				}
				
				/* remove the segment that completed sending data from the list*/
				c->outseglist = list_delete_int(c->outseglist, source_seg);
				
				/* no more segments with data on the list. we are done */
				if(list_length(c->outseglist) == 0)
					return true; /* done */
				
				/* start over from first seg as we just changes the seg list */
				break;
			}
				
			/* 
			 * ERROR!
			 */
			else
			{
				/* should never happen since we are blocking. Don't bother to try again, exit with error. */
				if (nbytes == 0)
					appendStringInfo(&(c->err_msg),
									 "MPPCOPY - Failed to get data from segment %d, attempt blocked\n",
									 source_seg);
				
				if (nbytes == -2)
					appendStringInfo(&(c->err_msg),
									 "MPPCOPY - Failed to get data from segment %d: %s\n",
									 source_seg, PQerrorMessage(q->conn));
				
				c->io_errors = true;
			}
		}
		
		if(c->copy_out_buf.len > COPYOUT_CHUNK_SIZE)
			break;
	}
	
	return false;
}

/*
 * ends the copy command on all segment databases.
 * c->err_msg will include the error msg(s) if any.
 */
void
cdbCopyEnd(CdbCopy *c)
{
	SegmentDatabaseDescriptor *q;
	SegmentDatabaseDescriptor **failedSegDBs;
	Gang	   *gp;
	PGresult   *res;
	int			result;			/* final result of COPY command execution */
	int			seg;
	int			db;
	int			failed_count = 0;
	bool		err_header = false;
	bool		primary_had_error; /* flag to consolidate pri/mir errors */
	bool		first_error = true;

	/* clean err message */
	c->err_msg.len = 0;
	c->err_msg.data[0] = '\0';
	c->err_msg.cursor = 0;

	/* allocate a failed segment database pointer array */
	failedSegDBs = (SegmentDatabaseDescriptor **) palloc(c->total_segs * 2 * sizeof(SegmentDatabaseDescriptor *));

	for (seg = 0; seg < c->total_segs; seg++)
	{
		primary_had_error = false; 
		
		for (db = 0; db < c->mirror_map[seg]; db++)
		{
			/* don't close COPY session if not in progress */
			if (c->segdb_state[seg][db] != SEGDB_COPY)
				continue;

			if (db == 0)
				gp = c->primary_writer;
			else
				gp = c->mirror_writer;

			q = getSegmentDescriptorFromGang(gp, seg);

			/* end this COPY command */
			result = PQputCopyEnd(q->conn, NULL);

			/* get command end status */
			if (result == 0)
			{
				/* attempt blocked */

				/*
				 * CDB TODO: Can this occur?  The libpq documentation says,
				 * "this case is only possible if the connection is in
				 * nonblocking mode... wait for write-ready and try again",
				 * i.e., the proper response would be to retry, not error out.
				 */

				if (!err_header)
					appendStringInfo(&(c->err_msg),
					"Failed to complete COPY on the following:\n");
				err_header = true;

				appendStringInfo(&(c->err_msg), "%s segment %d, dbid %d, attempt blocked\n", 
								 (db == 0 ? "primary" : "mirror"),
								 seg, q->segment_database_info->dbid);
				c->io_errors = true;
			}

			/*
			 * Fetch any error status existing on completion of the COPY command.
			 * It is critical that for any connection that had an asynchronous
			 * command sent thru it, we call PQgetResult until it returns NULL.
			 * Otherwise, the next time a command is sent to that connection,
			 * it will return an error that there's a command pending.
			 */
			while (NULL != (res = PQgetResult(q->conn)))
			{
				/* if the COPY command had a data error */
				if (PQresultStatus(res) == PGRES_FATAL_ERROR)
				{
					if(db == 0)
						primary_had_error = true;
					
					/* 
					 * Always append error from the primary. Append error from 
					 * mirror only if its primary didn't have an error. 
					 *
					 * For now, we only report the first error we get from the
					 * QE's.
					 *
					 * We get the error message in pieces so that we could append
					 * whoami to the primary error message only.
					 */
					if((db == 0 || (db != 0 && !primary_had_error)) && first_error)
					{
						char   *pri = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
						char   *dtl = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
						char   *ctx = PQresultErrorField(res, PG_DIAG_CONTEXT);
												
						appendStringInfo(&(c->err_msg), "%s", pri);
						
						if (q->whoami)
							appendStringInfo(&(c->err_msg),"  (%s)", q->whoami);
						
						if (dtl)
							appendStringInfo(&(c->err_msg), "\n%s", dtl);
						
						/*
						 * note that due to cdb_tidy_message() in elog.c "If more than 
						 * one line, move lines after the first to errdetail" so we save
						 * the context in another stringInfo and fetch it in the error
						 * callback in copy.c, so it wouldn't appear as DETAIL...
						 */
						if (ctx)
							appendStringInfo(&(c->err_context), "%s", ctx);
						
						/* Indicate that the err_msg already was filled with one error */
						first_error = false;
					}
					c->remote_data_err = true;
				}

				/* free the PGresult object */
				PQclear(res);
			}

			/* Finished with this segment db. */
			c->segdb_state[seg][db] = SEGDB_DONE;

			/* Lost the connection? */
			if (PQstatus(q->conn) == CONNECTION_BAD)
			{
				if (!err_header)
					appendStringInfo(&(c->err_msg),
					"ERROR - Failed to complete COPY on the following:\n");
				err_header = true;

				/* command error */
				c->io_errors = true;
				appendStringInfo(&(c->err_msg), "%s segment %d, dbid %d, with error: %s\n", (db == 0 ? "primary" : "mirror"),
				seg, q->segment_database_info->dbid, PQerrorMessage(q->conn));

				/* Free the PGconn object. */
				PQfinish(q->conn);
				q->conn = NULL;

				/* Let FTS deal with it! */
				failedSegDBs[failed_count] = q;
				failed_count++;
			}

		}
	}

	/* If lost contact with segment db, try to reconnect. */
	if (failed_count > 0)
	{
		elog(LOG, "%s", c->err_msg.data);
		elog(LOG, "MPPCOPY passes failed segment(s) information to FTS");
		FtsHandleNetFailure(failedSegDBs, failed_count);
	}
}


/*
 * start a global transaction for COPY.
 */
void
cdbCopyStartTransaction(void)
{
	/* since we don't use cdbdisp's dispatch services, we need to explicitly
	 * kick off a BEGIN if its the real start of a transaction
	 */
	startDtxTransaction(true);
	
	/* this txn is gonna be dirty */
	setCurrentDtxDirty();
}
