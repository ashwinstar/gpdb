import os
import fnmatch
import tempfile
import glob
#from gppylib.operations.dump import dump_prefix
from gppylib import gplog
from gppylib.gparray import GpArray
from gppylib.commands.base import WorkerPool, Command, REMOTE
from gppylib.commands.unix import Scp
from gppylib.db import dbconn
from gppylib.db.dbconn import execSQL, execSQLForSingleton, UnexpectedRowsError
dump_prefix = ''
DUMP_DIR = 'db_dumps'

logger = gplog.get_default_logger()

def expand_partitions_and_populate_filter_file(dbname, partition_list, file_prefix):
    expanded_partitions = expand_partition_tables(dbname, partition_list)
    dump_partition_list = list(set(expanded_partitions + partition_list))
    return create_temp_file_from_list(dump_partition_list, file_prefix)

def populate_filter_tables(table, rows, non_partition_tables, partition_leaves):
    if not rows:
        non_partition_tables.append(table)
    else:
        for (schema_name, partition_leaf_name) in rows:
            partition_leaf = schema_name.strip() + '.' + partition_leaf_name.strip()
            partition_leaves.append(partition_leaf)
    return (non_partition_tables, partition_leaves)
                    
def expand_partition_tables(dbname, filter_tables):
    partition_leaves_sql = """
                           SELECT x.schemaname, x.partitiontablename 
                           FROM (
                                SELECT distinct schemaname, tablename, partitiontablename, partitionlevel 
                                FROM pg_partitions 
                                WHERE tablename = '%s' and schemaname = '%s'
                                ) as X, 
                           (SELECT schemaname, tablename maxtable, max(partitionlevel) maxlevel 
                            FROM pg_partitions 
                            group by (tablename, schemaname)
                           ) as Y
                           WHERE x.schemaname = y.schemaname and x.tablename = Y.maxtable and x.partitionlevel = Y.maxlevel;
"""

    if filter_tables is None:
        return

    non_partition_tables = []
    partition_leaves = []

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        for table in filter_tables:
            (schemaname, tablename) = table.split('.')
            partition_sql = partition_leaves_sql % (tablename, schemaname)
            curs = dbconn.execSQL(conn, partition_sql)
            rows = curs.fetchall()
            (non_partition_tables, partition_leaves) =  \
                    populate_filter_tables(table, rows, non_partition_tables, partition_leaves)
    return non_partition_tables + partition_leaves 

def create_temp_file_from_list(entries, prefix):

    if entries is None:
        return None 

    fd = tempfile.NamedTemporaryFile(mode='w', prefix=prefix, delete=False)
    for n in entries:
        fd.write(n.rstrip() + '\n')
    tmp_file_name = fd.name
    fd.close()

    verify_lines_in_file(tmp_file_name, entries)

    return tmp_file_name 

def create_temp_file_with_tables(table_list):
    return create_temp_file_from_list(table_list, 'table_list_')

def validate_timestamp(ts):
    if not ts:
        return False
    if len(ts) != 14:
        return False
    if ts.isdigit():
        return True
    else:
        return False

def check_successful_dump(dbname, report_file_contents):
    for line in report_file_contents:
        if line.strip() == 'gp_dump utility finished successfully.':
            return True
    return False

# raise exception for bad data
def convert_reportfilename_to_cdatabasefilename(report_file):
    (dirname, fname) = os.path.split(report_file)
    ts = fname[-18:-4]
    return "%s/%sgp_cdatabase_1_1_%s" % (dirname, dump_prefix, ts)

def check_cdatabase_exists(dbname, report_file):
    try:
        filename = convert_reportfilename_to_cdatabasefilename(report_file) 
    except Exception as e:
        return False

    cdatabase_contents = get_lines_from_file(filename)
    for line in cdatabase_contents:
        if 'CREATE DATABASE' in line:
            parts = line.split()
            if len(parts) < 3:
                continue
            if dbname == parts[2]:
                return True
    return False

def get_type_ts_from_report_file(dbname, report_file, backup_type):
    report_file_contents = get_lines_from_file(report_file)

    if not check_successful_dump(dbname, report_file_contents):
        return None

    if not check_cdatabase_exists(dbname, report_file):
        return None

    if check_backup_type(report_file_contents, backup_type):
        return get_timestamp_val(report_file_contents)

    return None

def get_full_ts_from_report_file(dbname, report_file):
    return get_type_ts_from_report_file(dbname, report_file, 'Full')

def get_incremental_ts_from_report_file(dbname, report_file):
    return get_type_ts_from_report_file(dbname, report_file, 'Incremental')

def get_timestamp_val(report_file_contents):
    for line in report_file_contents:
        if line.startswith('Timestamp Key'):
            ts = line.split(':')[-1].strip()
            try:
                int(ts)
                if len(ts) != 14:
                    raise Exception('Invalid timestamp value found in report_file')
            except ValueError as e:
                raise Exception('Invalid timestamp value found in report_file')

            return ts
    return None

def check_backup_type(report_file_contents, backup_type):
    for line in report_file_contents:
        if line.startswith('Backup Type'):
            if line.split(':')[-1].strip() == backup_type:
                return True
    return False

def get_lines_from_file(fname):
    content = ''
    with open(fname) as fd:
        content = fd.read()
    return content.splitlines()

def write_lines_to_file(filename, lines):
    with open(filename, 'w') as fp:
        for line in lines:
            fp.write("%s\n" % line.rstrip())

def verify_lines_in_file(fname, expected):
    lines = get_lines_from_file(fname)
    if lines != expected:
        raise Exception("After writing file '%s' contents not as expected, suspected IO error" % fname)

def execute_sql(query, master_port, dbname):
    dburl = dbconn.DbURL(port=master_port, dbname=dbname)
    conn = dbconn.connect(dburl)
    cursor = execSQL(conn, query)
    return cursor.fetchall()

def get_backup_directory(master_data_dir, backup_dir, timestamp):
    if backup_dir:
        use_dir = backup_dir
    elif master_data_dir:
        use_dir = master_data_dir
    else:
        raise Exception("Can not locate backup directory with existing parameters")

    if not timestamp:
        raise Exception("Can not locate backup directory without timestamp")

    if not validate_timestamp(timestamp):
        raise Exception ('Invalid timestamp: "%s"' % timestamp)

    return ("%s/%s/%s" % (use_dir, DUMP_DIR, timestamp[0:8]))

def generate_report_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s.rpt" % (use_dir, dump_prefix, timestamp)

def generate_increments_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_increments" % (use_dir, dump_prefix, timestamp)

def generate_pgstatlastoperation_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_last_operation" % (use_dir, dump_prefix, timestamp)

def generate_dirtytable_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_dirty_list" % (use_dir, dump_prefix, timestamp)

def generate_plan_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_restore_%s_plan" % (use_dir, dump_prefix, timestamp)

def generate_metadata_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_1_1_%s.gz" % (use_dir, dump_prefix, timestamp)

def generate_partition_list_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_table_list" % (use_dir, dump_prefix, timestamp)

def generate_ao_state_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_ao_state_file" % (use_dir, dump_prefix, timestamp)
 
def generate_co_state_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return "%s/%sgp_dump_%s_co_state_file" % (use_dir, dump_prefix, timestamp)

def generate_files_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return '%s/%sgp_dump_%s_regular_files' % (use_dir, dump_prefix, timestamp) 

def generate_pipes_filename(master_data_dir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_data_dir, backup_dir, timestamp)
    return '%s/%sgp_dump_%s_pipes' % (use_dir, dump_prefix, timestamp) 

def generate_master_config_filename(timestamp):
    return '%sgp_master_config_files_%s.tar' % (dump_prefix, timestamp)

def generate_segment_config_filename(segId, timestamp):
    return '%sgp_segment_config_files_0_%d_%s.tar' % (dump_prefix, segId, timestamp)

def generate_filter_filename(master_datadir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_datadir, backup_dir, timestamp)
    return '%s/%s%s_filter' % (use_dir, generate_dbdump_prefix(), timestamp)

def generate_global_prefix():
    return '%sgp_global_1_1_' % (dump_prefix)

def generate_master_dbdump_prefix():
    return '%sgp_dump_1_1_' % (dump_prefix)

def generate_master_status_prefix():
    return '%sgp_dump_status_1_1_' % (dump_prefix)

def generate_seg_dbdump_prefix():
    return '%sgp_dump_0_' % (dump_prefix)

def generate_seg_status_prefix():
    return '%sgp_dump_status_0_' % (dump_prefix)

def generate_dbdump_prefix():
    return '%sgp_dump_' % (dump_prefix)

def generate_createdb_prefix():
    return '%sgp_cdatabase_1_1_' % (dump_prefix)

def generate_createdb_filename(master_datadir, backup_dir, timestamp):
    use_dir = get_backup_directory(master_datadir, backup_dir, timestamp)
    return '%s/%s%s' % (use_dir, generate_createdb_prefix(), timestamp) 

def get_dump_dirs(dump_dir):
    dump_path = os.path.join(dump_dir, DUMP_DIR)

    if not os.path.isdir(dump_path):
        return []

    initial_list = os.listdir(dump_path)
    initial_list = fnmatch.filter(initial_list, '[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]')

    dirnames = []
    for d in initial_list:
        pth = os.path.join(dump_path, d)
        if os.path.isdir(pth):
            dirnames.append(pth)

    if len(dirnames) == 0:
        return []
    dirnames = sorted(dirnames, key=lambda x: int(os.path.basename(x)), reverse=True)
    return dirnames

def get_latest_report_timestamp(backup_dir):
    dump_dirs = get_dump_dirs(backup_dir)

    for d in dump_dirs:
        latest = get_latest_report_in_dir(d)
    
        if latest:
            return latest

    return None

def get_latest_report_in_dir(d):
    files = os.listdir(d)

    if len(files) == 0:
        return None

    dump_report_files = fnmatch.filter(files, '%sgp_dump_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].rpt' % dump_prefix)

    if len(dump_report_files) == 0:
        return None

    dump_report_files = sorted(dump_report_files, key=lambda x: int(x.split('_')[-1].split('.')[0]), reverse=True)
    return dump_report_files[0][-18:-4]

def get_timestamp_from_increments_filename(filename):
    fname = os.path.basename(filename)
    parts = fname.split('_')
    # Check for 4 underscores if there is no prefix, or more than 4 if there is a prefix
    if not ((not dump_prefix and len(parts) == 4) or (dump_prefix and len(parts) > 4)):
        raise Exception("Invalid increments file '%s' passed to get_timestamp_from_increments_filename" % filename)
    return parts[-2].strip()

def get_full_timestamp_for_incremental(dbname, backup_dir, incremental_timestamp):
    pattern = '%s/db_dumps/[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]/%sgp_dump_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_increments' % (backup_dir, dump_prefix)
    increments_files = glob.glob(pattern)

    for increments_file in increments_files:
        increment_ts = get_lines_from_file(increments_file)
        if incremental_timestamp in increment_ts:
            full_timestamp = get_timestamp_from_increments_filename(increments_file)
            return full_timestamp

    return None

# backup_dir will be either MDD or some other directory depending on call
def get_latest_full_dump_timestamp(dbname, backup_dir):
    if not backup_dir:
        raise Exception('Invalid None param to get_latest_full_dump_timestamp')

    dump_dirs = get_dump_dirs(backup_dir)

    for dump_dir in dump_dirs:
        files = sorted(os.listdir(dump_dir))

        if len(files) == 0:
            logger.warn('Dump directory %s is empty' % dump_dir)
            continue

        dump_report_files = fnmatch.filter(files, '%sgp_dump_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].rpt' % dump_prefix)

        if len(dump_report_files) == 0:
            logger.warn('No dump report files found in dump directory %s' % dump_dir)
            continue

        dump_report_files = sorted(dump_report_files, key=lambda x: int(x.split('_')[-1].split('.')[0]), reverse=True)
        for dump_report_file in dump_report_files:
            logger.debug('Checking for latest timestamp in report file %s' % os.path.join(dump_dir, dump_report_file))
            ts = get_full_ts_from_report_file(dbname, os.path.join(dump_dir, dump_report_file))
            logger.debug('Timestamp = %s' % ts)
            if ts is not None:
                return ts

    raise Exception('No full backup found for incremental')

def get_all_segment_addresses(master_port):
    gparray = GpArray.initFromCatalog(dbconn.DbURL(port=master_port), utility=True)
    addresses = [seg.getSegmentAddress() for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]
    return list(set(addresses))

def scp_file_to_hosts(host_list, filename, batch_default):
    pool = WorkerPool(numWorkers = min(len(host_list), batch_default))

    for hname in host_list:
        pool.addCommand(Scp('Copying table_filter_file to %s' % hname,
                        srcFile=filename,
                        dstFile=filename,
                        dstHost=hname))
    pool.join()
    pool.check_results()

def remove_file_from_segments(master_port, filename):
    hostlist = get_all_segment_addresses(master_port)
    for hname in hostlist:
        cmd = Command('Remove tmp files', 'rm -f %s' % filename, ctxt=REMOTE, remoteHost=hname)
        cmd.run(validateAfter=True)

def run_pool_command(host_list, cmd_str, batch_default, check_results=True):
    pool = WorkerPool(numWorkers = min(len(host_list), batch_default))

    for h in host_list:
        cmd = Command(h, cmd_str, ctxt=REMOTE, remoteHost = h)
        pool.addCommand(cmd)

    pool.join()
    if check_results:
        pool.check_results()

def check_funny_chars_in_tablenames(tablenames):
    for tablename in tablenames:
        if '\n' in tablename or ',' in tablename or ':' in tablename:
            raise Exception('Tablename has an invalid character "\\n", ":", "," : "%s"' % tablename) 
