/*-------------------------------------------------------------------------
 *
 * cdbvars.h
 *	  definitions for CDB global variables
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	  See src/backend/utils/misc/guc.c for variable external specification.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBVARS_H
#define CDBVARS_H

#include "utils/guc.h"

/* 
 * ----- Declarations of CDB global variables ------ 
 */
 
/*
 * Parameters cdb_session_role and cdb_role
 * 
 * The run-time parameters (GUC variables) cdb_session_role and
 * cdb_role report and provide control over the role assumed by a
 * postgres process.  
 * 
 * Valid  roles are the following:
 * 
 * 	dispatch	The process acts as a parallel SQL dispatcher.
 * 	execute		The process acts as a parallel SQL executor.
 * 	utility		The process acts as a simple SQL engine.
 * 
 * Both parameters are initialized to the same value at connection
 * time and are local to the backend process resulting from the
 * connection.  The default is dispatch which is the normal setting
 * for a user of CDB connecting to a node of  the CDB cluster.
 * Neither parameter appears in the configuration file.
 * 
 * cdb_session_role 
 * 
 * - does not affect the operation of the backend, and  
 * - does not change during the lifetime of PostgreSQL session.  
 * 
 * cdb_role 
 * 
 * - determines the operating role of the backend, and
 * - may be changed by a superuser via the SET command.	
 * 
 * The connection time value of cdb_session_role used by a
 * libpq-based client application can be specified using the
 * environment variable PGOPTIONS.  For example, this is how to
 * invoke psql on database test as a utility:
 * 
 * 	PGOPTIONS='-c cdb_session_role=utility' psql test
 * 
 * Alternatively, libpq-based applications can be modified to set
 * the value directly via an argument to one of the libpq functions
 * PQconnectdb, PQsetdbLogin or PQconnectStart.
 *
 * Don't try to set cdb_role this way.  At the time options are
 * processed it is unknown whether the user is a superuser, so
 * the attempt will be rejected.
 * 
 * ----------
 * 
 * CDB Developers can access the values of these parameters via the
 * global variables Cdb_session_role and Cdb_role of type
 * CdbRoleValue. For example
 * 
 * 	#include "cdb/cdbvars.h"
 * 	
 * 	switch ( Cdb_role  )
 * 	{
 * 		case CDB_ROLE_DISPATCH:
 * 			... Act like a query dispatcher ...
 * 			break;
 * 		case CDB_ROLE_EXECUTE:
 * 			... Act like a query executor ...
 * 			break;
 * 		case CDB_ROLE_UTILITY:
 * 			... Act like an unmodified PostgreSQL backend. ...
 * 			break;
 * 		default:
 * 			... Won't happen ..
 * 			break;
 * 	}
 * 
 * You can also modify Cdb_role (even if the session doesn't have
 * superuser privileges) by setting it to one of the three valid
 * values, however this must be well documented to avoid
 * disagreements between modules.  Don't modify the value  of
 * Cdb_session_role. 
 *
 */

typedef enum 
{
	CDB_ROLE_UTILITY = 0,	/* Operating as a simple database engine */
	CDB_ROLE_DISPATCH,		/* Operating as the parallel query dispatcher */
	CDB_ROLE_EXECUTE,		/* Operating as a parallel query executor */
	CDB_ROLE_UNDEFINED		/* Should never see this role in use */
} CdbRoleValue;


extern CdbRoleValue		Cdb_session_role; /* GUC var - server startup mode.  */
extern char			  * cdb_session_role_string;	/* Use by guc.c as staging area for value. */
extern const char	  * assign_cdb_session_role( const char *newval, bool doit, GucSource source );
extern const char	  * show_cdb_session_role( void );

extern CdbRoleValue		Cdb_role;	/* GUC var - server operating mode.  */
extern char			  * cdb_role_string;	/* Use by guc.c as staging area for value. */
extern const char	  * assign_cdb_role( const char *newval, bool doit, GucSource source );
extern const char	  * show_cdb_role( void );


/*
 * Parameter cdb_connections_per_thread
 *
 * The run-time parameter (GUC variables) cdb_connections_per_thread
 * controls how many libpq connections to qExecs are processed in each
 * thread.
 *
 * Any number >= 1 is valid.
 *
 * 1 means each connection has its own thread.
 *
 * This can be set in the config file, or at runtime by a superuser using
 * SQL: set cdb_connections_per_thread = x;
 *
 * The default is 256.  So, if there are fewer than 256 segdbs, all would be handled
 * by the same thread.
 *
 * Currently, this is used in two situation:
 *      1) In cdblink_setup, when the libpq connections are obtained by the dispatcher
 *              to the qExecs.
 *      2) In CdbDispatchCommand, when commands are sent from the dispatcher to the qExecs.
 *
 */
extern int			cdb_connections_per_thread;     /* GUC var - server operating mode.  */
extern bool			assign_cdb_connections_per_thread( int newval, bool doit, GucSource source );
extern const char	*show_cdb_connections_per_thread( void );


/*
 * Parameter cdb_max_tuple_chunk_size
 *
 * The run-time parameter (GUC variable) cdb_max_tuple_chunk_size controls the
 * maximum size of a block of data that the Motion Layer will move from the
 * postgres process to the AMS process through IPC.
 * 
 * When a tuple is sent to a different backend by a motion node, it is
 * serialized into a byte-sequence (called its "serialized form").  If a
 * tuple's serialized form is less than the maximum tuple chunk size, then the
 * tuple will be moved as a complete unit to the AMS.  However, if the tuple's
 * serialized form is larger than the maximum tuple chunk size then the tuple
 * will be split into two or more chunks and moved piecemeal to the AMS.
 *
 * Ideally, most tuples being moved by motion nodes will have serialized forms
 * that are smaller than the maximum tuple chunk size.  However, it is
 * reasonable to expect that at least some tuples will need to be broken into
 * smaller chunks.
 * 
 * The minimum value of this parameter is 64 bytes, and the maximum is
 * slightly less than 64KB.
 */
extern int Cdb_max_tuple_chunk_size;    /* GUC var */

/*
 * Parameters enable_cdbrepartition and enable_cdbbroadcast
 *
 * These run-time parameters (GUC variables) influence the cdb join strategies.
 * These are analagous to the enable_nestloop, enable_mergejoin etc variable
 * for the postgres planner.  When set to false, the cost for that type of
 * strategy is increased by a very large amount.  This will effectively
 * cause the code to not choose this strategy unless there is no other.
 */
extern bool enable_cdbrepartition;
extern bool enable_cdbbroadcast;

/* --------------------------------------------------------------------------------------------------
 * Structure for row of cdb.cdb table
 */
typedef struct CdbCdb
{
    char	*cdbname; 		// array of SegmentDatabaseInfo's for segment databases
    int4	numsegments;	// count of distinct segindexes
	int4	dbid;			// the dbid of this database
	int4	segindex;		// content indicator: -1 for entry database, 0, ..., n-1 for segment database
} CdbCdb;

/* --------------------------------------------------------------------------------------------------
 * Global variable declaration for the data for the single row of cdb.cdb table
 */
extern CdbCdb CdbIdentity;

#endif	/* CDBVARS_H */
