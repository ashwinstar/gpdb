/*-------------------------------------------------------------------------
 *
 * cdbvars.c
 *	  Provides storage areas and processing routines for MPP variables
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



MppRoleValue Mpp_role;			/* Role paid by this MPP backend */
char	   *mpp_role_string;	/* Staging area for guc.c */
char	   *mpp_fault_action_string;	/* Staging area for guc.c */
bool		mpp_set_read_only;	/* Staging area for guc.c */

MppRoleValue Mpp_session_role;	/* Role paid by this MPP backend */
char	   *mpp_session_role_string;	/* Staging area for guc.c */

bool		Mpp_is_primary;		/* Is this session primary (or mirror) */

bool		Mpp_is_writer; 		/* is this qExec a "writer" process. */

int 		mpp_session_id;    /* global unique id for session. */

char       *mpp_qd_callback_info;	/* info for QE to call back to QD */

bool		mpp_enable_functions; 	/* allow functions that call back from QE to QD */

bool 		mpp_is_callback;		/* are we executing a callback query? */

unsigned long mpp_qd_proc_offset;	/* the proc entry for the original backend on the QD */

int         mpp_command_count;          /* num of commands from client */

bool		Debug_print_prelim_plan;	/* Shall we log argument of
										 * cdbparallelize? */

bool		Debug_print_slice_table;	/* Shall we log the slice table? */

int			mpp_connections_per_thread; /* How many libpq connections are
										 * handled in each thread */


int			Mpp_segment = UNDEF_SEGMENT;		/* What content this QE is
												 * handling. */

bool		Mpp_write_shared_snapshot;	/* tell the writer QE to write the
										 * shared snapshot */

bool		mpp_reraise_signal=true;	/* try to dump core when we get SIGABRT & SIGSEGV */

bool		mpp_set_proc_affinity=false; /* set processor affinity (if platform supports it) */

/*
 * TCP port the Interconnect listens on for incoming connections from other
 * backends.  Assigned by initMotionLayerIPC() at process startup.  This port
 * is used for the duration of this process and should never change.
 */
int			Mpp_listener_port;

int			Mpp_max_packet_size;	/* max Interconnect packet size */

int			interconnect_setup_timeout;


/*
 * MPP Optimizer GUCs
 */

bool        enable_adaptive_nestloop = true;
double      mpp_motion_cost_per_row = 0;
int         mpp_segments_for_planner = 0;
bool		enable_agg_restruct = TRUE;
double		mpp_hashagg_rewrite_limit = 1;

bool		mpp_enable_motion_deadlock_sanity = FALSE; /* planning time sanity check */

/* MPP Experimental Feature GUCs */
bool		mpp_enable_agg_distinct = FALSE;
bool		mpp_enable_preunique = FALSE;
bool		mpp_enable_explain_allstat = FALSE;
bool		mpp_enable_sort_limit = FALSE;
bool		mpp_enable_sort_distinct = FALSE;
int			mpp_sort_flags = 0;


/* --------------------------------------------------------------------------------------------------
 * Server debugging
 */

/*
 * mpp_debug_linger (integer)
 *
 * Upon an error with severity FATAL and error code ERRCODE_INTERNAL_ERROR,
 * errfinish() will sleep() for the specified number of seconds before
 * termination, to let the user attach a debugger.
 */
int  mpp_debug_linger = 30;


/* ----------------
 * Non-GUC globals
 */

int			currentSliceId = UNSET_SLICE_ID;	/* used by elog to show the
												 * current slice the process
												 * is executing. */
SeqServerControlBlock *seqServerCtl;

/* Segment id where singleton gangs are to be dispatched. */
int         mpp_singleton_segindex;


/* ----------------
 * The single row from mpp_mpp for this database is held in this variable,
 * which is initialized in cdbutil.c in the function
 * getMppMpp(), which is called from cdblink_setup()
 * for both dispatch and execute modes.
 *
 * Any code needing the  "numsegments"
 * can simply #include cdbvars.h, and use MppIdentity.numsegments
 */
MppMpp		MppIdentity = {NULL, 0, -1, -999};


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
MppRoleValue string_to_role(const char *string);
const char *role_to_string(MppRoleValue role);


/*
 * Convert a MPP role string (as for mpp_session_role or mpp_role) to an
 * enum value of type MppRoleValue. Return MPP_ROLE_UNDEFINED in case the
 * string is unrecognized.
 */
MppRoleValue
string_to_role(const char *string)
{
	MppRoleValue role = MPP_ROLE_UNDEFINED;

	if (pg_strcasecmp(string, "dispatch") == 0 || pg_strcasecmp(string, "") == 0)
	{
		role = MPP_ROLE_DISPATCH;
	}
	else if (pg_strcasecmp(string, "execute") == 0)
	{
		role = MPP_ROLE_EXECUTE;
	}
	else if (pg_strcasecmp(string, "utility") == 0)
	{
		role = MPP_ROLE_UTILITY;
	}

	return role;
}

/*
 * Convert a MppRoleValue to a role string (as for mpp_session_role or
 * mpp_role).  Return eyecatcher in the unexpected event that the value
 * is unknown or undefined.
 */
const char *
role_to_string(MppRoleValue role)
{
	switch (role)
	{
		case MPP_ROLE_DISPATCH:
			return "dispatch";
		case MPP_ROLE_EXECUTE:
			return "execute";
		case MPP_ROLE_UTILITY:
			return "utility";
		case MPP_ROLE_UNDEFINED:
		default:
			return "*undefined*";
	}
}


/*
 * Assign hook routine for "mpp_session_role" option.  Because this variable
 * has context PGC_BACKEND, we expect this assigment to happen only during
 * setup of a BACKEND, e.g., based on the role value specified on the connect
 * request.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_mpp_session_role(const char *newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_mpp_session_role: mpp_session_role=%s, newval=%s, doit=%s",
		 show_mpp_session_role(), newval, (doit ? "true" : "false"));
#endif

	MppRoleValue newrole = string_to_role(newval);

	if (newrole == MPP_ROLE_UNDEFINED)
	{
		return NULL;
	}

	if (doit)
	{
		Mpp_session_role = newrole;
		Mpp_role = Mpp_session_role;

		if (Mpp_role == MPP_ROLE_DISPATCH)
			Mpp_segment = -1;
	}
	return newval;
}



/*
 * Assign hook routine for "mpp_role" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set using an option on postmaster start, but this isn't
 * interesting beccause the derived global CdbRole is always set (along with
 * CdbSessionRole) on backend startup for a new connection.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
assign_mpp_role(const char *newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_mpp_role: mpp_role=%s, newval=%s, doit=%s",
		 show_mpp_role(), newval, (doit ? "true" : "false"));
#endif


	MppRoleValue newrole = string_to_role(newval);
	MppRoleValue oldrole = Mpp_role;

	if (newrole == MPP_ROLE_UNDEFINED)
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

		if (Mpp_role != newrole && IsUnderPostmaster)
		{
			if (Mpp_role != MPP_ROLE_UTILITY)
				do_disconnect = true;

			if (newrole != MPP_ROLE_UTILITY)
				do_connect = true;
		}

		if (do_disconnect)
			cdb_cleanup(0, (Datum) NULL);

		Mpp_role = newrole;

		if (source != PGC_S_DEFAULT)
		if (do_connect)
		{
			/*
			 * In case there are problems with the MPP tables or data,
			 * we catch any error coming out of cdblink_setup so we can set the
			 * mpp_role back to what it was.  Otherwise we may be left with
			 * inappropriate connections for the new role.
			 */
			PG_TRY();
			{
				cdb_setup();
			}
			PG_CATCH();
			{
				cdb_cleanup(0, (Datum) NULL);
				Mpp_role = oldrole;
				if (Mpp_role != MPP_ROLE_UTILITY)
					cdb_setup();
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
	}

	return newval;
}


/*
 * Assign hook routine for "mpp_connections_per_thread" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set in config file, but not inside of PGOPTIONS.)
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
bool
assign_mpp_connections_per_thread(int newval, bool doit, GucSource source)
{

#if FALSE
	elog(DEBUG1, "assign_mpp_connections_per_thread: mpp_connections_per_thread=%s, newval=%d, doit=%s",
	   show_mpp_connections_per_thread(), newval, (doit ? "true" : "false"));
#endif

	if (doit)
	{
		if (newval < 1)
			return false;

		mpp_connections_per_thread = newval;
	}

	return true;
}

/*
 * Assign hook routine for "mpp_interconnect_setup_timeout" option.
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
 * Show hook routine for "mpp_session_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_mpp_session_role(void)
{
	return role_to_string(Mpp_session_role);
}


/*
 * Show hook routine for "mpp_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_mpp_role(void)
{
	return role_to_string(Mpp_role);
}

/*
 * Show hook routine for "mpp_connections_per_thread" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_mpp_connections_per_thread(void)
{
	/*
	 * We rely on the fact that the memory context will clean up the memory
	 * for the buffer.data.
	 */
	StringInfoData buffer;

	initStringInfo(&buffer);

	appendStringInfo(&buffer, "%d", mpp_connections_per_thread);

	return buffer.data;
}

/*
 * Show hook routine for "mpp_interconnect_setup_timeout" option.
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
 * mpp_log_gangs (string)
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
CdbVars_Verbosity   mpp_log_gang;

/*
 * mpp_log_interconnect (string)
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
CdbVars_Verbosity   mpp_log_interconnect;


/*
 * cdbvars_string_to_verbosity
 */
CdbVars_Verbosity
cdbvars_string_to_verbosity(const char *s)
{
	CdbVars_Verbosity result;

	if (!s ||
        !s[0] ||
        !pg_strcasecmp("terse", s))
		result = CDBVARS_VERBOSITY_TERSE;
	else if (!pg_strcasecmp("off", s))
		result = CDBVARS_VERBOSITY_OFF;
	else if (!pg_strcasecmp("verbose", s))
		result = CDBVARS_VERBOSITY_VERBOSE;
	else if (!pg_strcasecmp("debug", s))
		result = CDBVARS_VERBOSITY_DEBUG;
    else
        result = CDBVARS_VERBOSITY_UNDEFINED;
	return result;
}                               /* cdbvars_string_to_verbosity */

/*
 * cdbvars_verbosity_to_string
 */
const char *
cdbvars_verbosity_to_string(CdbVars_Verbosity verbosity)
{
	switch (verbosity)
	{
		case CDBVARS_VERBOSITY_OFF:
			return "off";
		case CDBVARS_VERBOSITY_TERSE:
			return "terse";
		case CDBVARS_VERBOSITY_VERBOSE:
			return "verbose";
		case CDBVARS_VERBOSITY_DEBUG:
			return "debug";
		default:
			return "*undefined*";
	}
}                               /* cdbvars_verbosity_to_string */


/*
 * cdbvars_assign_mpp_log_gangs
 * cdbvars_show_mpp_log_gangs
 */
const char *
cdbvars_assign_mpp_log_gang(const char *newval, bool doit, GucSource source)
{
	CdbVars_Verbosity v = cdbvars_string_to_verbosity(newval);

	if (v == CDBVARS_VERBOSITY_UNDEFINED)
        return NULL;
	if (doit)
		mpp_log_gang = v;
	return newval;
}                               /* cdbvars_assign_mpp_log_gangs */

const char *
cdbvars_show_mpp_log_gang(void)
{
	return cdbvars_verbosity_to_string(mpp_log_gang);
}                               /* cdbvars_show_mpp_log_gangs */


/*
 * cdbvars_assign_mpp_log_interconnect
 * cdbvars_show_mpp_log_interconnect
 */
const char *
cdbvars_assign_mpp_log_interconnect(const char *newval, bool doit, GucSource source)
{
	CdbVars_Verbosity v = cdbvars_string_to_verbosity(newval);

	if (v == CDBVARS_VERBOSITY_UNDEFINED)
        return NULL;
	if (doit)
		mpp_log_interconnect = v;
	return newval;
}                               /* cdbvars_assign_mpp_log_interconnect */

const char *
cdbvars_show_mpp_log_interconnect(void)
{
	return cdbvars_verbosity_to_string(mpp_log_interconnect);
}                               /* cdbvars_show_mpp_log_interconnect */


