/*-------------------------------------------------------------------------
 *
 * cdbconn.h
 *
 * Functions returning results from a remote database
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBCONN_H
#define CDBCONN_H

#include "libpq-fe.h"               /* prerequisite for libpq-int.h */
#include "libpq-int.h"              /* PQExpBufferData */


/* --------------------------------------------------------------------------------------------------
 * Structure for segment database definition and working values
 */
typedef struct SegmentDatabaseDescriptor
{
	/* segment_database_info
	 *
	 * Points to the SegmentDatabaseInfo structure describing the
	 * parameters for this segment database.  Information in this structure is
	 * obtained from the MPP administrative schema tables.
	 */
	struct CdbComponentDatabaseInfo *segment_database_info;
	
	/* conn
	 *
	 * A non-NULL value points to the PGconn block of a successfully
	 * established connection to the segment database.
	 */
	PGconn				   *conn;		
	
	/* error_message
	 *
	 * A buffer containing messages received from the segment database.
	 */
	PQExpBufferData         error_message;

	/* segindex
	 *
	 * Identifies the segment this group of databases is serving.  This is the
	 * segindex assigned to the segment in mpp.segment.
	 */
	int4         			segindex;
	
    /*
     * Connection info saved at most recent PQconnectdb.
     *
     * NB: Use malloc/free, not palloc/pfree, for the items below.
     */
    char                   *dbname;         /* database name */
    int4		            motionListener; /* interconnect listener port */
    char                   *whoami;         /* QE identifier for msgs */

} SegmentDatabaseDescriptor;



void
cdbconn_initSegmentDescriptor(SegmentDatabaseDescriptor        *segdbDesc,
                              struct CdbComponentDatabaseInfo  *cdbinfo);


/* Free all memory owned by a segment descriptor. */
void
cdbconn_termSegmentDescriptor(SegmentDatabaseDescriptor *segdbDesc);


/* Connect to a QE as a client via libpq. */
bool                            /* returns true if connected */
cdbconn_doConnect(SegmentDatabaseDescriptor    *segdbDesc,
                  const char                   *dbname,
                  const char                   *options);


/* Set the slice index for error messages related to this QE. */
void
cdbconn_setSliceIndex(SegmentDatabaseDescriptor    *segdbDesc,
                      int                           sliceIndex);


#endif   /* CDBCONN_H */
