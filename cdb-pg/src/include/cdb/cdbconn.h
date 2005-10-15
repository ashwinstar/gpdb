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
	
	/* segindex
	 *
	 * Identifies the segment this group of databases is serving.  This is the
	 * segindex assigned to the segment in mpp.segment.
	 */
	int4         			segindex;
	
    /*
     * Which gang does this QE belong to?
     */
    struct Gang            *gang;           /* QE is a member of this gang */
    int                     gangindex;      /* QE's position within the gang */

    /* conn
	 *
	 * A non-NULL value points to the PGconn block of a successfully
	 * established connection to the segment database.
	 */
	PGconn				   *conn;		
	
	/*
	 * Error info saved when connection cannot be established.
	 */
    int                     errcode;        /* ERRCODE_xxx (sqlstate encoded as
                                             * an int) of first error, or 0.
                                             */
	PQExpBufferData         error_message;  /* message text; '\n' at end */

    /*
     * Connection info saved at most recent PQconnectdb.
     *
     * NB: Use malloc/free, not palloc/pfree, for the items below.
     */
    char                   *dbname;         /* database name */
    int4		            motionListener; /* interconnect listener port */
    char                   *whoami;         /* QE identifier for msgs */

} SegmentDatabaseDescriptor;


/* Initialize a segment descriptor in storage provided by the caller. */
void
cdbconn_initSegmentDescriptor(SegmentDatabaseDescriptor        *segdbDesc,
                              struct CdbComponentDatabaseInfo  *cdbinfo,
                              const char                       *dbname,
                              struct Gang                      *gang,
                              int                               gangindex);


/* Free all memory owned by a segment descriptor. */
void
cdbconn_termSegmentDescriptor(SegmentDatabaseDescriptor *segdbDesc);


/* Connect to a QE as a client via libpq. */
bool                            /* returns true if connected */
cdbconn_doConnect(SegmentDatabaseDescriptor    *segdbDesc,
                  const char                   *options);


/* Set the slice index for error messages related to this QE. */
void
cdbconn_setSliceIndex(SegmentDatabaseDescriptor    *segdbDesc,
                      int                           sliceIndex);


#endif   /* CDBCONN_H */
