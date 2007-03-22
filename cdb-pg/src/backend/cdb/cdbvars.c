/*-------------------------------------------------------------------------
 *
 * cdbvars.c
 *	  Provides storage areas and processing routines for Greenplum Database variables
 *	  managed by GUC.
 *
 * Copyright (c) 2003-2007, Greenplum
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
 *		GUC/global variables
 *
 *	Initial values are set by guc.c function "InitializeGUCOptions" called
 *	*very* early during postmaster, postgres, or bootstrap initialization.
 * ----------------
 */



GpRoleValue Gp_role;			/* Role paid by this Greenplum Database backend */
char	   *gp_role_string;	/* Staging area for guc.c */
char	   *gp_fault_action_string;	/* Staging area for guc.c */
bool		gp_set_read_only;	/* Staging area for guc.c */

GpRoleValue Gp_session_role;	/* Role paid by this Greenplum Database backend */
char	   *gp_session_role_string;	/* Staging area for guc.c */

bool		Gp_is_primary;		/* Is this session primary (or mirror) */

bool		Gp_is_writer; 		/* is this qExec a "writer" process. */

int 		gp_session_id;    /* global unique id for session. */

char       *gp_qd_callback_info;	/* info for QE to call back to QD */

bool		gp_enable_functions; 	/* allow functions that call back from QE to QD */

bool 		gp_is_callback;		/* are we executing a callback query? */

unsigned long gp_qd_proc_offset;	/* the proc entry for the original backend on the QD */

int         gp_command_count;          /* num of commands from client */

bool		Debug_print_prelim_plan;	/* Shall we log argument of
										 * cdbparallelize? */

bool		Debug_print_slice_table;	/* Shall we log the slice table? */

int			gp_connections_per_thread; /* How many libpq connections are
										 * handled in each thread */


int			Gp_segment = UNDEF_SEGMENT;		/* What content this QE is
												 * handling. */

bool		Gp_write_shared_snapshot;	/* tell the writer QE to write the
										 * shared snapshot */

bool		gp_reraise_signal=true;	/* try to dump core when we get SIGABRT & SIGSEGV */

bool		gp_set_proc_affinity=false; /* set processor affinity (if platform supports it) */

/*
 * TCP port the Interconnect listens on for incoming connections from other
 * backends.  Assigned by initMotionLayerIPC() at process startup.  This port
 * is used for the duration of this process and should never change.
 */
int			Gp_listener_port;

int			Gp_max_packet_size;	/* max Interconnect packet size */

int			interconnect_setup_timeout;


/*
 * Greenplum Database Optimizer GUCs
 */

bool        enable_adaptive_nestloop = true;
double      gp_motion_cost_per_row = 0;
int         gp_segments_for_planner = 0;
bool		enable_agg_restruct = TRUE;
double		gp_hashagg_rewrite_limit = 1;

bool		gp_enable_motion_deadlock_sanity = FALSE; /* planning time sanity check */

/* Greenplum Database Experimental Feature GUCs */
bool		gp_enable_agg_distinct = FALSE;
bool		gp_enable_preunique = FALSE;
bool		gp_enable_explain_allstat = FALSE;
bool		gp_enable_sort_limit = FALSE;
bool		gp_enable_sort_distinct = FALSE;
int			gp_sort_flags = 0;


/* --------------------------------------------------------------------------------------------------
 * Server debugging
 */

/*
 * gp_debug_linger (integer)
 *
 * Upon an error with severity FATAL and error code ERRCODE_INTERNAL_ERROR,
 * errfinish() will sleep() for the specified number of seconds before
 * termination, to let the user attach a debugger.
 */
int  gp_debug_linger = 30;


/* ----------------
 * Non-GUC globals
 */

int			currentSliceId = UNSET_SLICE_ID;	/* used by elog to show the
												 * current slice the process
												 * is executing. */
SeqServerControlBlock *seqServerCtl;

/* Segment id where singleton gangs are to be dispatched. */
int         gp_singleton_segindex;


/* ----------------
 * The single row from gp_id for this database is held in this variable,
 * which is initialized in cdbutil.c in the function
 * getGpId(), which is called from cdblink_setup()
 * for both dispatch and execute modes.
 *
 * Any code needing the  "numsegments"
 * can simply #include cdbvars.h, and use GpIdentity.numsegments
 */
GpId		GpIdentity = {NULL, 0, -1, -999};


/*
 * Keep track of a few dispatch-related  statistics:
 */
int cdb_total_slices = 0;
int cdb_total_plans = 0;
int cdb_max_slices = 0;

/*
 * Local macro to provide string values of numeric defines.
 */
#define CppNumericAsString(s) CppAsString(s)

/*
 *	Forward declarations of local function.
 */
GpRoleValue string_to_role(const char *string);
const char *role_to_string(GpRoleValue role);


/*
 * Convert a Greenplum Database role string (as for gp_session_role or gp_role) to an
 * enum value of type GpRoleValue. Return GP_ROLE_UNDEFINED in case the
 * string is unrecognized.
 */
GpRoleValue
string_to_role(const char *string)
{
	GpRoleValue role = GP_ROLE_UNDEFINED;

	if (pg_strcasecmp(string, "dispatch") == 0 || pg_strcasecmp(string, "") == 0)
	{
		role = GP_ROLE_DISPATCH;
	}
	else if (pg_strcasecmp(string, "execute") == 0)
	{
		role = GP_ROLE_EXECUTE;
	}
	else if (pg_strcasecmp(string, "utility") == 0)
	{
		role = GP_ROLE_UTILITY;
	}

	return role;
}

/*
 * Convert a GpRoleValue to a role string (as for gp_session_role or
 * gp_role).  Return eyecatcher in the unexpected event that the value
 * is unknown or undefined.
 */
const char *
role_to_string(GpRoleValue role)
{
	switch (role)
	{
		case GP_ROLE_DISPATCH:
			return "dispatch";
		case GP_ROLE_EXECUTE:
			return "execute";
		case GP_ROLE_UTILITY:
			return "utility";
		case GP_ROLE_UNDEFINED:
		default:
			return "*undefined*";
	}
}


/*
 * Assign hook routine for "gp_session_role" option.  Because this variable
 * has context PGC_BACKEND, we expect this assigment to happen only during
 * setup of a BACKEND, e.g., based on the role value specified on the connect
 * request.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_gp_session_role(const char *newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_gp_session_role: gp_session_role=%s, newval=%s, doit=%s",
		 show_gp_session_role(), newval, (doit ? "true" : "false"));
#endif

	GpRoleValue newrole = string_to_role(newval);

	if (newrole == GP_ROLE_UNDEFINED)
	{
		return NULL;
	}

	if (doit)
	{
		Gp_session_role = newrole;
		Gp_role = Gp_session_role;

		if (Gp_role == GP_ROLE_DISPATCH)
			Gp_segment = -1;
	}
	return newval;
}



/*
 * Assign hook routine for "gp_role" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set using an option on postmaster start, but this isn't
 * interesting beccause the derived global CdbRole is always set (along with
 * CdbSessionRole) on backend startup for a new connection.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_gp_role(const char *newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_gp_role: gp_role=%s, newval=%s, doit=%s",
		 show_gp_role(), newval, (doit ? "true" : "false"));
#endif


	GpRoleValue newrole = string_to_role(newval);
	GpRoleValue oldrole = Gp_role;

	if (newrole == GP_ROLE_UNDEFINED)
	{
		return NULL;
	}

	if (doit)
	{
		/*
		 * When changing between roles, we must
		 * call cdb_cleanup and then cdb_setup to get
		 * setup and connections appropriate to the new role.
		 */
		bool		do_disconnect = false;
		bool		do_connect = false;

		if (Gp_role != newrole && IsUnderPostmaster)
		{
			if (Gp_role != GP_ROLE_UTILITY)
				do_disconnect = true;

			if (newrole != GP_ROLE_UTILITY)
				do_connect = true;
		}

		if (do_disconnect)
			cdb_cleanup(0, (Datum) NULL);

		Gp_role = newrole;

		if (source != PGC_S_DEFAULT)
		if (do_connect)
		{
			/*
			 * In case there are problems with the Greenplum Database tables or data,
			 * we catch any error coming out of cdblink_setup so we can set the
			 * gp_role back to what it was.  Otherwise we may be left with
			 * inappropriate connections for the new role.
			 */
			PG_TRY();
			{
				cdb_setup();
			}
			PG_CATCH();
			{
				cdb_cleanup(0, (Datum) NULL);
				Gp_role = oldrole;
				if (Gp_role != GP_ROLE_UTILITY)
					cdb_setup();
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
	}

	return newval;
}


/*
 * Assign hook routine for "gp_connections_per_thread" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set in config file, but not inside of PGOPTIONS.)
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
bool
assign_gp_connections_per_thread(int newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_gp_connections_per_thread: gp_connections_per_thread=%s, newval=%d, doit=%s",
	   show_gp_connections_per_thread(), newval, (doit ? "true" : "false"));
#endif

	if (doit)
	{
		if (newval < 1)
			return false;

		gp_connections_per_thread = newval;
	}

	return true;
}

/*
 * Assign hook routine for "gp_interconnect_setup_timeout" option.
 *
 */
bool
assign_interconnect_setup_timeout(int newval, bool doit, GucSource source)
{

	if (doit)
	{
		if (newval < 0)
			return false;

		interconnect_setup_timeout = newval;

	}
	return true;
}



/*
 * Show hook routine for "gp_session_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_gp_session_role(void)
{
	return role_to_string(Gp_session_role);
}


/*
 * Show hook routine for "gp_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_gp_role(void)
{
	return role_to_string(Gp_role);
}

/*
 * Show hook routine for "gp_connections_per_thread" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_gp_connections_per_thread(void)
{
	/*
	 * We rely on the fact that the memory context will clean up the memory
	 * for the buffer.data.
	 */
	StringInfoData buffer;

	initStringInfo(&buffer);

	appendStringInfo(&buffer, "%d", gp_connections_per_thread);

	return buffer.data;
}

/*
 * Show hook routine for "gp_interconnect_setup_timeout" option.
 *
 * See guc.c for option definition.
 *
 */
const char *
show_interconnect_setup_timeout(void)
{
	/*
	 * We rely on the fact that the memory context will clean up the memory
	 * for the buffer.data.
	 */
	StringInfoData buffer;

	initStringInfo(&buffer);

	appendStringInfo(&buffer, "%d", interconnect_setup_timeout);

	return buffer.data;
}


/* --------------------------------------------------------------------------------------------------
 * Logging
 */


/*
 * gp_log_gangs (string)
 *
 * Should creation, reallocation and cleanup of gangs of QE processes be logged?
 * "OFF"     -> only errors are logged
 * "TERSE"   -> terse logging of routine events, e.g. creation of new qExecs
 * "VERBOSE" -> gang allocation per command is logged
 * "DEBUG"   -> additional events are logged at severity level DEBUG1 to DEBUG5
 *
 * The messages that are enabled by the TERSE and VERBOSE settings are
 * written with a severity level of LOG.
 */
GpVars_Verbosity   gp_log_gang;

/*
 * gp_log_interconnect (string)
 *
 * Should connections between internal processes be logged?  (qDisp/qExec/etc)
 * "OFF"     -> connection errors are logged
 * "TERSE"   -> terse logging of routine events, e.g. successful connections
 * "VERBOSE" -> most interconnect setup events are logged
 * "DEBUG"   -> additional events are logged at severity level DEBUG1 to DEBUG5.
 *
 * The messages that are enabled by the TERSE and VERBOSE settings are
 * written with a severity level of LOG.
 */
GpVars_Verbosity   gp_log_interconnect;


/*
 * gpvars_string_to_verbosity
 */
GpVars_Verbosity
gpvars_string_to_verbosity(const char *s)
{
	GpVars_Verbosity result;

	if (!s ||
        !s[0] ||
        !pg_strcasecmp("terse", s))
		result = GPVARS_VERBOSITY_TERSE;
	else if (!pg_strcasecmp("off", s))
		result = GPVARS_VERBOSITY_OFF;
	else if (!pg_strcasecmp("verbose", s))
		result = GPVARS_VERBOSITY_VERBOSE;
	else if (!pg_strcasecmp("debug", s))
		result = GPVARS_VERBOSITY_DEBUG;
    else
        result = GPVARS_VERBOSITY_UNDEFINED;
	return result;
}                               /* gpvars_string_to_verbosity */

/*
 * gpvars_verbosity_to_string
 */
const char *
gpvars_verbosity_to_string(GpVars_Verbosity verbosity)
{
	switch (verbosity)
	{
		case GPVARS_VERBOSITY_OFF:
			return "off";
		case GPVARS_VERBOSITY_TERSE:
			return "terse";
		case GPVARS_VERBOSITY_VERBOSE:
			return "verbose";
		case GPVARS_VERBOSITY_DEBUG:
			return "debug";
		default:
			return "*undefined*";
	}
}                               /* gpvars_verbosity_to_string */


/*
 * gpvars_assign_gp_log_gangs
 * gpvars_show_gp_log_gangs
 */
const char *
gpvars_assign_gp_log_gang(const char *newval, bool doit, GucSource source)
{
	GpVars_Verbosity v = gpvars_string_to_verbosity(newval);

	if (v == GPVARS_VERBOSITY_UNDEFINED)
        return NULL;
	if (doit)
		gp_log_gang = v;
	return newval;
}                               /* gpvars_assign_gp_log_gangs */

const char *
gpvars_show_gp_log_gang(void)
{
	return gpvars_verbosity_to_string(gp_log_gang);
}                               /* gpvars_show_gp_log_gangs */


/*
 * gpvars_assign_gp_log_interconnect
 * gpvars_show_gp_log_interconnect
 */
const char *
gpvars_assign_gp_log_interconnect(const char *newval, bool doit, GucSource source)
{
	GpVars_Verbosity v = gpvars_string_to_verbosity(newval);

	if (v == GPVARS_VERBOSITY_UNDEFINED)
        return NULL;
	if (doit)
		gp_log_interconnect = v;
	return newval;
}                               /* gpvars_assign_gp_log_interconnect */

const char *
gpvars_show_gp_log_interconnect(void)
{
	return gpvars_verbosity_to_string(gp_log_interconnect);
}                               /* gpvars_show_gp_log_interconnect */


