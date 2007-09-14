/*--------------------------------------------------------------------------
*
* cdbsreh.c
*	  Provides routines for single row error handling for COPY and external
*	  tables.
*
* Copyright (c) 2007, Greenplum
*
* IDENTIFICATION
*	  $Id$
*
*--------------------------------------------------------------------------
*/
#include "postgres.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"


/*
 * makeCdbSreh
 *
 * Allocate and initialize a Single Row Error Handling state object.
 * Pass in the only known parameters (both we get from the SQL stmt),
 * the other variables are set later on, when they are known.
 */
CdbSreh *
makeCdbSreh(bool should_keep, int rejectlimit, bool is_limit_in_rows)
{
	CdbSreh	*h;
	
	h = palloc(sizeof(CdbSreh));
	
	h->errmsg = NULL;
	h->rawdata = NULL;
	h->linenumber = 0;
	h->rejectlimit = rejectlimit;
	h->is_limit_in_rows = is_limit_in_rows;
	h->rejectcount = 0;
	h->is_server_enc = false;
	h->should_keep = should_keep;
	h->cdbcopy = NULL;
	
	return h;
}

/*
 * HandleSingleRowError
 *
 * The single row error handler. Gets called when a data error happened
 * in SREH mode. Reponsible for making a decision of what to do at that time.
 *
 * Some of the main actions are:
 *  - Keep track of reject limit. if reached make sure notify caller.
 *  - If error table used, call the error logger to log this error.
 *  - If QD COPY send the bad row to the QE COPY to deal with.
 *
 */
void HandleSingleRowError(CdbSreh *cdbsreh)
{
	/* increment total number of errors for this segment */
	cdbsreh->rejectcount++;
	
	/* 
	 * if reached the segment reject limit don't do anything.
	 * (this will get checked and handled later on by the caller).
	 */
	if(cdbsreh->rejectcount >= cdbsreh->rejectlimit)
		return;
	
	/* do stuff... */
	
	return; /* OK */
}

/* 
 * ReportSrehResults
 *
 * When necessary emit a NOTICE that describes the end result of the
 * SREH operations. Information includes the total number of rejected
 * rows, and whether rows were ignored or logged into an error table.
 */
void ReportSrehResults(CdbSreh *cdbsreh, int total_rejected)
{
	if(total_rejected > 0)
		ereport(NOTICE, 
				(errmsg("Rejected %d badly formatted rows.", total_rejected)));
	
	/* TODO: expand this function when error tables are enabled */
}


/*
 * Using this function the QE sends back to the client QD the number 
 * of rows that were rejected in this last data load in SREH mode.
 */
void SendNumRowsRejected(int numrejected)
{
	StringInfoData buf;
	
	Insist(Gp_role == GP_ROLE_EXECUTE);
	pq_beginmessage(&buf, 'j'); /* 'j' is the msg code for rejected records */
	pq_sendint(&buf, numrejected, 4);
	pq_endmessage(&buf);	
}





