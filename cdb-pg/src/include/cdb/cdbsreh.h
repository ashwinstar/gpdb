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

typedef struct CdbSreh
{
	char	*errmsg;		/* the error message for this bad data row */
	char	*rawdata;		/* the bad data row */
	int		linenumber;		/* line number of error in original file */
	int		rejectlimit;	/* SEGMENT REJECT LIMIT value */
	int		rejectcount;	/* how many were rejected so far */
	bool	is_server_enc;	/* was bad row converted to server encoding? */
	bool	should_keep;	/* if true error table should not get DROP'ed */
	CdbCopy *cdbcopy;		/* for QD to send bad rows to random QE */
	
} CdbSreh;

extern CdbSreh *makeCdbSreh(bool should_keep, int rejectlimit);
extern void HandleSingleRowError(CdbSreh *cdbsreh);

#endif /* CDBSREH_H */
