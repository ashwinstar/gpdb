/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbvars.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "catalog/gp_segment_config.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbutil.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "utils/memutils.h"
#include "storage/bfz.h"
#include "cdb/memquota.h"

GpRoleValue     Gp_session_role;
char           *gp_session_role_string;
const char     *
assign_gp_session_role(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
show_gp_session_role(void)
{



    return (char *) mock();

}

GpRoleValue     Gp_role;
char           *gp_role_string;
const char     *
assign_gp_role(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
show_gp_role(void)
{



    return (char *) mock();

}

bool            gp_reraise_signal;
bool            gp_version_mismatch_error;
bool            gp_set_proc_affinity;
bool            Gp_is_writer;
int             gp_session_id;
char           *qdHostname;
int             qdPostmasterPort;
char           *gp_qd_callback_info;
bool            gp_use_snapshop_during_callback;
unsigned long   gp_qd_proc_offset;
bool            gp_enable_functions;
bool            gp_is_callback;
bool            gp_use_dispatch_agent;
int             gp_cached_gang_threshold;
int             gp_reject_percent_threshold;
int             gp_max_csv_line_length;
bool            gp_select_invisible;
bool            gp_enable_slow_writer_testmode;
bool            gp_enable_slow_cursor_testmode;
bool            gp_enable_delete_as_truncate;
bool            gp_eager_hashtable_release;
bool            Debug_print_combocid_detail;
bool            gp_debug_pgproc;
bool            Debug_print_prelim_plan;
bool            Debug_print_slice_table;
bool            Debug_print_dispatch_plan;
bool            Debug_print_plannedstmt;
bool            gp_backup_directIO;
int             gp_backup_directIO_read_chunk_mb;
bool            gp_external_enable_exec;
bool            gp_external_grant_privileges;
int             gp_external_max_segs;
int             gp_command_count;
int             gp_safefswritesize;
bool            Gp_write_shared_snapshot;
bool            gp_fts_probe_pause;
int             gp_fts_transition_retries;
int             gp_fts_transition_timeout;
bool 
gpvars_assign_gp_fts_probe_pause(bool newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

int             gp_connections_per_thread;
bool 
assign_gp_connections_per_thread(int newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

const char     *
show_gp_connections_per_thread(void)
{



    return (char *) mock();

}

int32           gp_subtrans_warn_limit;
bool 
assign_gp_write_shared_snapshot(bool newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

char           *gp_fault_action_string;
bool            gp_set_read_only;
bool 
assign_gp_use_dispatch_agent(bool newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

const char     *
role_to_string(GpRoleValue role)
{

    check_expected(role);

    return (char *) mock();

}

int             gp_segment_connect_timeout;
int             gp_snapshotadd_timeout;
int             gp_fts_probe_retries;
int             gp_fts_probe_timeout;
int             gp_fts_probe_interval;
int             gp_fts_probe_threadcount;
bool            gp_fts_transition_parallel;
int             gp_log_retry_count;
int             gp_gang_creation_retry_count;
int             gp_gang_creation_retry_timer;
int             Gp_max_packet_size;
int             Gp_interconnect_type;
const char     *
gpvars_assign_gp_interconnect_type(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_interconnect_type(void)
{



    return (char *) mock();

}

int             Gp_interconnect_queue_depth;
int             Gp_interconnect_snd_queue_depth;
int             Gp_udp_bufsize_k;
int             Gp_interconnect_hash_multiplier;
bool            gp_interconnect_aggressive_retry;
bool            gp_interconnect_full_crc;
bool            gp_interconnect_elide_setup;
bool            gp_interconnect_log_stats;
int             Gp_segment;
int 
getgpsegmentCount(void)
{



    return (int) mock();

}

int             interconnect_setup_timeout;
int             gp_udpic_dropseg;
int             gp_udpic_dropxmit_percent;
int             gp_udpic_dropacks_percent;
int             gp_udpic_fault_inject_percent;
int             gp_udpic_fault_inject_bitmap;
int             gp_udpic_network_disable_ipv6;
uint32          gp_interconnect_id;
GpVars_Verbosity 
gpvars_string_to_verbosity(const char *s)
{

    check_expected(s);

    return (GpVars_Verbosity) mock();

}

const char     *
gpvars_verbosity_to_string(GpVars_Verbosity verbosity)
{

    check_expected(verbosity);

    return (char *) mock();

}

bool            gp_enable_fast_sri;
bool            gp_enable_direct_dispatch;
GpVars_Verbosity gp_log_gang;
const char     *
gpvars_assign_gp_log_gang(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_log_gang(void)
{



    return (char *) mock();

}

GpVars_Verbosity gp_log_fts;
const char     *
gpvars_assign_gp_log_fts(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_log_fts(void)
{



    return (char *) mock();

}

GpVars_Verbosity gp_log_interconnect;
const char     *
gpvars_assign_gp_log_interconnect(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_log_interconnect(void)
{



    return (char *) mock();

}

double          gp_process_memory_cutoff;
bool            enable_adaptive_nestloop;
double          gp_motion_cost_per_row;
int             gp_segments_for_planner;
bool            gp_enable_multiphase_agg;
bool            fast_path_expressions;
bool            gp_enable_motion_deadlock_sanity;
bool            gp_adjust_selectivity_for_outerjoins;
int             gp_hashjoin_tuples_per_bucket;
int             gp_hashagg_groups_per_bucket;
bool            gp_selectivity_damping_for_scans;
bool            gp_selectivity_damping_for_joins;
double          gp_selectivity_damping_factor;
bool            gp_selectivity_damping_sigsort;
bool            gp_enable_inheritance;
bool            gp_enable_agg_distinct;
bool            gp_enable_dqa_pruning;
bool            gp_eager_dqa_pruning;
bool            gp_eager_one_phase_agg;
bool            gp_eager_two_phase_agg;
bool            gp_enable_groupext_distinct_pruning;
bool            gp_enable_groupext_distinct_gather;
int             gp_distinct_grouping_sets_threshold;
bool            gp_enable_preunique;
bool            gp_eager_preunique;
bool            gp_enable_sequential_window_plans;
bool            gp_enable_explain_allstat;
bool            gp_enable_sort_limit;
bool            gp_enable_sort_distinct;
bool            gp_enable_mk_sort;
bool            gp_enable_motion_mk_sort;
bool            gp_mk_sort_check;
bool            trace_sort;
int             gp_sort_flags;
int             gp_dbg_flags;
int             gp_sort_max_distinct;
bool            gp_enable_hash_partitioned_tables;
bool            gp_dynamic_partition_pruning;
bool            gp_cte_sharing;
bool            gp_foreign_data_access;
bool            gp_setwith_alter_storage;
bool            gp_enable_tablespace_auto_mkdir;
bool            gp_create_index_concurrently;
int             gp_segworker_relative_priority;
double          gp_hashagg_rewrite_limit;
bool            gp_hashagg_recalc_density;
bool            gp_hashagg_streambottom;
int             gp_hashagg_default_nbatches;
int             gp_hashagg_spillbatch_min;
int             gp_hashagg_spillbatch_max;
int             gp_hashjoin_bloomfilter;
bool            gp_statistics_pullup_from_child_partition;
bool            gp_statistics_use_fkeys;
int             gp_statistics_blocks_target;
double          gp_statistics_ndistinct_scaling_ratio_threshold;
double          gp_statistics_sampling_threshold;
bool            gp_statistics_master_sample;
int             gp_motion_slice_noop;
bool            gp_disable_tuple_hints;
bool 
gpvars_assign_gp_enable_gpperfmon(bool newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

bool 
gpvars_assign_gp_gpperfmon_send_interval(int newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

bool            gp_enable_gpperfmon;
int             gp_gpperfmon_send_interval;
int             gp_hashagg_compress_spill_files;
int             gp_workfile_compress_algorithm;
bool            gp_workfile_checksumming;
bool            gp_workfile_faultinject;
int             gp_workfile_bytes_to_checksum;
int             gp_workfile_type_hashjoin;
bool            gp_mapreduce_define;
bool            coredump_on_memerror;
GpAutoStatsModeValue gp_autostats_mode;
char           *gp_autostats_mode_string;
GpAutoStatsModeValue gp_autostats_mode_in_functions;
char           *gp_autostats_mode_in_functions_string;
int             gp_autostats_on_change_threshold;
bool            log_autostats;
const char     *
gpvars_assign_gp_autostats_mode(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_autostats_mode(void)
{



    return (char *) mock();

}

const char     *
gpvars_assign_gp_autostats_mode_in_functions(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_autostats_mode_in_functions(void)
{



    return (char *) mock();

}

bool            gp_dev_notice_agg_cost;
int             gp_debug_linger;
int             currentSliceId;
int             gp_singleton_segindex;
int             cdb_total_plans;
bool            gp_cost_hashjoin_chainwalk;
int             cdb_total_slices;
int             cdb_max_slices;
GpId            GpIdentity;
int             GpStandbyDbid;
int             Gp_listener_port;
SeqServerControlBlock *seqServerCtl;
void 
write_log(const char *fmt,...)
{

    check_expected(fmt);

    mock();

}

void 
get_timestamp(char *strfbuf, int length)
{

    check_expected(strfbuf);
    check_expected(length);
    optional_assignment(strfbuf);
    mock();

}

void 
verifyGpIdentityIsSet(void)
{



    mock();

}

bool 
gpvars_assign_gp_hash_index(bool newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

bool            gp_disable_atsdb_set_with;
const char     *
gpvars_assign_gp_resqueue_memory_policy(const char *newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (char *) mock();

}

const char     *
gpvars_show_gp_resqueue_memory_policy(void)
{



    return (char *) mock();

}

bool 
gpvars_assign_statement_mem(int newval, bool doit, GucSource source)
{

    check_expected(newval);
    check_expected(doit);
    check_expected(source);

    return (bool) mock();

}

void 
increment_command_count(void)
{



    mock();

}

char           *gp_idf_deduplicate_str;
