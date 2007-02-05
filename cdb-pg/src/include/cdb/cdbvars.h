/*-------------------------------------------------------------------------
 *
 * cdbvars.h
 *	  definitions for MPP global variables
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

#ifndef CDBVARS_H
#define CDBVARS_H

#include "utils/guc.h"

/*
 * ----- Declarations of MPP global variables ------
 */

/*
 * Parameters mpp_session_role and mpp_role
 *
 * The run-time parameters (GUC variables) mpp_session_role and
 * mpp_role report and provide control over the role assumed by a
 * postgres process.
 *
 * Valid  roles are the following:
 *
 *	dispatch	The process acts as a parallel SQL dispatcher.
 *	execute		The process acts as a parallel SQL executor.
 *	utility		The process acts as a simple SQL engine.
 *
 * Both parameters are initialized to the same value at connection
 * time and are local to the backend process resulting from the
 * connection.	The default is dispatch which is the normal setting
 * for a user of MPP connecting to a node of  the MPP cluster.
 * Neither parameter appears in the configuration file.
 *
 * mpp_session_role
 *
 * - does not affect the operation of the backend, and
 * - does not change during the lifetime of PostgreSQL session.
 *
 * mpp_role
 *
 * - determines the operating role of the backend, and
 * - may be changed by a superuser via the SET command.
 *
 * The connection time value of mpp_session_role used by a
 * libpq-based client application can be specified using the
 * environment variable PGOPTIONS.	For example, this is how to
 * invoke psql on database test as a utility:
 *
 *	PGOPTIONS='-c mpp_session_role=utility' psql test
 *
 * Alternatively, libpq-based applications can be modified to set
 * the value directly via an argument to one of the libpq functions
 * PQconnectdb, PQsetdbLogin or PQconnectStart.
 *
 * Don't try to set mpp_role this way.  At the time options are
 * processed it is unknown whether the user is a superuser, so
 * the attempt will be rejected.
 *
 * ----------
 *
 * MPP Developers can access the values of these parameters via the
 * global variables Mpp_session_role and Mpp_role of type
 * MppRoleValue. For example
 *
 *	#include "cdb/cdbvars.h"
 *
 *	switch ( Mpp_role  )
 *	{
 *		case MPP_ROLE_DISPATCH:
 *			... Act like a query dispatcher ...
 *			break;
 *		case MPP_ROLE_EXECUTE:
 *			... Act like a query executor ...
 *			break;
 *		case MPP_ROLE_UTILITY:
 *			... Act like an unmodified PostgreSQL backend. ...
 *			break;
 *		default:
 *			... Won't happen ..
 *			break;
 *	}
 *
 * You can also modify Mpp_role (even if the session doesn't have
 * superuser privileges) by setting it to one of the three valid
 * values, however this must be well documented to avoid
 * disagreements between modules.  Don't modify the value  of
 * Mpp_session_role.
 *
 */

typedef enum
{
	MPP_ROLE_UTILITY = 0,		/* Operating as a simple database engine */
	MPP_ROLE_DISPATCH,			/* Operating as the parallel query dispatcher */
	MPP_ROLE_EXECUTE,			/* Operating as a parallel query executor */
	MPP_ROLE_UNDEFINED			/* Should never see this role in use */
} MppRoleValue;


extern MppRoleValue Mpp_session_role;	/* GUC var - server startup mode.  */
extern char *mpp_session_role_string;	/* Use by guc.c as staging area for
										 * value. */
extern const char *assign_mpp_session_role(const char *newval, bool doit, GucSource source);
extern const char *show_mpp_session_role(void);

extern MppRoleValue Mpp_role;	/* GUC var - server operating mode.  */
extern char *mpp_role_string;	/* Use by guc.c as staging area for value. */
extern const char *assign_mpp_role(const char *newval, bool doit, GucSource source);
extern const char *show_mpp_role(void);

/* Parameter mpp_is_primary
 *
 * This run_time parameter indicates whether session is associated with the
 * primary copy of its database's content or with a mirror.  It defaults to
 * true (primary), but may be specified as false using a connect option, e.g,
 *
 *	  PGOPTIONS='-c mpp_is_primary=false' psql test
 *
 * Though it will ordinarily be set by a QD connecting to a QE, rather than
 * directly.  It may not be reset in a running session.
 */
extern bool Mpp_is_primary;

/* Parameter Mpp_is_writer
 *
 * This run_time parameter indicates whether session is a qExec that is a
 * "writer" process.  Only writers are allowed to write changes to the database
 * and currently there is only one writer per group of segmates.
 *
 * It defaults to false, but may be specified as true using a connect option.
 * This should only ever be set by a QD connecting to a QE, rather than
 * directly.
 */
extern bool Mpp_is_writer;

/* Parameter mpp_session_id
 *
 * This run time parameter indicates a unique id to identify a particular user
 * session throughout the entire MPP array.
 */
extern int mpp_session_id;


/* Parameter mpp_qd_callback_info
 *
 * This is the port of the QD, sent down to the QEs
 */

extern char * mpp_qd_callback_info;
extern bool mpp_use_snapshop_during_callback;
extern unsigned long mpp_qd_proc_offset;

/*
 * Allow callback query?
 */
extern bool mpp_enable_functions;
/*
 * Are we executing a callback query?
 */
extern bool mpp_is_callback;

/* Parameter debug_print_prelim_plan
 *
 * This run-time parameter is closely related to the PostgreSQL parameter
 * debug_print_plan which, if true, causes the final plan to display on the
 * server log prior to execution.  This parameter, if true, causes the
 * preliminary plan (from the optimizer prior to cdbparallelize) to display
 * on the log.
 */
extern bool Debug_print_prelim_plan;

/* Parameter debug_print_slice_table
 *
 * This run-time parameter, if true, causes the slice table for a plan to
 * display on the server log.  On a QD this occurs prior to sending the
 * slice table to any QE.  On a QE it occurs just prior to execution of
 * the plan slice.
 */
extern bool Debug_print_slice_table;


/*
 * mpp_command_count
 *
 * This GUC is 0 at the beginning of a client session and
 * increments for each command received from the client.
 */
extern int mpp_command_count;

/*
 * Mpp_write_shared_snapshot
 *
 * The value of this variable is actually meaningless. We use it simply
 * in order to have a mechanism of dispatching a SET query that will
 * allocate writer gangs that will write the shared snapshot and make
 * it available for the reader gangs. This is currently only needed
 * before DECLARE'ing a cursor, since a cursor query uses only reader
 * gangs. But should be used for other things if it makes sense to do so.
 */
extern bool Mpp_write_shared_snapshot;

/*
 * Parameter mpp_connections_per_thread
 *
 * The run-time parameter (GUC variables) mpp_connections_per_thread
 * controls how many libpq connections to qExecs are processed in each
 * thread.
 *
 * Any number >= 1 is valid.
 *
 * 1 means each connection has its own thread.
 *
 * This can be set in the config file, or at runtime by a superuser using
 * SQL: set mpp_connections_per_thread = x;
 *
 * The default is 256.	So, if there are fewer than 256 segdbs, all would be handled
 * by the same thread.
 *
 * Currently, this is used in two situation:
 *		1) In cdblink_setup, when the libpq connections are obtained by the dispatcher
 *				to the qExecs.
 *		2) In CdbDispatchCommand, when commands are sent from the dispatcher to the qExecs.
 *
 */
extern int	mpp_connections_per_thread; /* GUC var - server operating mode.  */
extern bool assign_mpp_connections_per_thread(int newval, bool doit, GucSource source);
extern const char *show_mpp_connections_per_thread(void);
extern bool assign_interconnect_setup_timeout(int newval, bool doit, GucSource source);
extern const char *show_interconnect_setup_timeout(void);
extern bool assign_mpp_write_shared_snapshot(bool newval, bool doit, GucSource source);

extern char *mpp_fault_action_string;	/* Use by guc.c as user defined fault
										 * action */
extern bool mpp_set_read_only;
extern bool assign_mpp_set_read_only(bool newval, bool doit, GucSource source);
extern const char *show_mpp_set_read_only(void);
extern const char *role_to_string(MppRoleValue role);

extern int	mpp_segment_connect_timeout; /* GUC var - timeout specifier for gang creation */

/*
 * Parameter Mpp_max_packet_size
 *
 * The run-time parameter mpp_max_packet_size controls the largest packet
 * size that the interconnect will transmit.  In general, the interconnect
 * tries to squeeze as many tuplechunks as possible into a packet without going
 * above this size limit.
 *
 * The largest tuple chunk size that the system will form can be derived
 * from this with:
 *		MAX_CHUNK_SIZE = Mpp_max_packet_size - PACKET_HEADER_SIZE - TUPLE_CHUNK_HEADER_SIZE
 *
 *
 */
extern int	Mpp_max_packet_size;	/* GUC var */

#define DEFAULT_PACKET_SIZE 8192
#define MIN_PACKET_SIZE 512
#define MAX_PACKET_SIZE 65536

/*
 * Parameter mpp_segment
 *
 * The segment (content) controlled by this QE
 * content indicator: -1 for entry database, 0, ..., n-1 for segment database
 *
 * This variable is in the PGC_BACKEND context; it is set internally by the
 * Query Dispatcher, and is propagated to Query Executors in the connection
 * parameters.	It is not modifiable by any user, ever.
 */
#define UNDEF_SEGMENT -2
extern int	Mpp_segment;		/* GUC var */

extern int	getMPPSegmentCount(void);


/*
 * Parameter interconnect_setup_timeout
 *
 * The run-time parameter (GUC variable) interconnect_setup_timeout is used
 * during SetupInterconnect() to timeout the operation.  This value is in
 * seconds.  Setting it to zero effectively disables the timeout.
 */
extern int	interconnect_setup_timeout;


/* --------------------------------------------------------------------------------------------------
 * Logging
 */

typedef enum CdbVars_Verbosity
{
	CDBVARS_VERBOSITY_UNDEFINED = 0,
    CDBVARS_VERBOSITY_OFF,
	CDBVARS_VERBOSITY_TERSE,	
	CDBVARS_VERBOSITY_VERBOSE,
    CDBVARS_VERBOSITY_DEBUG,
} CdbVars_Verbosity;

CdbVars_Verbosity
cdbvars_string_to_verbosity(const char *s);
const char *
cdbvars_verbosity_to_string(CdbVars_Verbosity verbosity);

/*
 * mpp_log_gang (string)
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
extern CdbVars_Verbosity    mpp_log_gang;

const char *
cdbvars_assign_mpp_log_gang(const char *newval, bool doit, GucSource source);
const char *
cdbvars_show_mpp_log_gang(void);

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
extern CdbVars_Verbosity    mpp_log_interconnect;

const char *
cdbvars_assign_mpp_log_interconnect(const char *newval, bool doit, GucSource source);
const char *
cdbvars_show_mpp_log_interconnect(void);


/* --------------------------------------------------------------------------------------------------
 * MPP Optimizer GUCs
 */


/*
 * enable_adaptive_nestloop
 */
extern bool enable_adaptive_nestloop;

/*
 * "mpp_motion_cost_per_row"
 *
 * If >0, the planner uses this value -- instead of 2 * "cpu_tuple_cost" --
 * for Motion operator cost estimation.
 */
extern double   mpp_motion_cost_per_row;

/*
 * "mpp_segments_for_planner"
 *
 * If >0, number of segment dbs for the planner to assume in its cost and size
 * estimates.  If 0, estimates are based on the actual number of segment dbs.
 */
extern int      mpp_segments_for_planner;

/*
 * Unlike some other enable... vars, enable_agg_restruct is not cost based.
 * When set to false, the planner will not use two-stage aggregation.
 */
extern bool enable_agg_restruct;

/* 
 * Perform a post-planning scan of the final plan looking for motion deadlocks:
 * emit verbose messages about any found.
 */
extern bool mpp_enable_motion_deadlock_sanity;

/* ----- Experimental Features ----- */

/* May MPP redistribute on the argument of a lone aggregate distinct in
 * order to use 2-phase aggregation?
 *
 * The code does not currently use planner estimates for this.  If enabled,
 * the tactic is used whenever possible.
 */
extern bool mpp_enable_agg_distinct;

/* May MPP apply Unique operator (and possibly a Sort) in parallel prior
 * to the collocation motion for a Unique operator?  The idea is to reduce
 * the number of rows moving over the interconnect.
 *
 * The code does not currently use planner estimates for this.  If enabled,
 * the tactic is used whenever possible.
 */
extern bool mpp_enable_preunique;

/* May MPP dump statistics for all segments as a huge ugly string
 * during EXPLAIN ANALYZE? 
 *
 */
extern bool mpp_enable_explain_allstat;

/* May MPP restrict ORDER BY sorts to the first N rows if the ORDER BY
 * is wrapped by a LIMIT clause (where N=OFFSET+LIMIT)?
 *
 * The code does not currently use planner estimates for this.  If enabled,
 * the tactic is used whenever possible.
 */
extern bool mpp_enable_sort_limit;

/* May MPP discard duplicate rows in sort if it is is wrapped by a
 * DISTINCT clause (unique aggregation operator)?
 *
 * The code does not currently use planner estimates for this.  If enabled,
 * the tactic is used whenever possible.
 */
extern bool mpp_enable_sort_distinct;

/* Generic MPP sort flag for testing.
 *
 *
 */
extern int mpp_sort_flags;

/* The maximum number of times on average that the hybrid hashed aggregation
 * algorithm will plan to spill an input row to disk before including it in
 * an aggregation.  Increasing this parameter will cause the planner to choose 
 * hybrid hashed aggregation at lower settings of work_mem than it otherwise
 * would.
 */
extern double mpp_hashagg_rewrite_limit;


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
extern int  mpp_debug_linger;


/* --------------------------------------------------------------------------------------------------
 * Non-GUC globals
 */

#define UNSET_SLICE_ID -1
extern int	currentSliceId;

/* Segment id where singleton gangs are to be dispatched. */
extern int  mpp_singleton_segindex;


/* --------------------------------------------------------------------------------------------------
 * Structure for row of mpp.mpp table
 */
typedef struct MppMpp
{
	char	   *mppname;		/* array of SegmentDatabaseInfo's for segment
								 * databases */
	int4		numsegments;	/* count of distinct segindexes */
	int4		dbid;			/* the dbid of this database */
	int4		segindex;		/* content indicator: -1 for entry database,
								 * 0, ..., n-1 for segment database */
} MppMpp;

/* --------------------------------------------------------------------------------------------------
 * Global variable declaration for the data for the single row of mpp.mpp table
 */
extern MppMpp MppIdentity;


/* Stores the listener port that this process uses to listen for incoming
 * Interconnect connections from other Motion nodes.
 */
extern int	Mpp_listener_port;



/* SequenceServer information to be shared with everyone */
typedef struct SeqServerControlBlock
{
	int4		seqServerPort;
}	SeqServerControlBlock;

extern SeqServerControlBlock *seqServerCtl;



#endif   /* CDBVARS_H */
