/*-------------------------------------------------------------------------
 *
 * cdbvars.c
 *	  Provides storage areas and processing routines for CDB variables
 *	  managed by GUC.
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

#include "postgres.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "utils/memutils.h"
 
/*
 * ----------------
 * 		GUC/global variables
 *
 * 	Initial values are set by guc.c function "InitializeGUCOptions" called
 * 	*very* early during postmaster, postgres, or bootstrap initialization.
 * ----------------
 */
 
 
 
CdbRoleValue		Cdb_role;			/* Role paid by this CDB backend */
char			   *cdb_role_string;	/* Staging area for guc.c */

CdbRoleValue		Cdb_session_role;	/* Role paid by this CDB backend */
char			   *cdb_session_role_string; /* Staging area for guc.c */

int					cdb_connections_per_thread;	/* How many libpq connections are handled in each thread */

int					Cdb_max_tuple_chunk_size;   /* CDB Motion Layer -> AMS */

int					Cdb_querygroup_id;		/* Per-session ID, same across all backends in the session. */

bool				enable_cdbrepartition;
bool				enable_cdbbroadcast;

int 				ams_wait_time;  /*time in microseconds for motion layer to wait. */

/* 
 * The single row from cdb.cdb for this database is held in this variable,
 * which is initialized in cdbutil.c in the function
 * getCdbCdb(), which is called from cdblink_setup() 
 * for both dispatch and execute modes.
 *
 * Any code needing the "mysegindex" ot "numsegments"
 * can simply #include cdbvars.h, and use CdbIdentity.numsegments
 * or CdbIdentity.segindex.
 */
CdbCdb				CdbIdentity = {NULL, 0, 0, 0};  

/*
 * Local macro to provide string values of numeric defines.
 */
#define CppNumericAsString(s) CppAsString(s)

/* 
 *	Forward declarations of local function.
 */
CdbRoleValue string_to_role( const char *string );
const char * role_to_string( CdbRoleValue role );


/*
 * Convert a CDB role string (as for cdb_session_role or cdb_role) to an 
 * enum value of type CdbRoleValue. Return CDB_ROLE_UNDEFINED in case the 
 * string is unrecognized.
 */
CdbRoleValue string_to_role( const char *string )
{
	CdbRoleValue role = CDB_ROLE_UNDEFINED;
	
	if ( strcasecmp( string, "dispatch" ) == 0 || strcasecmp( string, "" ) == 0 )
    {
        role = CDB_ROLE_DISPATCH;
    }
	else if ( strcasecmp( string, "execute" ) == 0 )
    {
        role = CDB_ROLE_EXECUTE;
    }
    else if ( strcasecmp( string, "utility" ) == 0 )
    {
        role = CDB_ROLE_UTILITY;
    }
	
	return role;
}

/* 
 * Convert a CdbRoleValue to a role string (as for cdb_session_role or 
 * cdb_role).  Return eyecatcher in the unexpected event that the value
 * is unknown or undefined.
 */
const char * role_to_string( CdbRoleValue role )
{
	switch ( role )
	{
	case CDB_ROLE_DISPATCH:
		return "dispatch";
	case CDB_ROLE_EXECUTE:
		return "execute";
	case CDB_ROLE_UTILITY:
		return "utility";
	case CDB_ROLE_UNDEFINED:
	default:
		return "*undefined*";
	}
}


/*
 * Assign hook routine for "cdb_session_role" option.  Because this variable
 * has context PGC_BACKEND, we expect this assigment to happen only during
 * setup of a BACKEND, e.g., based on the role value specified on the connect
 * request.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_cdb_session_role( const char *newval, bool doit, GucSource source )
{
    
#if FALSE
    elog(DEBUG1, "assign_cdb_session_role: cdb_session_role=%s, newval=%s, doit=%s",
            show_cdb_session_role(), newval, ( doit ? "true" : "false" ) );
#endif

	CdbRoleValue newrole = string_to_role(newval);
	
	if ( newrole  == CDB_ROLE_UNDEFINED )
	{
		return NULL;
	}
	
    if ( doit )
    {
        Cdb_session_role = newrole;
   		Cdb_role = Cdb_session_role;
   	}
    return newval;
}



/*
 * Assign hook routine for "cdb_role" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set using an option on postmaster start, but this isn't
 * interesting beccause the derived global CdbRole is always set (along with 
 * CdbSessionRole) on backend startup for a new connection.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_cdb_role( const char *newval, bool doit, GucSource source )
{

#if FALSE
    elog(DEBUG1, "assign_cdb_role: cdb_role=%s, newval=%s, doit=%s",
            show_cdb_role(), newval, ( doit ? "true" : "false" ));
#endif


	CdbRoleValue newrole = string_to_role(newval);
	CdbRoleValue oldrole = Cdb_role;
	
	if ( newrole  == CDB_ROLE_UNDEFINED )
	{
		return NULL;
	}
	
    if ( doit )
    {
    	/*
    	 * When changing between roles, we must
    	 * call cdb_cleanup and then cdb_setup to get 
		 * setup and connections appropriate to the new role.
    	 */
    	bool do_disconnect = false;
    	bool do_connect = false;
    	
    	if ( Cdb_role != newrole && IsUnderPostmaster )
    	{
    		if ( Cdb_role != CDB_ROLE_UTILITY )
    			do_disconnect = true;

    		if ( newrole != CDB_ROLE_UTILITY )
    			do_connect = true;
    	}
    	
    	if ( do_disconnect )
    		cdb_cleanup();
	    	
        Cdb_role = newrole;
        
    	if ( do_connect )
    	{
    		/*
    		 * In case there are problems with the cdb tables or data,
    		 * we catch any error coming out of cdblink_setup so we can set the
    		 * cdb_role back to what it was.  Otherwise we may be left with
    		 * inappropriate connections for the new role.
    		 */
			PG_TRY();
			{
		  		cdb_setup();
			}
			PG_CATCH();
			{
	    		cdb_cleanup();
				Cdb_role = oldrole;
				if ( Cdb_role != CDB_ROLE_UTILITY )
		  			cdb_setup();
				PG_RE_THROW();
			}
			PG_END_TRY();
    	}
   	}
   	
    return newval;
}


/*
 * Assign hook routine for "cdb_connections_per_thread" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set in config file, but not inside of PGOPTIONS.)
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
bool
assign_cdb_connections_per_thread( int newval, bool doit, GucSource source )
{

#if FALSE
    elog(DEBUG1, "assign_cdb_connections_per_thread: cdb_connections_per_thread=%s, newval=%d, doit=%s",
            show_cdb_connections_per_thread(), newval, ( doit ? "true" : "false" ));
#endif
            
    if ( doit )
    {
    	if ( newval < 1 )
    		return false;
    		
        cdb_connections_per_thread = newval;
    }
   	
    return true;
}

/*
 * Show hook routine for "cdb_session_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_cdb_session_role( void )
{
    return role_to_string(Cdb_session_role);
}


/*
 * Show hook routine for "cdb_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_cdb_role( void )
{
    return role_to_string(Cdb_role);
}

/*
 * Show hook routine for "cdb_connections_per_thread" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_cdb_connections_per_thread( void )
{
	/*
	 * We rely on the fact that the memory context will clean up the memory
	 * for the buffer.data.
	 */
	StringInfoData buffer;
	initStringInfo(&buffer);
	
	appendStringInfo( &buffer, "%d", cdb_connections_per_thread );

    return buffer.data;
}

