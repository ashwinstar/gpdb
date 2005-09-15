/*
 * cdbconn.h
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Darko Prenosil <Darko.Prenosil@finteh.hr>
 * Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 * Copyright (c) 2001-2003, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#ifndef CDBCONN_H
#define CDBCONN_H

#include "libpq-fe.h"
#include "cdb/cdbutil.h"
#include <pthread.h>


/* --------------------------------------------------------------------------------------------------
 * Structure for segment database definition and working values
 */
typedef struct segment_database_descriptor
{
	/* segment_descriptor
	 *
	 * Points to the SegmentDescriptor for the segment of which this 
	 * segment database belongs.
	 */
	
	struct segment_descriptor   *segment_descriptor;
	
	/* segment_database_info
	 *
	 * Points to the SegmentDatabaseInfo structure describing the 
	 * parameters for this segment database.  Information in this structure is
	 * obtained from the CDB administrative schema tables.
	 */
	CdbComponentDatabaseInfo 	*segment_database_info;
	
	/* conn
	 *
	 * A non-NULL value points to the PGconn block of a successfully
	 * established connection to the segment database.
	 */
	PGconn						*conn;		
	
	/* thread
	 *
	 * A non-NULL value points to the thread reference for the thread
	 * executing the SQL statement on this segment.
	 */
	pthread_t					thread;
	
	/* error_message
	 *
	 * A non-NULL value points to an error message
	 * describing the connection failure associated with the last connection
	 * attempt to this segment/instance, or the last command issued thru this connection.
	 *
	 */
	char						*error_message;

	/* cmdTuples
	 *
	 * Number of rows in the result of the SQL command.
	 */
	int         				cmdTuples;

	/* resultStatus
	 *
	 * The SQL statement result, from PQresultStatus.
	 */
	ExecStatusType				resultStatus;

	/* cmdStatus
	 *
	 * The SQL statement command status, from PQcmdStatus.
	 */
	char						*cmdStatus;
	
	/* 
	 * interconnect Listener port 
	 * 
	 */
	int4		motionListener; 
	
	/* segindex
	 * 
	 * Identifies the segment this group of databases is serving.  This is the
	 * segindex assigned to the segment in mpp.segment.  Duplicate of segment_descriptor.segindex
	 */
	int4         					segindex;
	
} SegmentDatabaseDescriptor;

/* --------------------------------------------------------------------------------------------------
 * Structure for segment definition and working values
 */
typedef struct segment_descriptor
{
	/* segindex
	 * 
	 * Identifies the segment this group of databases is serving.  This is the
	 * segindex assigned to the segment in mpp.segment.
	 */
	int4         					segindex;
	
	/* db_count
	 *
	 * The number of segment database descriptors in db_descriptors
	 * below. Should be 1 or 2.
	 */
	int								db_count;
	
	/* connected_count
	 * 
	 * The number of segment databases to which a connection was obtained.
	 */
	int								connected_count;
	
	/*
	 * error_message
	 *
	 * Points to the error message text string applying to failures at the 
	 * segment level.  This includes lack of valid segment databases
	 * as well as failure to connect to any segment database.
	 */
	char				   			*error_message;
	
	/* db_descriptors
	 *
	 * A pointer to an array of SegmentDatabaseDescriptors, one for each
	 * segment database implementing this segindexr.  
	 */
	SegmentDatabaseDescriptor		*db_descriptors;
	
	/* query_sdd
	 *
	 * A pointer to the SegmentDatabaseDescriptor to be used for queries.
	 */
	SegmentDatabaseDescriptor		*query_db;
	
	/* sql
	 *
	 * A non-NULL value points to the SQL statement presented to this 
	 * segment.
	 */
	char 	   			  			*sql;
} SegmentDescriptor;


/* --------------------------------------------------------------------------------------------------
 * The CdbClusterDescriptor is the root level structure created by
 * cdblink_setup().  This structure, along with SegmentDescriptor and
 * SegmentDatabaseDescriptor, identifies all of the logical segments
 * in the CDB cluster and the connections made to each of the segment dastabases.
 */
 
typedef struct cdb_cluster_descriptor {
	
	int						segment_count;
	int						segdb_count;
	bool					all_valid_segdbs_connected;
	SegmentDescriptor	   *segment_descriptors;
} CdbClusterDescriptor;


/*
 * External declarations
 */
//extern CdbClusterDescriptor	   *CdbClusterDefinition;


#endif   /* CDBCONN_H */
