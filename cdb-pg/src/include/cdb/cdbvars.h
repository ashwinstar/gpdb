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

#endif	/* CDBVARS_H */
