/*-------------------------------------------------------------------------
*
* cdbsreh.h
*	  routines for single row error handling
*
* Copyright (c) 2007, greenplum inc
*
* $Id$
*
*-------------------------------------------------------------------------
*/

#ifndef CDBSREH_H
#define CDBSREH_H

#include "cdb/cdbcopy.h"
#include "utils/memutils.h"


/*
 * The error table is ALWAYS of the following format
 * cmdtime     timestamptz,
 * relname     text,
 * filename    text,
 * linenum     int,
 * bytenum     int,
 * errmsg      text,
 * rawdata     text,
 * rawbytes    bytea
 */
#define NUM_ERRORTABLE_ATTR 8
#define errtable_cmdtime 1
#define errtable_relname 2
#define errtable_filename 3   
#define errtable_linenum 4    
#define errtable_bytenum 5    
#define errtable_errmsg 6    
#define errtable_rawdata 7    
#define errtable_rawbytes 8   

/*
 * All the Single Row Error Handling state is kept here.
 * When an error happens and we are in single row error handling
 * mode this struct is updated and handed to the single row
 * error handling manager (cdbsreh.c).
 */
typedef struct CdbSreh
{
	char	*errmsg;		/* the error message for this bad data row */
	char	*rawdata;		/* the bad data row */
	char	*filename;		/* file where data comes from */
	char	*relname;		/* target relation */
	int		linenumber;		/* line number of error in original file */
	int		rejectlimit;	/* SEGMENT REJECT LIMIT value */
	int		rejectcount;	/* how many were rejected so far */
	bool	is_limit_in_rows; /* ROWS = true, PERCENT = false */
	bool	is_server_enc;	/* was bad row converted to server encoding? */
	bool	should_keep;	/* if true error table should not get DROP'ed */
	Relation errtbl;		/* the error table we use (if any) */
	
	CdbCopy *cdbcopy;		/* for QD COPY to send bad rows to random QE */
	int		lastsegid;		/* last QE COPY segid that QD COPY sent bad row to */
	
	MemoryContext badrowcontext;	/* per-badrow evaluation context */

} CdbSreh;

extern CdbSreh *makeCdbSreh(bool should_keep, int rejectlimit, 
							bool is_limit_in_rows, RangeVar *errtbl,
							char *filename, char *relname);
extern void destroyCdbSreh(CdbSreh *cdbsreh);
extern void HandleSingleRowError(CdbSreh *cdbsreh);
extern void ReportSrehResults(CdbSreh *cdbsreh, int total_rejected);
extern void SendNumRowsRejected(int numrejected);
extern void ValidateErrorTableMetaData(Relation rel);

#endif /* CDBSREH_H */
