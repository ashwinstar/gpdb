import getpass
import glob
import os
import re
import thread
import time
import filecmp
import shutil
import signal
import socket

from gppylib.commands.base import Command, ExecutionError, REMOTE
from gppylib.commands.unix import findCmdInPath
from gppylib.db import dbconn
from gppylib.gparray import GpArray
from gppylib.operations.unix import ListRemoteFilesByPattern, CheckRemoteFile
from gppylib.test.behave_utils.utils import bring_nic_down, bring_nic_up, get_table_names, get_segment_hostnames,\
                                            create_database, create_database_if_not_exists, check_empty_table, check_err_msg, check_stdout_msg, \
                                            check_return_code, create_int_table, create_partition, check_table_exists, create_large_num_partitions,\
                                            create_fake_pg_aoseg_table, drop_database, drop_database_if_exists, drop_table_if_exists, getRows,\
                                            get_hosts_and_datadirs, get_master_hostname, insert_row, start_database_if_not_started, stop_database_if_started,\
                                            run_gpcommand, are_segments_synchronized, has_exception, modify_data, modify_partition_data, \
                                            validate_part_table_data_on_segments, validate_table_data_on_segments, validate_db_data, \
                                            get_partition_tablenames, check_partition_table_exists, create_indexes, get_partition_names, \
                                            validate_restore_data, backup_data, backup_db_data, cleanup_report_files, run_command, \
                                            get_distribution_policy, validate_distribution_policy, cleanup_backup_files, create_schema, drop_schema_if_exists, \
                                            create_mixed_storage_partition, validate_mixed_partition_storage_types, validate_storage_type, truncate_table, \
                                            get_table_oid, verify_truncate_in_pg_stat_last_operation, verify_truncate_not_in_pg_stat_last_operation, insert_numbers, \
                                            populate_partition_diff_data_same_eof, populate_partition_same_data, execute_sql, verify_integer_tuple_counts,  validate_no_aoco_stats, \
                                            check_string_not_present_stdout, clear_all_saved_data_verify_files, copy_file_to_all_db_hosts, validate_num_restored_tables, \
                                            get_partition_list, verify_stats, drop_external_table_if_exists, get_all_hostnames_as_list, get_pid_for_segment, kill_process, get_num_segments, \
                                            check_user_permissions

from gppylib.operations.dump import get_partition_state


master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
if master_data_dir is None:
    raise Exception('Please set MASTER_DATA_DIRECTORY in environment')

@given('the database is running')
def impl(context): 
    start_database_if_not_started(context)
    if has_exception(context):
        raise context.exception

@given('the database is not running')
def impl(context): 
    stop_database_if_started(context)
    if has_exception(context):
        raise context.exception

@given('database "{dbname}" exists')
@then('database "{dbname}" exists')
def impl(context, dbname):
    create_database(context, dbname)

@given('database "{dbname}" is created if not exists')
@then('database "{dbname}" is created if not exists')
def impl(context, dbname):
    create_database_if_not_exists(context, dbname)

@when('the database "{dbname}" does not exist')
@given('the database "{dbname}" does not exist')
@then('the database "{dbname}" does not exist')
def impl(context, dbname):
    drop_database_if_exists(context, dbname)

def get_segment_hostlist():
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    segment_hostlist = sorted(gparray.get_hostlist(includeMaster=False)) 
    if not segment_hostlist:
        raise Exception('segment_hostlist was empty')
    return segment_hostlist

@given('we have determined the first segment hostname')
def impl(context):
    segment_hostlist = get_segment_hostlist()    
    context.first_segment_hostname = segment_hostlist[0] 

@given('{nic} on the first segment host is {nic_status}')
@then('{nic} on the first segment host is {nic_status}')
def impl(context, nic, nic_status):
    if nic_status.strip() == 'down': 
        bring_nic_down(context.first_segment_hostname, nic)
    elif nic_status.strip() == 'up':
        bring_nic_up(context.first_segment_hostname, nic)
    else:
        raise Exception('Invalid nic status in feature file %s' % nic_status)

@when('an insert on "{table}" in "{dbname}" is rolled back')
def impl(context, table, dbname):
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        insert_sql = """INSERT INTO %s values (1)""" % table
        dbconn.execSQL(conn, insert_sql)
        conn.rollback()

@when('a truncate on "{table}" in "{dbname}" is rolled back')
def impl(context, table, dbname):
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        insert_sql = """TRUNCATE table %s""" % table
        dbconn.execSQL(conn, insert_sql)
        conn.rollback()

@when('an alter on "{table}" in "{dbname}" is rolled back')
def impl(context, table, dbname):
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        insert_sql = """ALTER TABLE %s add column cnew int default 0""" % table
        dbconn.execSQL(conn, insert_sql)
        conn.rollback()
 
    
@when('table "{table_name}" is deleted in "{dbname}"')
def impl(context, table_name, dbname):
    drop_table_if_exists(context, table_name=table_name, dbname=dbname)

@given('the user truncates "{table_list}" tables in "{dbname}"')
@when('the user truncates "{table_list}" tables in "{dbname}"')
def impl(context, table_list, dbname):
    if not table_list:
        raise Exception('Table list is empty')
    tables = table_list.split(',') 
    for t in tables:
        truncate_table(dbname, t.strip()) 

@given('there is a "{tabletype}" table "{table_name}" with compression "{compression_type}" in "{dbname}" with data')
def impl(context, tabletype, table_name, compression_type, dbname):
    create_database_if_not_exists(context, dbname)
    drop_table_if_exists(context, table_name=table_name, dbname=dbname)
    if compression_type == "None":
        create_partition(context, table_name, tabletype, dbname, compression_type=None, partition=False)
    else:
        create_partition(context, table_name, tabletype, dbname, compression_type, partition=False)

@when('the partition table "{table_name}" in "{dbname}" is populated with similar data')
def impl(context, table_name, dbname):
    populate_partition_diff_data_same_eof(table_name, dbname)

@given('the partition table "{table_name}" in "{dbname}" is populated with same data')
def impl(context, table_name, dbname):
    populate_partition_same_data(table_name, dbname)

@given('there is a "{tabletype}" table "{table_name}" with index "{indexname}" compression "{compression_type}" in "{dbname}" with data')
def impl(context, tabletype, table_name, compression_type, indexname, dbname):
    create_database_if_not_exists(context, dbname)
    drop_table_if_exists(context, table_name=table_name, dbname=dbname)
    if compression_type == "None":
        create_partition(context, table_name, tabletype, dbname, compression_type=None, partition=False)
    else:
        create_partition(context, table_name, tabletype, dbname, compression_type, partition=False)
    create_indexes(context, table_name, indexname, dbname)

@given('there is a "{tabletype}" partition table "{table_name}" with compression "{compression_type}" in "{dbname}" with data')
def impl(context, tabletype, table_name, compression_type, dbname):
    create_database_if_not_exists(context, dbname)
    drop_table_if_exists(context, table_name=table_name, dbname=dbname)
    if compression_type == "None":
        create_partition(context, table_name, tabletype, dbname)
    else:
        create_partition(context, table_name, tabletype, dbname, compression_type)

@given('there is a mixed storage partition table "{tablename}" in "{dbname}" with data')
def impl(context, tablename, dbname):
    create_database_if_not_exists(context, dbname)
    drop_table_if_exists(context, table_name=tablename, dbname=dbname)
    create_mixed_storage_partition(context, tablename, dbname)
 
@given('there is {table_type} table {table_name} in "{dbname}" with data')
def impl(context, table_type, table_name, dbname):
    create_database_if_not_exists(context, dbname)
    drop_table_if_exists(context, table_name=table_name, dbname=dbname)
    create_int_table(context, table_type=table_type, table_name=table_name, dbname=dbname)

@given('"{dbname}" does not exist')
def impl(context, dbname):
    drop_database(context, dbname)

@given('{env_var} environment variable is not set')
def impl(context, env_var):
    if not hasattr(context, 'orig_env'):
        context.orig_env = dict()
    context.orig_env[env_var] = os.environ.get(env_var)

    if env_var in os.environ:
        del os.environ[env_var]

@given('there are no "{tmp_file_prefix}" tempfiles')
def impl(context, tmp_file_prefix):
    if tmp_file_prefix is not None and tmp_file_prefix:
        run_command(context, 'rm -f /tmp/%s*' % tmp_file_prefix)
    else:
        raise Exception('Invalid call to temp file removal %s' % tmp_file_prefix)
    
@then('{env_var} environment variable should be restored')
def impl(context, env_var):
    if not hasattr(context, 'orig_env'):
        raise Exception('%s can not be reset' % env_var)

    if env_var not in context.orig_env:
        raise Exception('%s can not be reset.' % env_var)

    os.environ[env_var] = context.orig_env[env_var]

    del context.orig_env[env_var]

@when('the table names in "{dbname}" is stored')
def impl(context, dbname):
    context.table_names = get_table_names(dbname)
   
@given('the user runs "{command}"') 
@when('the user runs "{command}"')
@then('the user runs "{command}"')
def impl(context, command):
    run_gpcommand(context, command)

def run_valgrind_command(context, command):
    current_path = os.path.realpath(__file__)
    current_dir = os.path.dirname(current_path)
    cmd_text = "valgrind --suppressions=%s/valgrind_suppression.txt %s" %(current_dir, command)
    run_command(context, cmd_text)
    for line in context.error_message.splitlines():
        if 'ERROR SUMMARY' in line:
            if '0 errors from 0 contexts' not in line: 
                raise Exception('Output: %s' % context.error_message)
            else:
                return
    raise Exception('Could not find "ERROR SUMMARY" in %s' % context.error_message)

@then('the user runs valgrind with "{command}" and options "{options}"')
@when('the user runs valgrind with "{command}" and options "{options}"')
def impl(context, command, options):
    port = os.environ.get('PGPORT')
    user = getpass.getuser()
    if hasattr(context, 'backup_timestamp'):
        ts = context.backup_timestamp 
    bnr_tool = command.split()[0].strip()
    if bnr_tool == 'gp_dump':
        command_str = command
    elif bnr_tool == 'gp_dump_agent':
        command_str = command + ' -p %s' % port
    elif bnr_tool == 'gp_restore':
        command_str = "%s %s --gp-k %s --gp-d db_dumps/%s --gp-r db_dumps/%s" % (command, options, context.backup_timestamp, context.backup_timestamp[0:8], context.backup_timestamp[0:8])
    elif bnr_tool == 'gp_restore_agent':
        command_str = "%s %s --gp-k %s --gp-d db_dumps/%s -p %s -U %s --target-host localhost --target-port %s db_dumps/%s/gp_dump_1_1_%s_post_data.gz" % (command, options, ts, ts[0:8], port, user, port, ts[0:8], ts) 
        
    run_valgrind_command(context, command_str)
 
@when('the timestamp key is stored')
def impl(context):
    stdout = context.stdout_message
    for line in stdout.splitlines():
        if '--gp-k' in line:
            pat = re.compile('.* --gp-k=([0-9]{14}).*')
            m = pat.search(line) 
            if not m:
                raise Exception('Timestamp key not found')
            context.timestamp_key = m.group(1)
            return

@then('{command} should print {err_msg} error message')
def impl(context, command, err_msg):
    check_err_msg(context, err_msg)

@then('{command} should print {out_msg} to stdout')
def impl(context, command, out_msg):
    check_stdout_msg(context, out_msg)

@then('{command} should not print {out_msg} to stdout')
def impl(context, command, out_msg):
    check_string_not_present_stdout(context, out_msg)

@given('{command} should return a return code of {ret_code}')
@when('{command} should return a return code of {ret_code}')
@then('{command} should return a return code of {ret_code}')
def impl(context, command, ret_code):
    check_return_code(context, ret_code)

@then('an "{ex_type}" should be raised')
def impl(context, ex_type):
    if not context.exception:
        raise Exception('An exception was expected but was not thrown')
    typ = context.exception.__class__.__name__
    if typ != ex_type:
        raise Exception('got exception of type "%s" but expected type "%s"' % (typ, ex_type))

@then('database "{dbname}" health check should pass on table "{tablename}"')
def impl(context, dbname, tablename):
    
    drop_database_if_exists(context, dbname)
    create_database(context, dbname)
    create_int_table(context, tablename, dbname=dbname)
    drop_database(context, dbname)

    
@then('the segments are synchronized')
def impl(context):

    times = 30
    sleeptime = 10

    for i in range(times):
        if are_segments_synchronized():
            return
        time.sleep(sleeptime)

    raise Exception('segments are not in sync after %d seconds' % (times * sleeptime))

@when('table "{table_list}" is assumed to be in dirty state in "{dbname}"')
@then('table "{table_list}" is assumed to be in dirty state in "{dbname}"')
@given('table "{table_list}" is assumed to be in dirty state in "{dbname}"')
def impl(context, table_list, dbname):
    tables = table_list.split(',')
    for t in tables:
        modify_data(context, t.strip(), dbname)
        backup_data(context, t.strip(), dbname)

    get_distribution_policy(dbname)

@given('all the data from "{dbname}" is saved for verification')
@when('all the data from "{dbname}" is saved for verification')
@then('all the data from "{dbname}" is saved for verification')
def impl(context, dbname):
    backup_db_data(context, dbname)
    
@then('partition "{partition}" of partition table "{table_list}" is assumed to be in dirty state in "{dbname}" in schema "{schema}"')
@when('partition "{partition}" of partition table "{table_list}" is assumed to be in dirty state in "{dbname}" in schema "{schema}"')
@given('partition "{partition}" of partition table "{table_list}" is assumed to be in dirty state in "{dbname}" in schema "{schema}"')
def impl(context, partition, table_list, dbname, schema):
    tables = table_list.split(',')
    for t in tables:
        part_t = get_partition_names(schema, t.strip(), dbname, 1, partition)
        if len(part_t) < 1 or len(part_t[0]) < 1:
            print part_t 
        a_partition_name = part_t[0][0].strip()
        dirty_table_name = '%s.%s\n' % (schema, a_partition_name)
        modify_partition_data(context, dirty_table_name.strip(), dbname, int(partition))
        backup_data(context, dirty_table_name.strip(), dbname)

def validate_timestamp(ts):
    try:
        int_ts = int(ts)
    except Exception as e:
        raise Exception('Timestamp is not valid %s' % ts)

    if len(ts) != 14:
        raise Exception('Timestamp is invalid %s' % ts)

@when('the subdir from gpcrondump is stored')    
@then('the subdir from gpcrondump is stored')
def impl(context):
    stdout = context.stdout_message
    for line in stdout.splitlines():
        if 'Dump subdirectory' in line:
            log_msg, delim, subdir = line.partition('=') 
            context.backup_subdir = subdir.strip()
            return 

    raise Exception('Dump subdirectory not found %s' % stdout)

def get_timestamp_from_output(context):
    ts = None
    stdout = context.stdout_message
    for line in stdout.splitlines():
        if 'Timestamp key = ' in line:
            log_msg, delim, timestamp = line.partition('=') 
            ts = timestamp.strip()
            validate_timestamp(ts)
            return ts

    raise Exception('Timestamp not found %s' % stdout)
 

@given('the full backup timestamp from gpcrondump is stored')
@when('the full backup timestamp from gpcrondump is stored')
@then('the full backup timestamp from gpcrondump is stored')
def impl(context):
    context.full_backup_timestamp = get_timestamp_from_output(context)

@when('the timestamp from gpcrondump is stored')    
@then('the timestamp from gpcrondump is stored')
def impl(context):
    context.backup_timestamp = get_timestamp_from_output(context)

@when('the timestamp is labeled "{lbl}"')
def impl(context, lbl):
    if not hasattr(context, 'timestamp_labels'):
        context.timestamp_labels = {}

    context.timestamp_labels[lbl] = get_timestamp_from_output(context)

@given('there is a list to store the incremental backup timestamps')
def impl(context):
    context.inc_backup_timestamps = []

@then('the timestamp from gpcrondump is stored in a list')
@when('the timestamp from gpcrondump is stored in a list')
def impl(context):
    context.backup_timestamp = get_timestamp_from_output(context)
    context.inc_backup_timestamps.append(context.backup_timestamp)

@then('verify that the incremental file has the stored timestamp')
def impl(context):

    inc_file_name = 'gp_dump_%s_increments' % context.full_backup_timestamp
    subdirectory = context.full_backup_timestamp[0:8]
    full_path = os.path.join(master_data_dir, 'db_dumps', subdirectory, inc_file_name) 

    if not os.path.isfile(full_path):
        raise Exception ("Can not find increments file: %s" % full_path)

    contents = ""
    with open(full_path) as fd: 
        contents = fd.read().strip()

    if context.backup_timestamp != contents:
        raise Exception("The increments file '%s' does not contain the timestamp %s" % (full_path, context.backup_timestamp))

def check_increments_file_for_list(context, location):
    inc_file_name = 'gp_dump_%s_increments' % context.full_backup_timestamp
    subdirectory = context.full_backup_timestamp[0:8]
    full_path = os.path.join(location, 'db_dumps', subdirectory, inc_file_name) 

    if not os.path.isfile(full_path):
        raise Exception ("Can not find increments file: %s" % full_path)

    found_timestamps = []
    contents = ""
    with open(full_path) as fd: 
        contents = fd.read()
        for line in contents.splitlines():
            line = line.strip()
            if not line:
                continue
            found_timestamps.append(line)

    found_timestamps = sorted(found_timestamps)
    context.inc_backup_timestamps = sorted(context.inc_backup_timestamps)

    if found_timestamps != context.inc_backup_timestamps:
        print "Found timestamps: "
        print found_timestamps
        print "Expected timestamps: "
        print context.inc_backup_timestamps
        raise Exception("Expected timestamps not found")

@then('verify that the incremental file in "{location}" has all the stored timestamps')
def impl(context, location):
    check_increments_file_for_list(context, location)

@then('verify that the incremental file has all the stored timestamps')
def impl(context):
    check_increments_file_for_list(context, master_data_dir)

@then('verify that the plan file is created for the latest timestamp')
def impl(context):
    context.inc_backup_timestamps = sorted(context.inc_backup_timestamps)
    latest_ts = context.inc_backup_timestamps[-1]
    plan_file_dir = os.path.join(master_data_dir, 'db_dumps', latest_ts[0:8])
    plan_file_count =  len(glob.glob('/%s/*_plan' % plan_file_dir)) 
    if plan_file_count != 1: 
        raise Exception('Expected only one plan file, found %s' % plan_file_count)
    filename = '%s/gp_restore_%s_plan' % (plan_file_dir, latest_ts)
    if not os.path.exists(filename):
        raise Exception('Plan file %s not created for the latest timestamp' % filename)

@then('the timestamp from gp_dump is stored and subdir is "{subdir}"')
def impl(context, subdir):
    stdout = context.stdout_message
    context.backup_subdir = subdir
    for line in stdout.splitlines():
        if 'Timestamp Key: ' in line:
            context.backup_timestamp = line.split()[-1] 
            validate_timestamp(context.backup_timestamp)
            return 

    raise Exception('Timestamp not found %s' % stdout)
   
@when('the state files are generated under "{dir}" for stored "{backup_type}" timestamp') 
@then('the state files are generated under "{dir}" for stored "{backup_type}" timestamp') 
def impl(context, dir, backup_type):
    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    if backup_type == 'full':
        timestamp = context.full_backup_timestamp
    else:
        timestamp = context.backup_timestamp

    ao_state_filename = "%s/db_dumps/%s/gp_dump_%s_ao_state_file" % (dump_dir, timestamp[0:8], timestamp)
    co_state_filename = "%s/db_dumps/%s/gp_dump_%s_co_state_file" % (dump_dir, timestamp[0:8], timestamp)

    if not os.path.exists(ao_state_filename):
        raise Exception('AO state file %s not generated' % ao_state_filename)
    if not os.path.exists(co_state_filename):
        raise Exception('CO state file %s not generated' % co_state_filename)

    verify_integer_tuple_counts(context, ao_state_filename)
    verify_integer_tuple_counts(context, co_state_filename)

@then('the "{file_type}" files are generated under "{dirname}" for stored "{backup_type}" timestamp')
def impl(context, file_type, dirname, backup_type):
    dump_dir = dirname if len(dirname.strip()) != 0 else master_data_dir
    if backup_type == 'full':
        timestamp = context.full_backup_timestamp
    else:
        timestamp = context.backup_timestamp
    last_operation_filename = "%s/db_dumps/%s/gp_dump_%s_last_operation" % (dump_dir, timestamp[0:8], timestamp) 
    if not os.path.exists(last_operation_filename):
        raise Exception('Last operation file %s not generated' % last_operation_filename)

@given('the user runs gp_restore with the the stored timestamp subdir and stored filename in "{dbname}"')
@when('the user runs gp_restore with the the stored timestamp subdir and stored filename in "{dbname}"')
def impl(context, dbname):
    command = 'gp_restore -i --gp-k %s --gp-d db_dumps/%s --gp-i --gp-r db_dumps/%s --gp-l=p -d %s --gp-c --gp-f %s' % (context.backup_timestamp, context.backup_subdir, context.backup_subdir, dbname, context.filename)
    run_gpcommand(context, command)

@then('the user runs gp_restore with the the stored timestamp and subdir in "{dbname}"')
def impl(context, dbname):
    command = 'gp_restore -i --gp-k %s --gp-d db_dumps/%s --gp-i --gp-r db_dumps/%s --gp-l=p -d %s --gp-c' % (context.backup_timestamp, context.backup_subdir, context.backup_subdir, dbname)
    run_gpcommand(context, command)

@then('the user runs gp_restore with the the stored timestamp and subdir in "{dbname}" and bypasses ao stats')
def impl(context, dbname):
    command = 'gp_restore -i --gp-k %s --gp-d db_dumps/%s --gp-i --gp-r db_dumps/%s --gp-l=p -d %s --gp-c --gp-nostats' % (context.backup_timestamp, context.backup_subdir, context.backup_subdir, dbname)
    run_gpcommand(context, command)

@then('the user runs gp_restore with the stored timestamp and subdir in "{dbname}" and backup_dir "{backup_dir}"')
def impl(context, dbname, backup_dir):
    command = 'gp_restore -i --gp-k %s --gp-d %s/db_dumps/%s --gp-i --gp-r %s/db_dumps/%s --gp-l=p -d %s --gp-c' % (context.backup_timestamp, backup_dir, context.backup_subdir, backup_dir, context.backup_subdir, dbname)
    run_gpcommand(context, command)

@when('the user runs gp_restore with the the stored timestamp and subdir for metadata only in "{dbname}"')
@then('the user runs gp_restore with the the stored timestamp and subdir for metadata only in "{dbname}"')
def impl(context, dbname):
    command = 'gp_restore -i --gp-k %s --gp-d db_dumps/%s --gp-i --gp-r db_dumps/%s --gp-l=p -d %s --gp-c -s db_dumps/%s/gp_dump_1_1_%s.gz' % \
                            (context.backup_timestamp, context.backup_subdir, context.backup_subdir, dbname, context.backup_subdir, context.backup_timestamp)
    run_gpcommand(context, command)

@when('the user runs gpdbrestore with the stored timestamp')
@then('the user runs gpdbrestore with the stored timestamp')
def impl(context):
    command = 'gpdbrestore -e -t %s -a' % context.backup_timestamp
    run_gpcommand(context, command)

@when('the user runs gpdbrestore with the stored timestamp to print the backup set with options "{options}"')
def impl(context, options):
    command = 'gpdbrestore -t %s %s --list-backup' % (context.backup_timestamp, options)
    run_gpcommand(context, command)

@then('the user runs gpdbrestore with the stored timestamp and options "{options}"')
@when('the user runs gpdbrestore with the stored timestamp and options "{options}"')
def impl(context, options):
    if options == '-b':
        command = 'gpdbrestore -e -b %s -a' % (context.backup_timestamp[0:8])
    else:
        command = 'gpdbrestore -e -t %s %s -a' % (context.backup_timestamp, options)
    run_gpcommand(context, command)

@when('the user runs gpdbrestore with the stored timestamp and options "{options}" without -e option')
def impl(context, options):
    if options == '-b':
        command = 'gpdbrestore -b %s -a' % (context.backup_timestamp[0:8])
    else:
        command = 'gpdbrestore -t %s %s -a' % (context.backup_timestamp, options)
    run_gpcommand(context, command)

@when('the user runs "{cmd}" with the stored timestamp')
@then('the user runs "{cmd}" with the stored timestamp')
def impl(context, cmd):
    command = '%s -t %s' % (cmd, context.backup_timestamp)
    run_gpcommand(context, command)

@then('verify that there is no table "{tablename}" in "{dbname}"')
def impl(context, tablename, dbname):
    if check_table_exists(context, dbname=dbname, table_name=tablename): 
        raise Exception("Table '%s' still exists when it should not" % tablename)

@then('verify that there is a "{table_type}" table "{tablename}" in "{dbname}"')
def impl(context, table_type, tablename, dbname):
    if not check_table_exists(context, dbname=dbname, table_name=tablename,table_type=table_type): 
        raise Exception("Table '%s' of type '%s' does not exist when expected" % (tablename, table_type))

@then('verify that there is partition "{partition}" of "{table_type}" partition table "{tablename}" in "{dbname}" in "{schemaname}"')
def impl(context, partition, table_type, tablename, dbname, schemaname):
    if not check_partition_table_exists(context, dbname=dbname, schemaname=schemaname, table_name=tablename, table_type=table_type, part_level=1, part_number=partition): 
        raise Exception("Partition %s for table '%s' of type '%s' does not exist when expected" % (partition, tablename, table_type))

@then('verify that there is partition "{partition}" of mixed partition table "{tablename}" with storage_type "{storage_type}"  in "{dbname}" in "{schemaname}"')
def impl(context, partition, tablename, storage_type, dbname, schemaname):
    part_t = get_partition_names(schemaname, tablename, dbname, 1, partition)
    partname = part_t[0][0].strip()
    validate_storage_type(context, partname, storage_type, dbname)

@then('verify that storage_types of the partition table "{tablename}" are as expected in "{dbname}"')
def impl(context, tablename, dbname):
    validate_mixed_partition_storage_types(context, tablename, dbname)

@then('data for partition table "{table_name}" with partition level "{part_level}" is distributed across all segments on "{dbname}"')
def impl(context, table_name, part_level, dbname):
    validate_part_table_data_on_segments(context, table_name, part_level, dbname) 

@then('data for table "{table_name}" is distributed across all segments on "{dbname}"')
def impl(context, table_name, dbname):
    validate_table_data_on_segments(context, table_name, dbname) 

@then('verify that the data of the {file} under "{backup_dir}" in "{dbname}" is validated after restore')
def impl(context, file, dbname, backup_dir):
    dump_dir = backup_dir if len(backup_dir.strip()) != 0 else master_data_dir

    if file == 'dirty tables': 
        dirty_list_filename = '%s/db_dumps/%s/gp_dump_%s_dirty_list' % (dump_dir, context.backup_timestamp[0:8], context.backup_timestamp)
    elif file == 'table_filter_file':
        dirty_list_filename = os.path.join(os.getcwd(), file)
        
    if not os.path.exists(dirty_list_filename):
        raise Exception('Dirty list file %s does not exist' % dirty_list_filename)
    
    with open(dirty_list_filename) as fd:
        tables = fd.readlines()
        for table in tables:
            validate_restore_data(context, table.strip(), dbname)

@then('verify that the distribution policy of all the tables in "{dbname}" are validated after restore')
def impl(context, dbname):
    validate_distribution_policy(context, dbname)

@then('verify that tables "{table_list}" in "{dbname}" has no rows')
def impl(context, table_list, dbname):
    tables = [t.strip() for t in table_list.split(',')] 
    for t in tables:
        check_empty_table(t, dbname)

@then('verify that partitioned tables "{table_list}" in "{dbname}" have {num_parts} partitions')
def impl(context, table_list, dbname, num_parts):
    num_parts = int(num_parts.strip())
    tables = [t.strip() for t in table_list.split(',')] 
    for t in tables:
        names = get_partition_tablenames(t, dbname)
        if len(names) != num_parts:
            raise Exception("%s.%s should have %d partitions but has %d" % (dbname, t, num_parts, len(names)))

# raise exception if tname does not have X empty partitions
def check_x_empty_parts(dbname, tname, x):
    num_empty = 0
    parts = get_partition_tablenames(tname, dbname)
    for part in parts:
        p = part[0]
        try:
            check_empty_table(p, dbname)
            num_empty += 1
        except Exception as e:
            print e

    if num_empty != x:
        raise Exception("%s.%s has %d empty partitions and should have %d" % (dbname, tname, num_empty, x))

@then('the user runs gpdbrestore with "{opt}" option in path "{path}"')
def impl(context, opt, path):
    command = 'gpdbrestore -e -a %s localhost:%s/db_dumps/%s --verbose' % (opt, path, context.backup_subdir)
    run_gpcommand(context, command)

@then('all files for full backup have been removed in path "{path}"')
def impl(context, path):
    path = path if len(path.strip()) != 0 else master_data_dir
    file_pattern = "*_%s*" % context.full_backup_timestamp
    dir = "%s/db_dumps/%s" %(path, context.backup_subdir)
    cleanup_cmd = "rm -f %s/%s" % (dir, file_pattern)
    run_command(context, cleanup_cmd)
    if context.exception:
        raise context.exception

@when('there are no backup files')
@given('there are no backup files')
def impl(context):
    cleanup_backup_files(context, 'template1')

@given('there are no backup files in "{location}"')
def impl(context, location):
    cleanup_backup_files(context, 'template1', location)

@then('there are no report files in the master data directory')
def impl(context):
    cleanup_report_files(context, master_data_dir)

@when('verify that partitioned tables "{table_list}" in "{dbname}" has {num_parts} empty partitions')
@then('verify that partitioned tables "{table_list}" in "{dbname}" has {num_parts} empty partitions')
def impl(context, table_list, dbname, num_parts):
    expected_num_parts = int(num_parts.strip())
    tables = [t.strip() for t in table_list.split(',')] 
    for t in tables:
        check_x_empty_parts(dbname, t, expected_num_parts)

@given('there is a backupfile of tables "{table_list}" in "{dbname}" exists for validation')
def impl(context, table_list, dbname):
    tables = [t.strip() for t in table_list.split(',')] 
    for t in tables:
        backup_data(context, t.strip(), dbname) 

@when('verify that there is a "{table_type}" table "{tablename}" in "{dbname}" with data')
@then('verify that there is a "{table_type}" table "{tablename}" in "{dbname}" with data')
def impl(context, table_type, tablename, dbname):
    if not check_table_exists(context, dbname=dbname, table_name=tablename, table_type=table_type):
        raise Exception("Table '%s' does not exist when it should" % tablename)
    validate_restore_data(context, tablename, dbname) 

@given('there is schema "{schema_list}" exists in "{dbname}"')
def impl(context, schema_list, dbname):
    schemas = [s.strip() for s in schema_list.split(',')]
    for s in schemas:
        drop_schema_if_exists(context, s.strip(), dbname)
        create_schema(context, s.strip(), dbname)

@then('the temporary file "{filename}" is removed')
def impl(context, filename):
    if os.path.exists(context.filename):
        os.remove(context.filename)

def create_table_file_locally(context, filename, table_list, location=os.getcwd()):
    tables = [t.strip() for t in table_list.split(',')] 
    file_path = os.path.join(location, filename)
    with open(file_path, 'w') as fp:
        for t in tables:
            fp.write(t.strip() + '\n')
    context.filename = file_path

@given('there is a file "{filename}" with tables "{table_list}"')
def impl(context, filename, table_list):
    create_table_file_locally(context, filename, table_list)

@given('there is a fake pg_aoseg table named "{table}" in "{dbname}"')
def impl(context, table, dbname):
    create_fake_pg_aoseg_table(context, table, dbname)

@then('verify that the report file in "{report_dir}" dir contains "{text_find}"')
def impl(context, report_dir, text_find):
    if len(report_dir.strip()) == 0:
        report_dir = master_data_dir

    fn = 'gp_dump_%s.rpt' % context.backup_timestamp

    subdirectory = context.backup_timestamp[0:8]

    full_path = os.path.join(report_dir, 'db_dumps', subdirectory, fn)

    if not os.path.isfile(full_path):
        raise Exception ("Can not find report file: %s" % full_path)

    contents = ""
    with open(full_path) as fd:
        contents = fd.read()

    if not text_find in contents:
        raise Exception("Did not find '%s' in file %s" % (text_find, full_path))

@then('the timestamp in the report file should be same as timestamp key')
def impl(context):
    if not hasattr(context, 'timestamp_key'):
        raise Exception('Unable to find timestamp key in context')
       
    if hasattr(context, 'backup_dir'): 
        report_file = os.path.join(context.backup_dir, 'db_dumps', '%s' % (context.timestamp_key[0:8]),'gp_dump_%s.rpt' % context.timestamp_key)
    else:
        report_file = os.path.join(master_data_dir, 'db_dumps', '%s' % (context.timestamp_key[0:8]), 'gp_dump_%s.rpt' % context.timestamp_key)
    
    with open(report_file) as rpt:
        for line in rpt:
            if line.startswith('Timestamp Key'):
                timestamp_key = line.split(':')[-1].strip()
                if timestamp_key != context.timestamp_key:
                    raise Exception('Expected timestamp key to be %s, but found %s in report file %s' % (context.timestamp_key, timestamp_key, report_file))

@then('there should be dump files with filename having timestamp key in "{dbname}"')
def impl(context, dbname):
    if not hasattr(context, 'timestamp_key'):
        raise Exception('Unable to find timestamp key in context')
       
    master_hostname = get_master_hostname(dbname) 
    results = get_hosts_and_datadirs(dbname)

    for (host, datadir) in results:
        if host == master_hostname:
            if hasattr(context, 'backup_dir'): 
                dump_dir = os.path.join(context.backup_dir, 'db_dumps', '%s' % (context.timestamp_key[0:8]))
            else:
                dump_dir = os.path.join(master_data_dir, 'db_dumps', '%s' % (context.timestamp_key[0:8]))
    
            master_dump_files = ['%s/gp_dump_1_1_%s' % (dump_dir, context.timestamp_key),
                                 '%s/gp_dump_status_1_1_%s'  % (dump_dir, context.timestamp_key),
                                 '%s/gp_cdatabase_1_1_%s' % (dump_dir, context.timestamp_key),
                                 '%s/gp_dump_1_1_%s_post_data' % (dump_dir, context.timestamp_key)]

            for dump_file in master_dump_files:
                cmd = Command('check for dump files', 'ls -1 %s | wc -l' % (dump_file))
                cmd.run(validateAfter=True)
                results = cmd.get_results()
                if int(results.stdout.strip()) != 1:
                    raise Exception('Dump file %s not found after gp_dump on host %s' % (dump_file, host))
        else:
            if hasattr(context, 'backup_dir'): 
                dump_dir = os.path.join(context.backup_dir, 'db_dumps', '%s' % (context.timestamp_key[0:8]))
            else:
                dump_dir = os.path.join(datadir, 'db_dumps', '%s' % (context.timestamp_key[0:8]))

            segment_dump_files = ['%s/gp_dump_*_*_%s' % (dump_dir, context.timestamp_key),
                                  '%s/gp_dump_status_*_*_%s' % (dump_dir, context.timestamp_key)]

            for dump_file in segment_dump_files:
                cmd = Command('check for dump files', 'ls -1 %s | wc -l' % (dump_file), ctxt=REMOTE, remoteHost=host)
                cmd.run(validateAfter=True)
                results = cmd.get_results()
                if int(results.stdout.strip()) != 1:
                    raise Exception('Dump file %s not found after gp_dump on host %s' % (dump_file, host))

@then('"{filetype}" file should not be created under "{dir}"')
def impl(context, filetype, dir):
    if not hasattr(context, 'backup_timestamp'):
        raise Exception('Unable to find out the %s because backup timestamp has not been stored' % filename)

    filename = ''
    if filetype == "dirty_list":
        filename = 'gp_dump_%s_dirty_list' % context.backup_timestamp
    elif filetype == "plan":
        filename = 'gp_restore_%s_plan' % context.backup_timestamp
    elif filetype == 'pipes':
        filename = 'gp_dump_%s_pipes' % context.backup_timestamp
    elif filetype == 'regular_files':
        filename = 'gp_dump_%s_regular_files' % context.backup_timestamp
    else:
        raise Exception("Unknown filetype '%s' specified" % filetype)

    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    file_path = os.path.join(dump_dir, 'db_dumps', context.backup_timestamp[0:8], filename)

    if os.path.exists(file_path):
        raise Exception("File path %s should not exist for filetype '%s'" % (file_path, filetype))

def get_plan_filename(context):
    filename = 'gp_restore_%s_plan' % context.backup_timestamp
    return os.path.join(master_data_dir, 'db_dumps', context.backup_timestamp[0:8], filename)

def get_dirty_list_filename(context):
    filename = 'gp_dump_%s_dirty_list' % context.backup_timestamp
    return os.path.join(master_data_dir, 'db_dumps', context.backup_timestamp[0:8], filename)

@then('plan file should match "{filename}"')
def impl(context, filename):

    current_path = os.path.realpath(__file__)
    current_dir = os.path.dirname(current_path)
    golden_filename = "%s/%s" % (current_dir, filename)
    generated_filename = get_plan_filename(context)
    if not filecmp.cmp(generated_filename, golden_filename):
        raise Exception("File contents do not match '%s' and '%s'" % (generated_filename, golden_filename))

def parse_plan_file(filename):
    plan = {} 
    with open(filename) as fd:
        for line in fd:
            parts = line.partition(":")
            ts = parts[0].strip()
            if ts not in plan:
                plan[ts] = set()
                tables = parts[2].split(",")
                for t in tables:
                    if t not in plan[ts]:
                        plan[ts].add(t.strip())
    return plan

def modify_plan_with_labels(context, expected_plan):
    newplan = {}
    for k in expected_plan:
        if k not in context.timestamp_labels:
            raise Exception("Label '%s' not specified in behave test" % k)
        ts = context.timestamp_labels[k]
        newplan[ts] = expected_plan[k]
    return newplan

def compare_plans(expected, actual):
    expected_keys = expected.keys()
    actual_keys = actual.keys()
    
    if len(expected_keys) != len(actual_keys):
        raise Exception("Expected plan has %s timestamps actual plan has %s timestamps" % (len(expected_keys), len(actual_keys)))

    for k in expected:
        if k not in actual:
            raise Exception("Expected timestamp in plan and did not find it: %s " % k)
        expected_tables = sorted(expected[k])
        actual_tables = sorted(actual[k])
        if expected_tables != actual_tables:
            print "Expected plan: %s" % expected
            print "Actual plan: %s" % actual
            raise Exception("Tables in plan for timestamp '%s' do not match expected tables" % k)

@then('the plan file is validated against "{expected_plan}"')
def impl(context, expected_plan):
    context.restore_plan = parse_plan_file(get_plan_filename(context))

    current_path = os.path.realpath(__file__)
    current_dir = os.path.dirname(current_path)
    expected_file = '%s/%s' % (current_dir, expected_plan)
    expected_plan = parse_plan_file(expected_file)
    expected_plan = modify_plan_with_labels(context, expected_plan)
    compare_plans(expected_plan, context.restore_plan)
    
@then('there should be "{numtimestamps}" timestamps in the plan file')
def impl(context, numtimestamps):
    num = int(numtimestamps)
    if len(context.restore_plan) != num:
        raise Exception("Expected %d timestamps and found %d in restore plan" % (num, len(context.restore_plan)))

@then('restore plan for timestamp "{ts}" should contain "{numtables}" tables')
def impl(context, ts, numtables):
    num = int(numtables)
    if ts not in context.restore_plan:
        raise Exception("Timestamp label '%s' not found in restore plan" % ts)

@then('"{filetype}" file is removed under "{dir}"')
def impl(context, filetype, dir):
    if not hasattr(context, 'backup_timestamp'):
        raise Exception('Backup timestamp has not been stored')

    if filetype == "dirty_list":
        filename = 'gp_dump_%s_dirty_list' % context.backup_timestamp
    elif filetype == "plan":
        filename = 'gp_restore_%s_plan' % context.backup_timestamp
    elif filetype == "global":
        filename = 'gp_global_1_1_%s' % context.backup_timestamp
    elif filetype == "report":
        filename = 'gp_dump_%s.rpt' % context.backup_timestamp
    else:
        raise Exception("Unknown filetype '%s' specified" % filetype)

    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    file_path = os.path.join(dump_dir, 'db_dumps', context.backup_timestamp[0:8], filename)

    if os.path.exists(file_path):
        os.remove(file_path)
 

@when('"{filetype}" file should be created under "{dir}"')
@then('"{filetype}" file should be created under "{dir}"')
def impl(context, filetype, dir):
    if not hasattr(context, 'backup_timestamp'):
        raise Exception('Backup timestamp has not been stored')

    if filetype == "dirty_list":
        filename = 'gp_dump_%s_dirty_list' % context.backup_timestamp
    elif filetype == "plan":
        filename = 'gp_restore_%s_plan' % context.backup_timestamp
    elif filetype == "global":
        filename = 'gp_global_1_1_%s' % context.backup_timestamp
    elif filetype == 'pipes':
        filename = 'gp_dump_%s_pipes' % context.backup_timestamp
    elif filetype == 'regular_files':
        filename = 'gp_dump_%s_regular_files' % context.backup_timestamp
    else:
        raise Exception("Unknown filetype '%s' specified" % filetype)


    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    file_path = os.path.join(dump_dir, 'db_dumps', context.backup_timestamp[0:8], filename)

    if not os.path.exists(file_path):
        raise Exception("File path %s does not exist for filetype '%s'" % (file_path, filetype))

@then('verify there are no "{tmp_file_prefix}" tempfiles')
def impl(context, tmp_file_prefix):
    if tmp_file_prefix is not None and tmp_file_prefix:
       if glob.glob('/tmp/%s*' % tmp_file_prefix): 
            raise Exception('Found temp %s files where they should not be present' % tmp_file_prefix)
    else:
        raise Exception('Invalid call to temp file removal %s' % tmp_file_prefix) 

@then('tables names should be identical to stored table names in "{dbname}"')
def impl(context, dbname):
    table_names = sorted(get_table_names(dbname))
    stored_table_names = sorted(context.table_names)

    if table_names != stored_table_names:
        print "Table names after backup:"
        print stored_table_names
        print "Table names after restore:"
        print table_names
        raise Exception('Schema not restored correctly. List of tables are not equal before and after restore in database %s' % dbname)
   
@then('tables in "{dbname}" should not contain any data')
def impl(context, dbname):
    for table in context.table_names:
        table_name = "%s.%s" % (table[0], table[1])
        check_empty_table(table_name, dbname)

@then('verify that the data of "{expected_count}" tables in "{dbname}" is validated after restore')
def impl(context, dbname, expected_count):
    validate_db_data(context, dbname, int(expected_count))

@then('all the data from the remote segments in "{dbname}" are stored in path "{dir}" for "{backup_type}"')
def impl(context, dbname, dir, backup_type):
    segs = get_segment_hostnames(context, dbname)
    if backup_type == 'inc':
        timestamp = context.backup_timestamp[0:8]
    elif backup_type == 'full':
        timestamp = context.full_backup_timestamp[0:8]

    from_path = '%s/db_dumps/%s' %(dir, timestamp)
    to_path = '%s/db_dumps' %(dir)
    for seg in segs:
        print type(seg[0].strip())
        cmdStr = "%s -o 'StrictHostKeyChecking no' -r %s:%s %s" % (findCmdInPath('scp'), seg[0].strip(),from_path, to_path)
        run_command(context, cmdStr)
    
@then('pg_stat_last_operation registers the truncate for tables "{table_list}" in "{dbname}" in schema "{schema}"')
def impl(context, table_list, dbname, schema):
    if not table_list:
        raise Exception('Empty table list')
    tables = table_list.split(',')
    for t in tables:
        table_oid = get_table_oid(context, dbname, schema, t.strip())  
        verify_truncate_in_pg_stat_last_operation(context, dbname, table_oid)

@then('pg_stat_last_operation does not register the truncate for tables "{table_list}" in "{dbname}" in schema "{schema}"')
def impl(context, table_list, dbname, schema):
    if not table_list:
        raise Exception('Empty table list')
    tables = table_list.split(',')
    for t in tables:
        table_oid = get_table_oid(context, dbname, schema, t.strip())  
        verify_truncate_not_in_pg_stat_last_operation(context, dbname, table_oid)

@given('the numbers "{lownum}" to "{highnum}" are inserted into "{tablename}" tables in "{dbname}"')
@when('the numbers "{lownum}" to "{highnum}" are inserted into "{tablename}" tables in "{dbname}"')
def impl(context, lownum, highnum, tablename, dbname):
    insert_numbers(dbname, tablename, lownum, highnum)


@when('the user adds column "{cname}" with type "{ctype}" and default "{defval}" to "{tname}" table in "{dbname}"')
def impl(context, cname, ctype, defval, tname, dbname):
    sql = "ALTER table %s ADD COLUMN %s %s DEFAULT %s" % (tname, cname, ctype, defval)
    execute_sql(dbname, sql)

@given('there is a fake timestamp for "{ts}"')
def impl(context, ts):
    dname = os.path.join(master_data_dir, 'db_dumps', ts[0:8])
    os.makedirs(dname)

    contents = """
Timestamp Key: %s
Backup Type: Full
gp_dump utility finished successfully.
""" % ts

    fname = os.path.join(dname, 'gp_dump_%s.rpt' % ts)
    with open(fname, 'w') as fd:
        fd.write(contents)

@then('a timestamp in increments file in "{directory}" is modified to be newer')
def impl(context, directory):
    if not hasattr(context, 'full_backup_timestamp'):
        raise Exception('Full backup timestamp needs to be specified in the test')

    if not directory.strip():
        directory = master_data_dir

    dump_dir = os.path.join(directory, 'db_dumps', context.full_backup_timestamp[0:8])
    increments_filename = os.path.join(dump_dir, 'gp_dump_%s_increments' % context.full_backup_timestamp)

    if not os.path.exists(increments_filename):
        raise Exception('Increments file %s does not exist !' % increments_filename) 

    with open(increments_filename) as fd:
        lines = fd.readlines()
        lines[0] = str(int(lines[0].strip()) + 10000)

    with open(increments_filename, 'w') as fd:
        for line in lines:
            fd.write(line + '\n')

@then('the "{table_type}" state file under "{backup_dir}" is saved for the "{backup_type}" timestamp')
def impl(context, table_type, backup_dir, backup_type):
    timestamp_key = None
    if backup_type == 'full':
        timestamp_key = context.full_backup_timestamp
    elif backup_type == 'inc':
        timestamp_key = context.backup_timestamp

    backup_dir = backup_dir if len(backup_dir.strip()) != 0 else master_data_dir
    context.state_file = os.path.join(backup_dir, 'db_dumps', timestamp_key[0:8], 'gp_dump_%s_%s_state_file' % (timestamp_key, table_type))

@then('the saved state file is deleted')
def impl(context):
    run_command(context, 'rm -f %s' % context.state_file) 
    if context.exception:
        raise context.exception

@then('the saved state file is corrupted')
def impl(context):

    write_lines = list()
    with open(context.state_file, "r") as fr:
        lines = fr.readlines()

    for line in lines:
        line = line.replace(",", "")
        write_lines.append(line)

    with open(context.state_file, "w") as fw:
        for line in write_lines:
            fw.write("%s\n" % line.rstrip())

@then('"{table_name}" is marked as dirty in dirty_list file')
def impl(context, table_name):
    dirty_list = get_dirty_list_filename(context)
    with open(dirty_list) as fp:
        for line in fp:
            if table_name.strip() in line.strip():
                return

    raise Exception('Expected table %s to be marked as dirty in %s' % (table_name, dirty_list))

@when('the "{table_name}" is recreated with same data in "{dbname}"')
def impl(context, table_name, dbname):
    select_sql = 'select * into public.temp from %s' % table_name
    execute_sql(dbname, select_sql)
    drop_sql = 'drop table %s' % table_name
    execute_sql(dbname, drop_sql)
    recreate_sql = 'select * into %s from public.temp' % table_name 
    execute_sql(dbname, recreate_sql)
    
@then('verify that plan file has latest timestamp for "{table_name}"')
def impl(context, table_name):
    plan_file = get_plan_filename(context)
    with open(plan_file) as fp:
        for line in fp:
            if table_name in line.strip():
                timestamp = line.split(':')[0].strip()
                if timestamp != context.backup_timestamp:
                    raise Exception('Expected table %s with timestamp %s in plan file %s does not match timestamp %s' \
                                        % (table_name, context.backup_timestamp, plan_file, timestamp))

@given('the row "{row_values}" is inserted into "{table}" in "{dbname}"')
def impl(context, row_values, table, dbname):
    insert_row(context, row_values, table, dbname)

@when('the method get_partition_state is executed on table "{table}" in "{dbname}" for ao table "{ao_table}"')
def impl(context, table, dbname, ao_table):
    (sch, tbl) = table.split('.')
    ao_sch, ao_tbl = ao_table.split('.') 
    part_info = [(1, ao_sch, ao_tbl, tbl)]
    try:
        context.exception = None
        context.partition_list_res = None
        context.partition_list_res = get_partition_state(master_port=os.environ.get('PGPORT'),
                        dbname=dbname, catalog_schema=sch, partition_info=part_info)
    except Exception as e:
        context.exception = e

@then('an exception should be raised with "{txt}"')
def impl(context, txt):
    if not context.exception:
        raise Exception("An exception was not raised")
    output = context.exception.__str__()
    if not txt in output:
        raise Exception("Exception output is not matching: '%s'" % output)

@then('the get_partition_state result should contain "{elem}"')
def impl(context, elem):
    if not context.partition_list_res:
        raise Exception('get_partition_state did not return any results')

    if len(context.partition_list_res) != 1:
        raise Exception('get_partition_state returned more results than expected "%s"' % context.partition_list_res)

    if elem not in context.partition_list_res:
        raise Exception('Expected text "%s" not found in partition list returned by get_partition_state "%s"' % (elem, context.partition_list_res)) 

@given('older backup directories "{dirlist}" exists')
def impl(context, dirlist):
    dirs = [d.strip() for d in dirlist.split(',')]

    for d in dirs:
        if len(d) != 8 or not d.isdigit():
            raise Exception('Invalid directory name provided %s' % d)

    for d in dirs:
        dump_dir = os.path.join(master_data_dir, 'db_dumps', d)
        if os.path.exists(dump_dir):
            continue
        os.makedirs(dump_dir)
        for i in range(10):
            with open(os.path.join(dump_dir, '%s_%s' % (d, i)), 'w'):
                pass

@then('the dump directories "{dirlist}" should not exist')
def impl(context, dirlist):
    dirs = [d.strip() for d in dirlist.split(',')]

    for d in dirs:
        if len(d) != 8 or not d.isdigit():
            raise Exception('Invalid directory name provided %s' % d)

    for d in dirs:
        dump_dir = os.path.join(master_data_dir, 'db_dumps', d)
        if os.path.exists(dump_dir):
            raise Exception('Unexpected directory exists %s' % dump_dir) 

@then('the dump directory for the stored timestamp should exist')
def impl(context):
    if not hasattr(context, 'full_backup_timestamp'):
        raise Exception('Full backup timestamp needs to be stored')
   
    dump_dir = os.path.join(master_data_dir, 'db_dumps', context.full_backup_timestamp[0:8]) 
    if not os.path.exists(dump_dir):
        raise Exception('Expected directory does not exist %s' % dump_dir)

def validate_master_config_backup_files(backup_timestamp):
    master_dump_dir = os.path.join(master_data_dir, 'db_dumps', backup_timestamp[0:8]) 
    dump_files = os.listdir(master_dump_dir)
    for df in dump_files:
        if df.startswith('gp_master_config_files') and df.endswith('.tar'):
            return
    if not exists:
        raise Exception('Config files not backed up on master "%s"' % master_config_file)

def validate_segment_config_backup_files(backup_timestamp):
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    primary_segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary()]

    for ps in primary_segs:
        dump_dir = os.path.join(ps.getSegmentDataDirectory(), 'db_dumps', backup_timestamp[0:8])
        dump_files = ListRemoteFilesByPattern(dump_dir, 
                                              'gp_segment_config_files_*.tar',
                                              ps.getSegmentHostName()).run()
        if len(dump_files) != 1:
            raise Exception('Error in finding config files "%s" for segment %s' % (dump_files, ps.getSegmentDataDirectory())) 
        
@then('config files should be backed up on all segments')
def impl(context):
    if not hasattr(context, 'backup_timestamp'):
        raise Exception('Backup timestamp needs to be stored')
    
    validate_master_config_backup_files(context.backup_timestamp)
    validate_segment_config_backup_files(context.backup_timestamp)

@then('verify that the table "{table_name}" in "{dbname}" has dump info for the stored timestamp')
def impl(context, table_name, dbname):
    dump_history = {}
    dump_history_sql = 'select dump_key,options from public.%s' % table_name 
    dump_history =  getRows(dbname, dump_history_sql)
    for (dump_key,options) in dump_history:
        if context.backup_timestamp == dump_key.strip() and dbname in options:
            return

    raise Exception('Could not find dump info for timestamp %s in %s table' % (context.backup_timestamp, table_name))

@then('verify that database "{dbname}" does not exist')
def impl(context, dbname):
    with dbconn.connect(dbconn.DbURL(dbname='template1')) as conn:
        sql = """select datname from pg_database"""
        dbs = dbconn.execSQL(conn, sql)
        if dbname in dbs:
            raise Exception('Database exists when it shouldnt "%s"' % dbname)

@then('there are no saved data files')
def impl(context):
    clear_all_saved_data_verify_files(context)

@when('the timestamp for database dumps "{db_list}" are stored')
def impl(context, db_list):
    context.db_timestamps = get_timestamp_from_output_for_db(context)

def get_timestamp_from_output_for_db(context):
    db_timestamps = {}
    ts = None
    database = None
    stdout = context.stdout_message
    for line in stdout.splitlines():
        if 'Target database' in line:
            log_msg, delim, database = line.partition('=') 
            db = database.strip()
        elif 'Dump key ' in line:
            log_msg, delim, timestamp = line.partition('=')
            ts = timestamp.strip()
            validate_timestamp(ts)
            if database is None:
                raise Exception('Database not found for timestamp "%s"' % ts)
            db_timestamps[db] = ts
            database = None

    if not db_timestamps:
        raise Exception('No Timestamps found')

    return db_timestamps

@then('the dump timestamp for "{db_list}" are different')
def impl(context, db_list):
    if db_list is None:
        raise Exception('Expected at least 1 database in the list, found none.')

    if not hasattr(context, 'db_timestamps'):
        raise Exception('The database timestamps need to be stored')

    db_names = db_list.strip().split(',')
    for db in db_names:
        if db.strip() not in context.db_timestamps:
            raise Exception('Could not find timestamp for database: %s' % context.db_timestamps)

    timestamp_set = set([v for v in context.db_timestamps.values()]) 
    if len(timestamp_set) != len(context.db_timestamps):
        raise Exception('Some databases have same timestamp: "%s"' % context.db_timestamps)

@given('there is a "{table_type}" table "{table_name}" in "{db_name}" having large number of partitions')
def impl(context, table_type, table_name, db_name):
    create_large_num_partitions(table_type, table_name, db_name)

@given('the length of partition names of table "{table_name}" in "{db_name}" exceeds the command line maximum limit')
def impl(context, table_name, db_name):
    partitions = get_partition_tablenames(table_name, db_name)
    partition_list_string = '' 
    for part in partitions:
        partition_list_string += (part[0] + ',')
    if partition_list_string[-1] == ',':
        parition_list_string = partition_list_string[:-1]
    MAX_COMMAND_LINE_LEN = 100000
    if len(partition_list_string) < MAX_COMMAND_LINE_LEN:
        raise Exception('Expected the length of the string to be greater than %s, but got %s instead' % (MAX_COMMAND_LINE_LEN, len(partition_list_string)))

@given('there is a table-file "{filename}" with tables "{table_list}"')
def impl(context, filename, table_list):
    tables = table_list.split(',')
    with open(filename, 'w') as fd:
        for table in tables:
            fd.write(table.strip() + '\n')

    if not os.path.exists(filename):
        raise Exception('Unable to create file "%s"' % filename)

def create_ext_table_file(file_location):
    with open(file_location, 'w') as fd:
        for i in range(100):
            fd.write('abc, 10, 10\n')

def get_host_and_port():
    if 'PGPORT' not in os.environ:
        raise Exception('PGPORT needs to be set in the environment')
    port = os.environ['PGPORT']
    gparray  = GpArray.initFromCatalog(dbconn.DbURL())
    master_host = None
    for seg in gparray.getDbList():
        if seg.isSegmentMaster():
            master_host = seg.getSegmentAddress()

    if master_host is None:
        raise Exception('Unable to determine the master hostname') 

    return (master_host, port)

@given('there is an external table "{tablename}" in "{dbname}" with data for file "{file_location}"')
def impl(context, tablename, dbname, file_location):
    create_ext_table_file(file_location) 
    host, port = get_host_and_port()
    ext_table_sql = """CREATE EXTERNAL WEB TABLE %s(name text, column1 int, column2 int) EXECUTE 'cat %s 2> /dev/null || true' 
                       ON MASTER FORMAT 'CSV' (DELIMITER ',')""" % (tablename, file_location)
    execute_sql(dbname, ext_table_sql)
   
    verify_ext_table_creation_sql = """SELECT count(*) FROM pg_class WHERE relname = '%s'""" % tablename 
    row_count = getRows(dbname, verify_ext_table_creation_sql)[0][0]
    if row_count != 1:
        raise Exception('Creation of external table failed for "%s:%s, row count = %s"' % (file_location, tablename, row_count))

@then('verify that there is no "{tablename}" in the "{file_type}" file in "{backup_dir}"')
def impl(context, tablename, file_type, backup_dir):
    dump_dir = backup_dir if len(backup_dir.strip()) != 0 else master_data_dir

    filename = '%s/db_dumps/%s/gp_dump_%s_%s' % (dump_dir, context.backup_timestamp[0:8], context.backup_timestamp, file_type)
    with open(filename) as fd:
        for line in fd:
            if tablename in line:
                raise Exception('Found an external table in the file: "%s" in line: "%s"' %(filename, line))  

@then('verify that exactly "{num_tables}" tables in "{dbname}" have been restored')
def impl(context, num_tables, dbname):
    validate_num_restored_tables(context, num_tables, dbname)

@then('the user runs gpdbrestore on dump date directory with options "{options}"')
def impl(context, options):
    command = 'gpdbrestore -e -b %s %s -a' % (context.backup_timestamp[0:8], options)
    run_gpcommand(context, command)

@then('the timestamps should be printed in sorted order')
def impl(context):
    stdout_lines = context.stdout_message.split('\n')
    process_ts = False
    timestamps = []
    for line in stdout_lines:
        if '--------------------------' in line:
            process_ts = True
        elif process_ts:
            if 'Enter timestamp number to restore' not in line:
                timestamps.append(line.strip().split('......')[-1].strip()) 
            else:
                process_ts = False
                break
            
    timestamps = [ts.split()[0]+ts.split()[1] for ts in timestamps] 

    sorted_timestamps = sorted(timestamps, key=lambda x: int(x))    
    
    if sorted_timestamps != timestamps:
        raise Exception('Timestamps are not in sorted order "%s"' % timestamps)

@given('there are "{table_count}" "{tabletype}" tables "{table_name}" with data in "{dbname}"')
def impl(context, table_count, tabletype, table_name, dbname):
    table_count = int(table_count)
    for i in range(1, table_count+1):
        tablename = "%s_%s" % (table_name, i)
        create_database_if_not_exists(context, dbname)
        drop_table_if_exists(context, table_name=tablename, dbname=dbname)
        create_partition(context, tablename, tabletype, dbname, compression_type=None, partition=False)

@given('the tables "{table_list}" are in dirty hack file "{dirty_hack_file}"')
def impl(context, table_list, dirty_hack_file):
    tables = [t.strip() for t in table_list.split(',')]
    
    with open(dirty_hack_file, 'w') as fd:
        for t in tables:
            fd.write(t + '\n')

    if not os.path.exists(dirty_hack_file):
        raise Exception('Failed to create dirty hack file "%s"' % dirty_hack_file)

@given('partition "{part_num}" of partition tables "{table_list}" in "{dbname}" in schema "{schema}" are in dirty hack file "{dirty_hack_file}"')
def impl(context, part_num, table_list, dbname, schema, dirty_hack_file):
    tables = table_list.split(',')
    with open(dirty_hack_file, 'w') as fd:
        part_num = int(part_num.strip())
        for table in tables:
            part_t = get_partition_names(schema, table.strip(), dbname, 1, part_num)
            if len(part_t) < 1 or len(part_t[0]) < 1:
                print part_t
            partition_name = part_t[0][0].strip()
            fd.write(schema + '.' + partition_name + '\n')

    if not os.path.exists(dirty_hack_file):
        raise Exception('Failed to write to dirty hack file "%s"' % dirty_hack_file)

@then('verify that the config files are backed up with the stored timestamp')
def impl(context):
    if not hasattr(context, 'backup_timestamp'):
        raise Exception('Timestamp needs to be stored')
    
    config_file = os.path.join(master_data_dir, 'db_dumps', context.backup_timestamp[0:8],
                               'gp_master_config_files_%s.tar' % context.backup_timestamp)
    if not os.path.exists(config_file):
        raise Exception('Failed to locate config file on master "%s"' % config_file)

    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    
    primary_segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]

    for seg in primary_segs:
        segment_config_file = os.path.join(seg.getSegmentDataDirectory(), 'db_dumps', context.backup_timestamp[0:8],
                                           'gp_segment_config_files_0_%s_%s.tar' % (seg.getSegmentDbId(), context.backup_timestamp)) 
        if not CheckRemoteFile(segment_config_file, seg.getSegmentAddress()):
            raise Exception('Failed to locate "%s" on "%s"' % segment_config_file, seg.getSegmentDataDirectory())

@then('verify that the list of stored timestamps is printed to stdout')
def impl(context):
    found_ts = 0
    stdout = context.stdout_message
    for ts in context.inc_backup_timestamps:
        for line in stdout.splitlines():
            ts_str = 'Backup Timestamp: %s' % ts
            if ts_str in line:
                found_ts += 1
                          
    print 'context.inc_backup_timestamps = ', context.inc_backup_timestamps
    if found_ts != len(context.inc_backup_timestamps):
        raise Exception('Expected "%d" timestamps but found "%d" timestamps' % (len(context.inc_backup_timestamps), found_ts))

@given('there is a "{table_type}" table "{tablename}" with funny characters in "{dbname}"')
def impl(context, table_type, tablename, dbname):
    funny_chars_table_name_sql = """create table "%s
        new line in it" (a int)""" % tablename.strip()

    table_type = table_type.strip()
    if table_type == 'ao':
        funny_chars_table_name_sql += ' with(appendonly=true)'
    elif table_type == 'co':
        funny_chars_table_name_sql += ' with(appendonly=true, orientation=column)'
    elif table_type == 'heap':
        pass
    else:
        raise Exception('Unknown table type specified "%s"' % table_type) 

    execute_sql(dbname.strip(), funny_chars_table_name_sql)

@then('verify that the tuple count of all appendonly tables are consistent in "{dbname}"')
def impl(context, dbname):
    ao_partition_list = get_partition_list('ao', dbname)
    verify_stats(dbname, ao_partition_list)
    co_partition_list = get_partition_list('co', dbname)
    verify_stats(dbname, co_partition_list)

@then('verify that there are no aoco stats in "{dbname}" for table "{tables}"')
def impl(context, dbname, tables):
    tables = tables.split(',')
    for t in tables:
        validate_no_aoco_stats(context, dbname, t.strip())

@when('the performance timer is started')
def impl(context):
    context.performance_timer = time.time()

@then('the performance timer should be less then "{num_seconds}" seconds')
def impl(context, num_seconds):
    max_seconds = float(num_seconds)
    current_time = time.time()
    elapsed = current_time - context.performance_timer
    if elapsed > max_seconds:
        raise Exception("Performance timer ran for %.1f seconds but had a max limit of %.1f seconds" % (elapsed, max_seconds))
    print "Elapsed time was %.1f seconds" % elapsed


@then('the file "{filename}" is removed from the system')
def impl(context, filename): 
    os.remove(filename)

@given('the client program "{program_name}" is present under {parent_dir} in "{sub_dir}"')
def impl(context, program_name, parent_dir, sub_dir):

    if parent_dir == 'CWD':
        parent_dir = os.getcwd()
    program_path = '%s/%s/%s' % (parent_dir, sub_dir, program_name)

    print program_path
    if not os.path.isfile(program_path):
        raise Exception('gpfdist client progream does not exist: %s' % (program_path))

@when('the user runs client program "{program_name}" from "{subdir}" under {parent_dir}')
def impl(context, program_name, subdir, parent_dir):
    if parent_dir == 'CWD':
        parent_dir = os.getcwd()

    command_line = "python %s/%s/%s" % (parent_dir, subdir, program_name)
    run_command(context, command_line)

@then('the gpfdist should print {msg} to "{filename}" under {parent_dir}')
def impl(context, msg, filename, parent_dir):
    if parent_dir == 'CWD':
        parent_dir = os.getcwd()
    filepath = '%s/%s' % (parent_dir, filename)

    with open(filepath, 'r') as fp:
        for line in fp:
            if msg in line:
                return

        raise Exception('Log file %s did not contain the message %s' % (filepath, msg))

@given('the "{process_name}" process is killed')
@then('the "{process_name}" process is killed')
@when('the "{process_name}" process is killed')
def impl(context, process_name):
    run_command(context, 'pkill %s' % process_name)

@then('the client program should print {msg} to stdout with value in range {min_val} to {max_val}')
def impl(context, msg, min_val, max_val):
    stdout = context.stdout_message

    for line in stdout:
        if msg in line:
            val = re.finadall(r'\d+', line)
            if not val:
                raise Exception('Expected a numeric digit after message: %s' % msg) 
            if len(val) > 1:
                raise Exception('Expected one numeric digit after message: %s' % msg) 

            if val[0] < min_val or val[0] > max_val:
                raise Exception('Value not in acceptable range %s' % val[0]) 

@given('the directory "{dirname}" exists in current working directory')
def impl(context, dirname):
    dirname = os.path.join(os.getcwd(), dirname)
    if os.path.isdir(dirname):
        shutil.rmtree(dirname, ignore_errors=True)
        if os.path.isdir(dirname):
            raise Exception("directory '%s' not removed" % dirname)
    os.mkdir(dirname)
    if not os.path.isdir(dirname):
        raise Exception("directory '%s' not created" % dirname)

@given('the directory "{dirname}" does not exist in current working directory')
def impl(context, dirname):
    dirname = os.path.join(os.getcwd(), dirname)
    if os.path.isdir(dirname):
        shutil.rmtree(dirname, ignore_errors=True)
    if os.path.isdir(dirname):
        raise Exception("directory '%s' not removed" % dirname)

@when('the data line "{dataline}" is appened to "{fname}" in cwd')
def impl(context, dataline, fname):
    fname = os.path.join(os.getcwd(), fname)
    with open(fname, 'a') as fd:
        fd.write("%s\n" % dataline)


@when('a "{readwrite}" external table "{tname}" is created on file "{fname}" in "{dbname}"')
def impl(context, readwrite, tname, fname, dbname):

    hostname = socket.gethostname()

    sql = """CREATE %s EXTERNAL TABLE  
            %s (name varchar(255), id int) 
            LOCATION ('gpfdist://%s:8080/%s') 
            FORMAT 'text'
            (DELIMITER '|');
            """ % (readwrite, tname, hostname, fname)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, sql)
        conn.commit()

@given('the external table "{tname}" does not exist in "{dbname}"')
def impl(context, tname, dbname):
    drop_external_table_if_exists(context, table_name=tname, dbname=dbname)


@when('all rows from table "{tname}" db "{dbname}" are stored in the context')
def impl(context, tname, dbname):

    context.stored_rows = []

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        sql = "SELECT * FROM %s" % tname 
        curs = dbconn.execSQL(conn, sql)
        context.stored_rows = curs.fetchall()

@then('validate that "{dataline}" "{formatter}" seperated by "{delim}" is in the stored rows')
def impl(context, dataline, formatter, delim):
    lookfor = dataline.split(delim)
    formatter = formatter.split(delim)

    for i in range(len(formatter)):
        if formatter[i] == 'int':
            lookfor[i] = int(lookfor[i])

    if lookfor not in context.stored_rows:
        print context.stored_rows
        print lookfor
        raise Exception("'%s' not found in stored rows" % dataline)

@then('validate that following rows are in the stored rows')
def impl(context):
    for row in context.table:
        found_match = False

        for stored_row in context.stored_rows:
            match_this_row = True
            for i in range(len(stored_row)):
                if row[i] != str(stored_row[i]):
                    match_this_row = False
                    break

            if match_this_row:
                found_match = True
                break

        if not found_match:
            print context.stored_rows
            raise Exception("'%s' not found in stored rows" % row)


@then('validate that stored rows has "{numlines}" lines of output')
def impl(context, numlines):
    num_found = len(context.stored_rows)
    numlines = int(numlines)
    if num_found != numlines:
        raise Exception("Found %d of stored query result but expected %d records" % (num_found, numlines))

def get_standby_host():
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    segments = gparray.getDbList() 
    standby_master = [seg.getSegmentHostName() for seg in segments if seg.isSegmentStandby()]
    if len(standby_master) > 0:
        return standby_master[0]
    else:
        return []

@given('user does not have ssh permissions') 
def impl(context):
    user_home = os.environ.get('HOME')
    authorized_keys_file = '%s/.ssh/authorized_keys' % user_home
    if os.path.exists(os.path.abspath(authorized_keys_file)):
       shutil.move(authorized_keys_file, '%s.bk' % authorized_keys_file) 

@then('user has ssh permissions') 
def impl(context):
    user_home = os.environ.get('HOME')
    authorized_keys_backup_file = '%s/.ssh/authorized_keys.bk' % user_home
    if os.path.exists(authorized_keys_backup_file):
        shutil.move(authorized_keys_backup_file, authorized_keys_backup_file[:-3])

def delete_data_dir(host):
    cmd = Command(name='remove data directories',
           cmdStr='rm -rf %s' % master_data_dir,
           ctxt=REMOTE,
           remoteHost=host)
    cmd.run(validateAfter=True)

@when('the user initializes standby master on "{hostname}"') 
def impl(context, hostname):
    port = os.environ.get('PGPORT')
    if hostname.strip() == 'mdw':
        if not hasattr(context, 'master') or not hasattr(context, 'standby_host'):
            raise Exception('Expected master to be saved but was not found')
        delete_data_dir(context.master)
        cmd = Command(name='gpinitstandby',
                      cmdStr='PGPORT=%s MASTER_DATA_DIRECTORY=%s gpinitstandby -a -s %s' % (port, master_data_dir, context.master),
                      ctxt=REMOTE,
                      remoteHost=context.standby_host)
        cmd.run(validateAfter=True)
        return
    elif hostname.strip() == 'smdw':
        context.master = get_master_hostname('template1')[0][0]
        context.ret_code = 0
        context.stdout_message = ''
        context.error_message = ''
        segment_hostlist = get_segment_hostlist()    
        segment_hosts = [seg for seg in segment_hostlist if seg != context.master] 
        context.standby_host = segment_hosts[0]
        context.standby_was_initialized = False
        standby = get_standby_host()

        if standby:
            context.standby_was_initialized = True
            context.standby_host = standby
            return
    
        delete_data_dir(context.standby_host)
        cmd = "gpinitstandby -a -s %s" % context.standby_host
        run_gpcommand(context, cmd)
    else:
        raise Exception('Invalid host type specified "%s"' % hostname)

@when('the user runs the query "{query}" on "{dbname}"')
def impl(context, query, dbname):
    thread.start_new_thread(getRows, (dbname, query))
    time.sleep(30)

@given('we have exchanged keys with the cluster')
def impl(context):
    hostlist = get_all_hostnames_as_list(context, 'template1')
    host_str = ' -h '.join(hostlist)
    cmd_str = 'gpssh-exkeys %s' % host_str
    run_gpcommand(context, cmd_str) 

@then('the temp files "{filename_prefix}" are not created in the system')
def impl(context, filename_prefix):
    hostlist = get_all_hostnames_as_list(context, 'template1')
    print hostlist
    file_pattern = 'ls /tmp/%s* | wc -l' % filename_prefix
    print file_pattern
    for host in hostlist:
        cmd = Command(name='check for temp files', 
                      cmdStr=file_pattern, 
                      ctxt=REMOTE, 
                      remoteHost=host)
        cmd.run(validateAfter=True)
        results = cmd.get_results()
        if int(results.stdout.strip()) > 0:
            raise Exception('Temp files with prefix %s are not cleaned up on host %s after gpcrondump' % (filename_prefix, host))

@when('the user activates standby on the standbyhost')
def impl(context):
    port = os.environ.get('PGPORT')
    cmd = 'PGPORT=%s MASTER_DATA_DIRECTORY=%s gpactivatestandby -d %s -fa' % (port, master_data_dir, master_data_dir) 
    cmd = Command('run remote command', cmd, ctxt=REMOTE, remoteHost=context.standby_host)
    cmd.run(validateAfter=True)

@then('the user runs the command "{cmd}" from standby')
@when('the user runs the command "{cmd}" from standby')
def impl(context, cmd):
    port = os.environ.get('PGPORT')
    cmd = 'PGPORT=%s MASTER_DATA_DIRECTORY=%s %s' % (port, master_data_dir, cmd) 
    cmd = Command('run remote command', cmd, ctxt=REMOTE, remoteHost=context.standby_host)
    cmd.run(validateAfter=True)

@given('user kills a primary postmaster process')
def impl(context):
    gparray=GpArray.initFromCatalog(dbconn.DbURL())

    for seg in gparray.getDbList():
        if seg.isSegmentPrimary():
            seg_data_dir = seg.getSegmentDataDirectory()
            seg_host = seg.getSegmentHostName()
            break
    
    pid = get_pid_for_segment(seg_data_dir, seg_host) 
    if pid is None:
        raise Exception('Unable to locate segment "%s" on host "%s"' % (seg_data_dir, seg_host))

    kill_process(int(pid), seg_host)

    time.sleep(10)

    pid = get_pid_for_segment(seg_data_dir, seg_host)
    if pid is not None:
        raise Exception('Unable to kill postmaster with pid "%d" datadir "%s"' % (pid, seg_data_dir)) 

@when('the temp files "{filename_prefix}" are removed from the system')
@given('the temp files "{filename_prefix}" are removed from the system')
def impl(context, filename_prefix):
    hostlist = get_all_hostnames_as_list(context, 'template1')
    print hostlist
    for host in hostlist:
        cmd = Command(name='remove data directories',
                cmdStr='rm -rf /tmp/%s*' % filename_prefix,
                ctxt=REMOTE,
                remoteHost=host)
        cmd.run(validateAfter=True)

@then('the standby is initialized if required')
def impl(context):
    if context.standby_was_initialized: 
        delete_data_dir(context.standby_host)
        cmd = Command('create the standby', cmdStr='gpinitstandby -s %s -a' % context.standby_host)
        cmd.run(validateAfter=True)

@given('user can start transactions')
def impl(context):
    num_retries = 50
    attempt = 0
    while attempt < num_retries:
        try:
            with dbconn.connect(dbconn.DbURL()) as conn:
                break
        except Exception as e:
            attempt +=1 
            pass
        time.sleep(1)
   
    if attempt == num_retries:
        raise Exception('Unable to establish a connection to database !!!') 
    
@when('user runs "{cmd}" with sudo access')
def impl(context, cmd):
    gphome = os.environ.get('GPHOME')
    python_path = os.environ.get('PYTHONPATH')
    python_home = os.environ.get('PYTHONHOME')
    ld_library_path = os.environ.get('LD_LIBRARY_PATH')
    path = os.environ.get('PATH')
    cmd_str = """sudo GPHOME=%s 
                    PATH=%s 
                    PYTHONHOME=%s 
                    PYTHONPATH=%s 
                    LD_LIBRARY_PATH=%s %s/bin/%s""" % (gphome, path, python_home, python_path, ld_library_path, gphome, cmd) 
    run_command(context, cmd_str)

def verify_num_files(results, expected_num_files, timestamp):
    num_files = results.stdout.strip()
    if num_files != expected_num_files:
        raise Exception('Expected "%s" files with timestamp key "%s" but found "%s"' % (expected_num_files, timestamp,num_files))
    
    
def verify_timestamps_on_master(timestamp, dump_type):
    list_cmd = 'ls -l %s/db_dumps/%s/*%s* | wc -l' % (master_data_dir, timestamp[:8], timestamp) 
    cmd = Command('verify timestamps on master', list_cmd)
    cmd.run(validateAfter=True)
    expected_num_files = '10' if dump_type == 'incremental' else '8'
    verify_num_files(cmd.get_results(), expected_num_files, timestamp)

def verify_timestamps_on_segments(timestamp):
    gparray = GpArray.initFromCatalog(dbconn.DbURL()) 
    primary_segs = [segdb for segdb in gparray.getDbList() if segdb.isSegmentPrimary()]

    for seg in primary_segs:
        db_dumps_dir = os.path.join(seg.getSegmentDataDirectory(), 
                                    'db_dumps',
                                    timestamp[:8])
        list_cmd = 'ls -l %s/*%s* | wc -l' % (db_dumps_dir, timestamp) 
        cmd = Command('get list of dump files', list_cmd, ctxt=REMOTE, remoteHost=seg.getSegmentHostName())
        cmd.run(validateAfter=True)
        verify_num_files(cmd.get_results(), '2', timestamp)
        
@then('verify that "{dump_type}" dump files have stored timestamp in their filename')
def impl(context, dump_type):
    if dump_type.strip().lower() != 'full' and dump_type.strip().lower() != 'incremental':
        raise Exception('Invalid dump type "%s"' % dump_type)

    verify_timestamps_on_master(context.backup_timestamp, dump_type.strip().lower())
    verify_timestamps_on_segments(context.backup_timestamp)

def validate_files(file_list, pattern_list, expected_file_count):
    file_count = 0 
    for pattern in pattern_list:
        pat = re.compile(pattern)
        pat_found = False
        for f in file_list:
            m = pat.search(f.strip())
            if m is not None:
                pat_found = True
                file_count += 1

        if not pat_found:
            raise Exception('Expected file not found for pattern: "%s" in file list "%s"' % (pattern, file_list))

    if file_count != expected_file_count:
        raise Exception('Expected count of %d does not match actual count %d in file list "%s"' % (expected_file_count, file_count, file_list))
     
@then('the "{file_type}" file under "{directory}" with options "{options}" is validated after dump operation')
def impl(context, file_type, directory, options):
    backup_dir = directory if directory.strip() != '' else master_data_dir  
    if len(options.split(',')) > 3:
        raise Exception('Invalid options specified "%s"' % options) 
    option_list = options.split(',')

    pipe_file_count = 1 + get_num_segments(primary=True, mirror=False, master=True, standby=False)
    reg_file_count = 6

    pipes_pattern_list = ['gp_dump_.*_%s.*(?:\.gz)?' % context.backup_timestamp]
    regular_pattern_list = ['gp_cdatabase_1_1_%s' % context.backup_timestamp, 'gp_dump_%s.*' % context.backup_timestamp, 'gp_dump_status_1_1_%s' % context.backup_timestamp]

    if '-G' in option_list: 
    if '-g' in option_list:
        reg_file_count += get_num_segments(primary=True, mirror=False, master=True, standby=False) 
        regular_pattern_list += ['gp_master_config_files_%s.*' % context.backup_timestamp, 'gp_segment_config_files_.*_.*_%s.*' % context.backup_timestamp]
    if '--incremental' in option_list:
        regular_pattern_list += ['gp_dump_%s.*' % context.full_backup_timestamp]
        reg_file_count += 1
     
file_type.strip())

    with open(filename) as fp:
        file_list = fp.readlines()

    if file_type == 'pipes':
        validate_files(file_list, pipes_pattern_list, pipe_file_count)
    elif file_type == 'regular_files':
        validate_files(file_list, regular_pattern_list, reg_file_count)
        
@then('the "{file_type}" file under "{directory}" with options "{options}" is validated after dump operation')
def impl(context, file_type, directory, options):
    backup_dir = directory if directory.strip() != '' else master_data_dir  
    if len(options.split(',')) > 3:
        raise Exception('Invalid options specified "%s"' % options) 
    option_list = options.split(',')

    pipe_file_count = 1 + get_num_segments(primary=True, mirror=False, master=True, standby=False)
    reg_file_count = 6

    pipes_pattern_list = ['gp_dump_.*_%s.*(?:\.gz)?' % context.backup_timestamp]
    regular_pattern_list = ['gp_cdatabase_1_1_%s' % context.backup_timestamp, 'gp_dump_%s.*' % context.backup_timestamp, 'gp_dump_status_1_1_%s' % context.backup_timestamp]

    if '-G' in option_list: 
        pipe_file_count += 1
        pipes_pattern_list += ['gp_global_1_1_%s' % context.backup_timestamp]
    if '-g' in option_list:
        pipe_file_count += get_num_segments(primary=True, mirror=False, master=True, standby=False) 
        pipes_pattern_list += ['gp_master_config_files_%s.*' % context.backup_timestamp, 'gp_segment_config_files_.*_.*_%s.*' % context.backup_timestamp]
    if '--incremental' in option_list:
        regular_pattern_list += ['gp_dump_%s.*' % context.full_backup_timestamp]
        reg_file_count += 1
     
    filename = '%s/db_dumps/%s/gp_dump_%s_%s' % (backup_dir, context.backup_timestamp[0:8], context.backup_timestamp, 
file_type.strip())

    with open(filename) as fp:
        file_list = fp.readlines()

    if file_type == 'pipes':
        validate_files(file_list, pipes_pattern_list, pipe_file_count)
    elif file_type == 'regular_files':
        validate_files(file_list, regular_pattern_list, reg_file_count)
        
@then('the timestamp key "{timestamp_key}" for gpcrondump is stored')
def impl(context, timestamp_key):
    context.backup_timestamp = timestamp_key

@then('there are no dump files created under "{dir}"')
def impl(context, dir):
    master_dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    primary_segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary()]
    unwanted_files = ['gp_dump_%s_pipes' % context.backup_timestamp, 'gp_dump_%s_regular_files' % context.backup_timestamp] 
    for seg in primary_segs:
        seg_dump_dir = seg.getSegmentDataDirectory()
        segment_dump_dir =  dir if len(dir.strip()) != 0 else seg_dump_dir
        cmd = Command('check dump files', 'ls %s/db_dumps/%s' % (segment_dump_dir, context.backup_timestamp[0:8]), ctxt=REMOTE, remoteHost=seg.getSegmentHostName())
        cmd.run(validateAfter=False) #because we expect ls to fail
        results = [r for r in cmd.get_results().stdout.strip().split() if r.strip() not in unwanted_files]
        if len(results) != 0:
            print 'results = ', results
            raise Exception('Found extra dump files on the segment %s under %s/db_dumps/%s' % (seg.getSegmentDataDirectory(), segment_dump_dir, context.backup_timestamp[0:8]))
    
    cmd = Command('check dump files', 'ls %s/db_dumps/%s' % (master_dump_dir, context.backup_timestamp[0:8]))
    cmd.run(validateAfter=True)
    results = cmd.get_results().stdout.strip().split('\n')
    
    if len(results) != 2:
        raise Exception('Found extra dump files %s on the master under %s' % (results, master_dump_dir))

    pipes_file = 'gp_dump_%s_pipes' % context.backup_timestamp
    reg_file = 'gp_dump_%s_regular_files' % context.backup_timestamp
    if not pipes_file in results or not  reg_file in results:
        raise Exception('Found invalid files %s under dump dir %s' % (results, master_dump_dir))

@then('the named pipes are created for the timestamp "{timestamp_key}" under "{dir}"')
def impl(context, timestamp_key, dir):
    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    pipes_filename = '%s/db_dumps/%s/gp_dump_%s_pipes' % (dump_dir, timestamp_key[0:8], timestamp_key)

    with open(pipes_filename, 'r') as fp:
        for f in fp:
            (host, filename) = [t.strip() for t in f.split(':')]
            cmd_str = 'mkdir -p %s' % os.path.dirname(filename)
            cmd = Command('create named pipes directory', cmd_str, ctxt=REMOTE, remoteHost=host)
            cmd.run(validateAfter=True)
            results = cmd.get_results()
            if int(results.rc) != 0:
                raise Exception('Non zero return code during makedirs command')

            cmd = Command('create named pipes', 'mkfifo %s' % filename, ctxt=REMOTE, remoteHost=host)
            cmd.run(validateAfter=True)
            results = cmd.get_results()
            if int(results.rc) != 0:
                raise Exception('Non zero return code during mkfifo command')

@then('the named pipes are validated against the timestamp "{timestamp_key}" under "{dir}"')
def impl(context, timestamp_key, dir):
    dump_dir = dir if len(dir.strip()) != 0 else master_data_dir
    pipes_filename = '%s/db_dumps/%s/gp_dump_%s_pipes' % (dump_dir, timestamp_key[0:8], timestamp_key)

    with open(pipes_filename, 'r') as fp:
        for f in fp:
            (host, filename) = [t.strip() for t in f.split(':')]
            cmd = Command('create named pipes', 'file %s' % filename, ctxt=REMOTE, remoteHost=host)
            cmd.run(validateAfter=True)
            results = cmd.get_results()
            if int(results.rc) != 0:
                raise Exception('Non zero return code during mkfifo command')
            if not 'named pipe' in results.stdout:
                raise Exception('Expected %s to be a named pipe' % filename)

@then('the named pipe script for the "{operation}" is run for the files under "{dump_directory}"')
def impl(context, operation, dump_directory):
    dump_dir = dump_directory if len(dump_directory.strip()) != 0 else master_data_dir
    if operation == 'restore' and hasattr(context, 'inc_backup_timestamps'):
        if not hasattr(context, 'backup_timestamp'):
            raise Exception('Backup timestamp not stored')
        for ts in context.inc_backup_timestamps:
            open_named_pipes(context, operation, ts, dump_dir) 
    else:
        open_named_pipes(context, operation, context.backup_timestamp, dump_dir)

@then('close all opened pipes')
def impl(context):
    hosts = set(get_all_hostnames_as_list(context, 'template1'))
    for h in hosts:
        find_cmd = Command('get list of pipe processes',
                      "ps -eaf | grep _pipe.py | grep -v grep | grep -v ssh",
                      ctxt=REMOTE,
                      remoteHost=h) 
        results = find_cmd.get_results().stdout.strip().split('\n')
        for process in results:
            pid = process.split()[1].strip()
            print 'pid = %s on host %s' % (pid, h) 
            cmd = Command('Kill pipe process',
                          "kill %s" % pid,
                          ctxt=REMOTE,
                          remoteHost=h)
            cmd.run(validateAfter=True)
      
        find_cmd.run() # We expecte failure here

        results = find_cmd.get_results().stdout.strip()
        if results:
            raise Exception('Unexpected pipe processes found "%s"' % results) 
    

def open_named_pipes(context, operation, timestamp, dump_dir):
    sleeptime = 5
    pipes_filename = '%s/db_dumps/%s/gp_dump_%s_pipes' % (dump_dir, timestamp[0:8], timestamp)


    filename = os.path.join(os.getcwd(), './gppylib/test/data/%s_pipe.py' % operation)


    for seg in segs:
        cmdStr = "%s -o 'StrictHostKeyChecking no' %s %s:%s" % (findCmdInPath('scp'), filename, seg, '/tmp')
        run_command(context, cmdStr)

    with open(pipes_filename, 'r') as fp:
        for f in fp:
            (host, filename) = [t.strip() for t in f.split(':')]
            cmd = Command('run pipe script', 'sh -c "python /tmp/%s_pipe.py %s" &>/dev/null &' % (operation, filename),
                          ctxt=REMOTE, remoteHost=host)
            cmd.run(validateAfter=True)
            results = cmd.get_results()

@then('close all opened pipes')
def impl(context):
    hosts = set(get_all_hostnames_as_list(context, 'template1'))
    for h in hosts:
        find_cmd = Command('get list of pipe processes',
                      "ps -eaf | grep _pipe.py | grep -v grep | grep -v ssh",
                      ctxt=REMOTE,
                      remoteHost=h) 
        find_cmd.run()
        results = find_cmd.get_results().stdout.strip().split('\n')
        for process in results:
            if not process.strip():
                continue
            pid = process.split()[1].strip()
            print 'pid = %s on host %s' % (pid, h) 
            cmd = Command('Kill pipe process',
                          "kill %s" % pid,
                          ctxt=REMOTE,
                          remoteHost=h)
            cmd.run(validateAfter=True)
      
        find_cmd.run() # We expecte failure here

        results = find_cmd.get_results().stdout.strip()
        if results:
            raise Exception('Unexpected pipe processes found "%s"' % results) 
    

def open_named_pipes(context, operation, timestamp, dump_dir):
    sleeptime = 5
    pipes_filename = '%s/db_dumps/%s/gp_dump_%s_pipes' % (dump_dir, timestamp[0:8], timestamp)


    filename = os.path.join(os.getcwd(), './gppylib/test/data/%s_pipe.py' % operation)

    segs = get_all_hostnames_as_list(context, 'template1')

    for seg in segs:
        cmdStr = "%s -o 'StrictHostKeyChecking no' %s %s:%s" % (findCmdInPath('scp'), filename, seg, '/tmp')
        run_command(context, cmdStr)

    with open(pipes_filename, 'r') as fp:
        for f in fp:
            (host, filename) = [t.strip() for t in f.split(':')]
            cmd = Command('run pipe script', 'sh -c "python /tmp/%s_pipe.py %s" &>/dev/null &' % (operation, filename),
                          ctxt=REMOTE, remoteHost=host)
            cmd.run(validateAfter=True)
            time.sleep(sleeptime)
            results = cmd.get_results()

@given('the core dump directory is stored')
def impl(context):
    with open('/etc/sysctl.conf', 'r') as fp:
        for line in fp:
            if 'kernel.core_pattern' in line:
                context.core_dir = os.path.dirname(line.strip().split('=')[1])

@given('the number of core files "{stage}" running "{utility}" is stored')
@then('the number of core files "{stage}" running "{utility}" is stored')
def impl(context, stage, utility):
    core_files_count = 0
    files_list = os.listdir(context.core_dir)
    for f in files_list:
        if f.startswith('core'):
            core_files_count += 1 

    if stage.strip() == 'before':
        context.before_core_count = core_files_count 
    elif stage.strip() == 'after':
        context.after_core_count = core_files_count 
    else:
        raise Exception('Invalid stage entered: %s' % stage)

@then('the number of core files is the same')
def impl(context):
    if not hasattr(context, 'before_core_count'):
        raise Exception('Core file count not stored before operation') 
    if not hasattr(context, 'after_core_count'):
        raise Exception('Core file count not stored after operation')

    if context.before_core_count != context.after_core_count:
        raise Exception('Core files count before %s does not match after %s' % (context.before_core_count, context.after_core_count))


@given('the gpAdminLogs directory has been backed up')
def impl(context):
    src = os.path.join(os.path.expanduser('~'), 'gpAdminLogs') 
    dest = os.path.join(os.path.expanduser('~'), 'gpAdminLogs.bk') 
    shutil.move(src, dest)

@given('the user does not have "{access}" permission on the home directory')
def impl(context, access):
    home_dir = os.path.expanduser('~')
    context.orig_write_permission = check_user_permissions(home_dir, 'write')
    if access == 'write':
        cmd = "sudo chmod u-w %s" % home_dir
    run_command(context, cmd)

    if check_user_permissions(home_dir, access):
        raise Exception('Unable to change "%s" permissions for the directory "%s"' % (access, home_dir))
    
@then('the "{filetype}" path "{file_path}" should "{cond}" exist')
def impl(context, filetype, file_path, cond):
    cond = cond.strip()
    if file_path[0] == '~':
        file_path = os.path.join(os.path.expanduser('~'), file_path[2:])

    if filetype == 'file':
        existence_check_fn = os.path.isfile
    elif filetype == 'directory':
        existence_check_fn  = os.path.isdir
    else:
        raise Exception('File type should be either file or directory')

    if cond == '' and not existence_check_fn(file_path):
        raise Exception('The %s "%s" does not exist' % (filetype, file_path))
    elif cond == 'not' and existence_check_fn(file_path):
        raise Exception('The %s "%s" exist' % (filetype, file_path))

@then('the directory "{file_path}" is removed')
def impl(context, file_path):

    if file_path[0] == '~':
        file_path = os.path.join(os.path.expanduser('~'), file_path[2:]) 
    backup_file_path = file_path + '.bk'

    if not os.path.exists(backup_file_path):
        raise Exception('Backup file "%s" must exist in order to delete the file "%s"' % (backup_file_path, file_path))

    if '*' in file_path:
        raise Exception('WildCard found in file path !!!!. Cannot delete')

    run_command(context, 'rm -rf %s' % file_path)

   
@given('below sql is executed in "{dbname}" db')
@when('below sql is executed in "{dbname}" db')
def impl(context, dbname):
    sql = context.text
    execute_sql(dbname, sql)

@when('sql "{sql}" is executed in "{dbname}" db')
def impl(context, sql, dbname):
    execute_sql(dbname, sql)

@when('execute following sql in db "{dbname}" and store result in the context')
def impl(context, dbname):
    context.stored_rows = []

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        curs = dbconn.execSQL(conn, context.text)
        context.stored_rows = curs.fetchall()


@when('execute sql "{sql}" in db "{dbname}" and store result in the context')
def impl(context, sql, dbname):
    context.stored_rows = []

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        curs = dbconn.execSQL(conn, sql)
        context.stored_rows = curs.fetchall()

@then('validate that "{message}" is in the stored rows')
def impl(context, message):
    for row in context.stored_rows:
        for column in row:
            if message in column:
                return

    print context.stored_rows
    print message 
    raise Exception("'%s' not found in stored rows" % message)

@then('verify that file "{filename}" exists under "{path}"')
def impl(context, filename, path):
    fullpath = "%s/%s" % (path, filename)
    fullpath = os.path.expandvars(fullpath)

    if not os.path.exists(fullpath):
        raise Exception('file "%s" is not exist' % fullpath)
