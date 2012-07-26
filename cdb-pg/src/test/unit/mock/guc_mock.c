/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/guc.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>
#include <signal.h>
#include "access/gin.h"
#include "access/transam.h"
#include "access/aosegfiles.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbfilerep.h"
#include "cdb/tupchunk.h"
#include "commands/async.h"
#include "commands/vacuum.h"
#include "commands/variable.h"
#include "executor/executor.h"	/* TupOutputState, TupleDesc */
#include "executor/execWorkfile.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "libpq/password_hash.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/gramparse.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgwriter.h"
#include "postmaster/checkpoint.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/backoff.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/bfz.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/gp_rand.h"
#include "utils/guc_tables.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"
#include "utils/tzparser.h"
#include "utils/resscheduler.h"
#include "pgstat.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "cdb/memquota.h"

List           *gp_guc_list_for_explain;
List           *gp_guc_list_for_no_plan;
bool            log_duration;
bool            Debug_print_plan;
bool            Debug_print_parse;
bool            Debug_print_rewritten;
bool            Debug_pretty_print;
bool            Explain_pretty_print;
bool            Debug_print_full_dtm;
bool            Debug_print_snapshot_dtm;
bool            Debug_print_qd_mirroring;
bool            Debug_permit_same_host_standby;
bool            Debug_print_semaphore_detail;
bool            Debug_disable_distributed_snapshot;
bool            Debug_abort_after_distributed_prepared;
bool            Debug_abort_after_segment_prepared;
bool            Debug_appendonly_print_insert;
bool            Debug_appendonly_print_insert_tuple;
bool            Debug_appendonly_print_scan;
bool            Debug_appendonly_print_scan_tuple;
bool            Debug_appendonly_print_storage_headers;
bool            Debug_appendonly_print_verify_write_block;
bool            Debug_appendonly_use_no_toast;
bool            Debug_appendonly_print_blockdirectory;
bool            Debug_appendonly_print_read_block;
bool            Debug_appendonly_print_append_block;
bool            Debug_appendonly_print_segfile_choice;
bool            Debug_gp_relation_node_fetch_wait_for_debugging;
bool            Debug_delay_before_persistent_init_scan;
bool            gp_crash_recovery_abort_suppress_fatal;
bool            gp_persistent_statechange_suppress_error;
bool            Debug_bitmap_print_insert;
bool            Test_appendonly_override;
bool            Test_enable_broken_quicklz3;
bool            gp_test_orientation_override;
bool            gp_permit_persistent_metadata_update;
bool            gp_permit_relation_node_change;
bool            Test_checksum_override;
int             Test_compresslevel_override;
int             Test_blocksize_override;
int             Test_safefswritesize_override;
bool            Master_mirroring_administrator_disable;
int             gp_max_local_distributed_cache;
bool            gp_local_distributed_cache_stats;
bool            gp_appendonly_verify_block_checksums;
bool            gp_appendonly_verify_write_block;
bool            gp_heap_require_relhasoids_match;
bool            gp_suppress_distributed_tx_vacuum;
bool            gp_new_reindex_database;
bool            Debug_appendonly_rezero_quicklz_compress_scratch;
bool            Debug_appendonly_rezero_quicklz_decompress_scratch;
bool            Debug_appendonly_guard_end_quicklz_scratch;
bool            Debug_xlog_insert_print;
bool            Debug_persistent_print;
int             Debug_persistent_print_level;
bool            Debug_persistent_recovery_print;
int             Debug_persistent_recovery_print_level;
bool            Debug_persistent_store_print;
bool            Debug_persistent_bootstrap_print;
bool            Debug_bulk_load_bypass_wal;
bool            Debug_persistent_appendonly_commit_count_print;
bool            Debug_cancel_print;
bool            Debug_datumstream_write_print_small_varlena_info;
bool            Debug_datumstream_write_print_large_varlena_info;
bool            Debug_datumstream_read_check_large_varlena_integrity;
bool            Debug_datumstream_block_read_check_integrity;
bool            Debug_datumstream_block_write_check_integrity;
bool            Debug_datumstream_read_print_varlena_info;
bool            Debug_datumstream_write_use_small_initial_buffers;
int             Debug_persistent_store_print_level;
bool            Debug_database_command_print;
int             Debug_database_command_print_level;
int             gp_max_databases;
int             gp_max_tablespaces;
int             gp_max_filespaces;
bool            gp_initdb_mirrored;
bool            gp_before_persistence_work;
bool            gp_before_filespace_setup;
bool            gp_startup_integrity_checks;
bool            gp_change_tracking;
bool            gp_persistent_skip_free_list;
bool            gp_persistent_repair_global_sequence;
bool            Debug_print_xlog_relation_change_info;
bool            Debug_print_xlog_relation_change_info_skip_issues_only;
bool            Debug_print_xlog_relation_change_info_backtrace_skip_issues;
bool            Debug_filerep_crc_on;
bool            Debug_filerep_print;
bool            Debug_filerep_gcov;
bool            Debug_filerep_config_print;
bool            Debug_filerep_verify_performance_print;
bool            Debug_filerep_memory_log_flush;
bool            filerep_inject_listener_fault;
bool            filerep_inject_db_startup_fault;
bool            filerep_inject_change_tracking_recovery_fault;
bool            gp_crash_recovery_suppress_ao_eof;
bool            Debug_check_for_invalid_persistent_tid;
bool            gp_watch_visibility_in_action;
bool            gp_upgrade_mode;
bool            gp_maintenance_mode;
bool            gp_maintenance_conn;
bool            allow_segment_DML;
bool            gp_ignore_window_exclude;
int             verify_checkpoint_interval;
bool            Debug_rle_type_compression;
bool            rle_type_compression_stats;
bool            Debug_print_tablespace;
bool            Debug_print_server_processes;
bool            Debug_print_control_checkpoints;
bool            Debug_dtm_action_primary;
bool            log_parser_stats;
bool            log_planner_stats;
bool            log_executor_stats;
bool            log_statement_stats;
bool            log_dispatch_stats;
bool            log_btree_build_stats;
bool            check_function_bodies;
bool            default_with_oids;
bool            SQL_inheritance;
int             log_min_error_statement;
int             log_min_messages;
int             client_min_messages;
int             log_min_duration_statement;
int             num_temp_buffers;
int             ddboost_buf_size;
bool            gp_cancel_query_print_log;
int             gp_cancel_query_delay_time;
bool            gp_perfmon_print_packet_info;
bool            gp_temporary_files_filespace_repair;
bool            gp_partitioning_dynamic_selection_log;
double          gp_thread_dispatch_wait;
bool            gp_plpgsql_clear_cache_always;
int             Debug_dtm_action;
int             Debug_dtm_action_target;
int             Debug_dtm_action_protocol;
int             Debug_dtm_action_delay_ms;
int             Debug_dtm_action_segment;
char           *ConfigFileName;
char           *HbaFileName;
char           *IdentFileName;
char           *external_pid_file;
char           *application_name;
char           *Debug_dtm_action_sql_command_tag;
char           *Debug_dtm_action_str;
char           *Debug_dtm_action_target_str;
char           *Debug_dtm_action_protocol_str;
bool            gp_encoding_check_locale_compatibility;
int             tcp_keepalives_idle;
int             tcp_keepalives_interval;
int             tcp_keepalives_count;
int             gp_filerep_tcp_keepalives_idle;
int             gp_filerep_tcp_keepalives_interval;
int             gp_filerep_tcp_keepalives_count;
int             WalSendClientTimeout;
char           *data_directory;
bool            gp_opt_minidump;
bool            gp_opt_print_query;
bool            gp_opt_print_plan;
bool            gp_opt_print_xform;
bool            gp_opt_disable_xform_result_printing;
bool            gp_opt_print_memo_after_exploration;
bool            gp_opt_print_memo_after_implementation;
bool            gp_opt_print_memo_after_optimization;
bool            gp_opt_print_job_scheduler;
bool            gp_opt_print_expression_properties;
bool            gp_opt_print_group_properties;
bool            gp_opt_print_optimization_context;
bool            gp_opt_print_optimization_stats;
bool            gp_opt_parallel;
bool            gp_opt_local;
int             gp_opt_retries;
char           *gp_email_smtp_server;
char           *gp_email_smtp_userid;
char           *gp_email_smtp_password;
char           *gp_email_from;
char           *gp_email_to;
int             gp_email_connect_timeout;
int             gp_email_connect_failures;
int             gp_email_connect_avoid_duration;
char           *gp_snmp_community;
char           *gp_snmp_monitor_address;
char           *gp_snmp_use_inform_or_trap;
char           *gp_snmp_debug_log;
char           *gp_hadoop_connector_jardir;
char           *gp_hadoop_connector_version;
char           *gp_hadoop_target_version;
char           *gp_hadoop_home;
char           *gp_auth_time_override_str;
void 
SetConfigOption(const char *name, const char *value, GucContext context, GucSource source)
{

    check_expected(name);
    check_expected(value);
    check_expected(context);
    check_expected(source);

    mock();

}

void 
DefineCustomBoolVariable(const char *name, const char *short_desc, const char *long_desc, bool * valueAddr, GucContext context, GucBoolAssignHook assign_hook, GucShowHook show_hook)
{

    check_expected(name);
    check_expected(short_desc);
    check_expected(long_desc);
    check_expected(valueAddr);
    check_expected(context);
    check_expected(assign_hook);
    check_expected(show_hook);
    optional_assignment(valueAddr);
    mock();

}

void 
DefineCustomIntVariable(const char *name, const char *short_desc, const char *long_desc, int *valueAddr, int minValue, int maxValue, GucContext context, GucIntAssignHook assign_hook, GucShowHook show_hook)
{

    check_expected(name);
    check_expected(short_desc);
    check_expected(long_desc);
    check_expected(valueAddr);
    check_expected(minValue);
    check_expected(maxValue);
    check_expected(context);
    check_expected(assign_hook);
    check_expected(show_hook);
    optional_assignment(valueAddr);
    mock();

}

void 
DefineCustomRealVariable(const char *name, const char *short_desc, const char *long_desc, double *valueAddr, double minValue, double maxValue, GucContext context, GucRealAssignHook assign_hook, GucShowHook show_hook)
{

    check_expected(name);
    check_expected(short_desc);
    check_expected(long_desc);
    check_expected(valueAddr);
    check_expected(minValue);
    check_expected(maxValue);
    check_expected(context);
    check_expected(assign_hook);
    check_expected(show_hook);
    optional_assignment(valueAddr);
    mock();

}

void 
DefineCustomStringVariable(const char *name, const char *short_desc, const char *long_desc, char **valueAddr, GucContext context, GucStringAssignHook assign_hook, GucShowHook show_hook)
{

    check_expected(name);
    check_expected(short_desc);
    check_expected(long_desc);
    check_expected(valueAddr);
    check_expected(context);
    check_expected(assign_hook);
    check_expected(show_hook);
    optional_assignment(valueAddr);
    mock();

}

void 
EmitWarningsOnPlaceholders(const char *className)
{

    check_expected(className);

    mock();

}

const char     *
GetConfigOption(const char *name)
{

    check_expected(name);

    return (char *) mock();

}

const char     *
GetConfigOptionResetString(const char *name)
{

    check_expected(name);

    return (char *) mock();

}

bool 
IsSuperuserConfigOption(const char *name)
{

    check_expected(name);

    return (bool) mock();

}

bool 
IsSusetDependOption(const char *name)
{

    check_expected(name);

    return (bool) mock();

}

void 
ProcessConfigFile(GucContext context)
{

    check_expected(context);

    mock();

}

void 
InitializeGUCOptions(void)
{



    mock();

}

bool 
SelectConfigFiles(const char *userDoption, const char *progname)
{

    check_expected(userDoption);
    check_expected(progname);

    return (bool) mock();

}

void 
ResetAllOptions(void)
{



    mock();

}

void 
AtEOXact_GUC(bool isCommit, bool isSubXact)
{

    check_expected(isCommit);
    check_expected(isSubXact);

    mock();

}

void 
BeginReportingGUCOptions(void)
{



    mock();

}

void 
ParseLongOption(const char *string, char **name, char **value)
{

    check_expected(string);
    check_expected(name);
    check_expected(value);
    optional_assignment(name);
    optional_assignment(value);
    mock();

}

bool 
set_config_option(const char *name, const char *value, GucContext context, GucSource source, bool isLocal, bool changeVal)
{

    check_expected(name);
    check_expected(value);
    check_expected(context);
    check_expected(source);
    check_expected(isLocal);
    check_expected(changeVal);

    return (bool) mock();

}

char           *
GetConfigOptionByName(const char *name, const char **varname)
{

    check_expected(name);
    check_expected(varname);

    return (char *) mock();

}

void 
GetConfigOptionByNum(int varnum, const char **values, bool * noshow)
{

    check_expected(varnum);
    check_expected(values);
    check_expected(noshow);
    optional_assignment(noshow);
    mock();

}

int 
GetNumConfigOptions(void)
{



    return (int) mock();

}

void 
SetPGVariable(const char *name, List * args, bool is_local, bool gp_dispatch)
{

    check_expected(name);
    check_expected(args);
    check_expected(is_local);
    check_expected(gp_dispatch);
    optional_assignment(args);
    mock();

}

void 
GetPGVariable(const char *name, DestReceiver * dest)
{

    check_expected(name);
    check_expected(dest);
    optional_assignment(dest);
    mock();

}

TupleDesc 
GetPGVariableResultDesc(const char *name)
{

    check_expected(name);

    return (TupleDesc) mock();

}

void 
ResetPGVariable(const char *name)
{

    check_expected(name);

    mock();

}

char           *
flatten_set_variable_args(const char *name, List * args)
{

    check_expected(name);
    check_expected(args);
    optional_assignment(args);
    return (char *) mock();

}

void 
ProcessGUCArray(ArrayType * array, GucSource source)
{

    check_expected(array);
    check_expected(source);
    optional_assignment(array);
    mock();

}

ArrayType      *
GUCArrayAdd(ArrayType * array, const char *name, const char *value)
{

    check_expected(array);
    check_expected(name);
    check_expected(value);
    optional_assignment(array);
    return (ArrayType *) mock();

}

ArrayType      *
GUCArrayDelete(ArrayType * array, const char *name)
{

    check_expected(array);
    check_expected(name);
    optional_assignment(array);
    return (ArrayType *) mock();

}

ArrayType      *
GUCArrayReset(ArrayType * array)
{

    check_expected(array);
    optional_assignment(array);
    return (ArrayType *) mock();

}

void 
pg_timezone_abbrev_initialize(void)
{



    mock();

}

int 
gp_guc_list_show(struct StringInfoData * buf, const char *pfx, const char *fmt, GucSource excluding, List * guclist)
{

    check_expected(buf);
    check_expected(pfx);
    check_expected(fmt);
    check_expected(excluding);
    check_expected(guclist);
    optional_assignment(buf);
    optional_assignment(guclist);
    return (int) mock();

}

const char     *
assign_default_tablespace(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
assign_regex_flavor(const char *value, bool doit, GucSource source)
{

    check_expected(value);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
assign_search_path(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
assign_xlog_sync_method(const char *method, bool doit, GucSource source)
{

    check_expected(method);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}
