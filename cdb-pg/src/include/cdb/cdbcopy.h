/*--------------------------------------------------------------------------
*
* cdbcopy.h
*	 Definitions and API functions for cdbcopy.c
*	 These are functions that are used by the backend
*	 COPY command in MPP.
*
* Copyright (c) 2005, Greenplum
*
* IDENTIFICATION
*	 $Id$
*
*--------------------------------------------------------------------------
*/
#ifndef CDBCOPY_H
#define CDBCOPY_H

#include "lib/stringinfo.h"
#include "cdb/cdbgang.h"

typedef enum SegDbState
{
	/*
	 * (it is best to avoid names like OUT that are likely to be #define'd or
	 * typedef'd in some platform-dependent runtime library header file)
	 */
	SEGDB_OUT,					/* Not participating in COPY (invalid etc...) */
	SEGDB_IDLE,					/* Participating but COPY not yet started */
	SEGDB_COPY,					/* COPY in progress */
	SEGDB_DONE					/* COPY completed (with or without errors) */
}	SegDbState;

typedef struct CdbCopy
{
	Gang	   *primary_writer;
	Gang	   *mirror_writer;
	int			total_segs;		/* total number of segments in cdb */
	int		   *mirror_map;		/* indicates how many db's each segment has */
	bool		data_errors;	/* true if any data
								 * parsing/partitioning/inserting error
								 * occured */
	bool		io_errors;		/* true if any I/O error occured trying to
								 * communicate with segDB's */
	SegDbState **segdb_state;
	StringInfoData err_msg;		/* error message for cdbcopy operations */

} CdbCopy;



/* global function declarations */
CdbCopy    *makeCdbCopy(void);
int			cdbCopyGetDbCount(int total_segs, int seg);
int			cdbCopyStart(CdbCopy *cdbCopy, char *copyCmd);
void cdbCopySendData(CdbCopy *c, int target_seg, const char *buffer,
				int nbytes);
void		cdbCopyEnd(CdbCopy *c);
void		cdbCopyStartTransaction(void);

#endif   /* CDBCOPY_H */
