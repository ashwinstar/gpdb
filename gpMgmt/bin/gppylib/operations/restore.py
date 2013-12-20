import os
import gzip
import getpass
import shutil
import socket
import time
from contextlib import closing
import sys

from gppylib import gplog
from gppylib.db import dbconn
from gppylib.db.dbconn import execSQL, execSQLForSingleton
from gppylib.gparray import GpArray
from gppylib.mainUtils import ExceptionNoStackTraceNeeded
from gppylib.commands.base import WorkerPool, Command, REMOTE
from gppylib.commands.gp import Psql
from gppylib.commands.unix import Scp
from gppylib.operations import Operation
from gppylib.operations.backup_utils import check_backup_type, generate_dirtytable_filename, generate_partition_list_filename, generate_plan_filename,\
                                            generate_metadata_filename, get_full_timestamp_for_incremental, get_lines_from_file, write_lines_to_file, \
                                            verify_lines_in_file, generate_increments_filename, generate_report_filename, create_temp_file_with_tables, \
                                            get_all_segment_addresses, scp_file_to_hosts, run_pool_command, execute_sql, expand_partition_tables, \
                                            generate_global_prefix, generate_master_dbdump_prefix, \
                                            generate_master_status_prefix, generate_seg_dbdump_prefix, generate_seg_status_prefix, \
                                            generate_dbdump_prefix, generate_createdb_filename, generate_createdb_prefix

from gppylib.operations.utils import RemoteOperation, ParallelOperation
from gppylib.operations.unix import CheckFile, CheckRemoteDir, MakeRemoteDir, CheckRemotePath

import gppylib.operations.backup_utils as backup_utils

"""
TODO: partial restore. In 4.x, dump will only occur on primaries. 
So, after a dump, dump files must be pushed to mirrors. (This is a task for gpcrondump.)
"""

""" TODO: centralize logging """
logger = gplog.get_default_logger()

WARN_MARK = '<<<<<'
DUMP_DIR = 'db_dumps'
POST_DATA_SUFFIX = '_post_data'

# TODO: use CLI-agnostic custom exceptions instead of ExceptionNoStackTraceNeeded

def update_ao_stat_func(conn, schema, table, counter, batch_size):
    qry = "SELECT * FROM gp_update_ao_master_stats('%s.%s')" % (schema, table)
    rows = execSQLForSingleton(conn, qry)
    if counter % batch_size == 0:
        conn.commit()

def update_ao_statistics(master_port, dbname):
    qry = """SELECT c.relname,n.nspname 
                      FROM pg_class c, pg_namespace n 
                      WHERE c.relnamespace=n.oid 
                      AND (c.relstorage='a' OR c.relstorage='c')"""

    conn = None
    counter = 1
    try:
        results  = execute_sql(qry, master_port, dbname)
        with dbconn.connect(dbconn.DbURL(port=master_port, dbname=dbname)) as conn:
            for (tbl, sch) in results:
                update_ao_stat_func(conn, sch, tbl, counter, batch_size=1000)
                counter = counter + 1
            conn.commit()
    except Exception as e:
        logger.info("Error updating ao statistics after restore")
        raise e

def get_restore_dir(data_dir, backup_dir):
    if backup_dir is not None:
        return backup_dir
    else:
        return data_dir

def get_restore_tables_from_table_file(table_file):
    if not os.path.isfile(table_file):
        raise Exception('Table file does not exist "%s"' % table_file)

    return get_lines_from_file(table_file)

def get_incremental_restore_timestamps(master_datadir, backup_dir, full_timestamp, inc_timestamp):
    inc_file = generate_increments_filename(master_datadir, backup_dir, full_timestamp)
    timestamps = get_lines_from_file(inc_file)
    sorted_timestamps = sorted(timestamps, key=lambda x: int(x), reverse=True)
    incremental_restore_timestamps = []
    try:
        incremental_restore_timestamps = sorted_timestamps[sorted_timestamps.index(inc_timestamp):]
    except ValueError as e:
        pass
    return incremental_restore_timestamps 

def get_partition_list(master_datadir, backup_dir, timestamp_key):
    partition_list_file = generate_partition_list_filename(master_datadir, backup_dir, timestamp_key)
    partition_list = get_lines_from_file(partition_list_file)
    partition_list = [(p.split('.')[0].strip(), p.split('.')[1].strip()) for p in partition_list] 
    return partition_list

def get_dirty_table_file_contents(master_datadir, backup_dir, timestamp_key):
    dirty_list_file = generate_dirtytable_filename(master_datadir, backup_dir, timestamp_key)
    return get_lines_from_file(dirty_list_file)

def create_plan_file_contents(master_datadir, backup_dir, table_set_from_metadata_file, incremental_restore_timestamps, full_timestamp):
    restore_set = {}
    for ts in incremental_restore_timestamps:
        restore_set[ts] = []
        dirty_tables = get_dirty_table_file_contents(master_datadir, backup_dir, ts)
        for dt in dirty_tables:
            if dt in table_set_from_metadata_file:
                table_set_from_metadata_file.remove(dt)
                restore_set[ts].append(dt)

    restore_set[full_timestamp] = []
    if len(table_set_from_metadata_file) != 0:
        for table in table_set_from_metadata_file:
            restore_set[full_timestamp].append(table)

    return restore_set

def write_to_plan_file(plan_file_contents, plan_file):
    if plan_file is None or not plan_file:
        raise Exception('Invalid plan file %s' % str(plan_file)) 

    sorted_plan_file_contents = sorted(plan_file_contents, key=lambda x: int(x), reverse=True)
  
    lines_to_write = [] 
    for ts in sorted_plan_file_contents:
        tables_str = ','.join(plan_file_contents[ts])
        lines_to_write.append(ts + ':' + tables_str)
  
    write_lines_to_file(plan_file, lines_to_write)
    verify_lines_in_file(plan_file, lines_to_write)

    return lines_to_write
    
def create_restore_plan(master_datadir, backup_dir, db_timestamp, dbname, ddboost):
    dump_tables = get_partition_list(master_datadir=master_datadir,
                                     backup_dir=backup_dir,
                                     timestamp_key=db_timestamp) 

    table_set_from_metadata_file = [schema + '.' + table for schema, table in dump_tables]

    full_timestamp = get_full_timestamp_for_incremental(dbname, get_restore_dir(master_datadir, backup_dir), db_timestamp)
    if not full_timestamp:
        raise Exception("Could not locate fullbackup associated with ts '%s'. Either increments file or fullback is missing." % db_timestamp)

    incremental_restore_timestamps = get_incremental_restore_timestamps(master_datadir, backup_dir, full_timestamp, db_timestamp)

    plan_file_contents = create_plan_file_contents(master_datadir, backup_dir, table_set_from_metadata_file, incremental_restore_timestamps, full_timestamp)

    plan_file = generate_plan_filename(master_datadir, backup_dir, db_timestamp)  

    write_to_plan_file(plan_file_contents, plan_file)

    return plan_file

def is_incremental_restore(master_datadir, backup_dir, timestamp):

    filename = generate_report_filename(master_datadir, backup_dir, timestamp)
    if not os.path.isfile(filename):
        logger.warn('Report file %s does not exist for restore timestamp %s' % (filename, timestamp))
        return False

    report_file_contents = get_lines_from_file(filename)
    if check_backup_type(report_file_contents, 'Incremental'):
        return True

    return False 

def is_begin_incremental_run(master_datadir, backup_dir, timestamp, noplan):
    if is_incremental_restore(master_datadir, backup_dir, timestamp) and not noplan:
        return True
    else:
        return False

def get_plan_file_contents(master_datadir, backup_dir, timestamp):
    plan_file_items = []
    plan_file = generate_plan_filename(master_datadir, backup_dir, timestamp)
    if not os.path.isfile(plan_file):
        raise Exception('Plan file %s does not exist' % plan_file) 
    plan_file_lines = get_lines_from_file(plan_file)
    if len(plan_file_lines) <= 0:
        raise Exception('Plan file %s has no contents' % plan_file)

    for line in plan_file_lines:
        if ':' not in line:
            raise Exception('Invalid plan file format')
        parts = line.split(':')
        plan_file_items.append((parts[0].strip(), parts[1].strip()))
    return plan_file_items

def get_restore_table_list(table_list, restore_tables):
    restore_list = []

    if restore_tables is None:
        restore_list = table_list
    else:
        for table in table_list:
            if table.strip() in restore_tables:
                restore_list.append(table.strip())

    if restore_list == []:  
        return None

    return create_temp_file_with_tables(restore_list)

def validate_restore_tables_list(plan_file_contents, restore_tables):
    if restore_tables is None:
        return

    table_set = set()
    comp_set = set()
    
    for ts, table in plan_file_contents:
        tables = table.split(',')
        for table in tables:
            table_set.add(table)

    invalid_tables = []
    for table in restore_tables:
        comp_set.add(table)
        if not comp_set.issubset(table_set): 
            invalid_tables.append(table)

    if invalid_tables != []:
        raise Exception('Invalid tables for -T option: The following tables were not found in plan file : "%s"' % (invalid_tables))

def restore_incremental_data_only(master_datadir, backup_dir, timestamp, restore_tables):
    restore_data = False
    plan_file_items = get_plan_file_contents(master_datadir, backup_dir, timestamp)
    table_file = None
    table_files = []

    validate_restore_tables_list(plan_file_items, restore_tables)
 
    for (ts, table_list) in plan_file_items:
        if table_list:
            restore_data = True
            table_file = get_restore_table_list(table_list.split(','), restore_tables)
            if table_file is None:
                continue
            cmd = _build_gpdbrestore_cmd_line(ts, table_file, backup_dir)
            logger.info('Invoking commandline: %s: ' % cmd)
            Command('Invoking gpdbrestore', cmd).run(validateAfter=True) 
            table_files.append(table_file)

    if not restore_data:
        raise Exception('There were no tables to restore. Check the plan file contents for restore timestamp %s' % timestamp)

    for table_file in table_files:
        if table_file: 
            os.remove(table_file)

    return True

def _build_gpdbrestore_cmd_line(ts, table_file, backup_dir):
    cmd = 'gpdbrestore -t %s --table-file %s -a -v --noplan --noanalyze --noaostats' % (ts, table_file) 
    if backup_dir is not None:
        cmd += " -u %s" % backup_dir
    if backup_utils.dump_prefix:
        cmd += " --prefix=%s" % backup_utils.dump_prefix.strip('_')
    return cmd

class RestoreDatabase(Operation):
    def __init__(self, restore_timestamp, no_analyze, drop_db, restore_global, master_datadir, backup_dir, 
                master_port, dump_dir, ddboost, no_plan, restore_tables, batch_default, no_ao_stats): 
        self.restore_timestamp = restore_timestamp
        self.no_analyze = no_analyze
        self.drop_db = drop_db
        self.restore_global = restore_global
        self.master_datadir = master_datadir
        self.backup_dir = backup_dir
        self.master_port = master_port
        self.dump_dir = dump_dir
        self.ddboost = ddboost
        self.no_plan = no_plan
        self.restore_tables = restore_tables
        self.batch_default = batch_default
        self.no_ao_stats = no_ao_stats

    def execute(self): 
        (restore_timestamp, restore_db, compress) = ValidateRestoreDatabase(restore_timestamp = self.restore_timestamp,
                                                                            master_datadir = self.master_datadir,
                                                                            backup_dir = self.backup_dir,
                                                                            master_port = self.master_port,
                                                                            dump_dir = self.dump_dir,
                                                                            ddboost = self.ddboost).run()

        if self.drop_db:
            self._multitry_createdb(restore_timestamp, 
                                    restore_db, 
                                    self.master_datadir, 
                                    self.backup_dir, 
                                    self.master_port)
        
        if self.restore_global:
            self._restore_global(restore_timestamp, self.master_datadir, self.backup_dir)
       
        if is_begin_incremental_run(self.master_datadir, self.backup_dir, self.restore_timestamp, self.no_plan):
            metadata_file = generate_metadata_filename(self.master_datadir, self.backup_dir, self.restore_timestamp)
            table_filter_file = self.create_filter_file() # returns None if nothing to filter
            restore_line = self._build_schema_only_restore_line(restore_timestamp, 
                                                                restore_db, 
                                                                compress, 
                                                                self.master_port, 
                                                                metadata_file,
                                                                table_filter_file)
            logger.info("Running metadata restore")
            logger.info("Invoking commandline: %s" % restore_line) 
            Command('Invoking gp_restore', restore_line).run(validateAfter=True)

            logger.info("Expanding parent partitions in table filter")
            self.restore_tables = expand_partition_tables(restore_db, self.restore_tables)
            logger.info("Running data restore")
            restore_incremental_data_only(self.master_datadir, 
                                          self.backup_dir, 
                                          self.restore_timestamp, 
                                          self.restore_tables)

            logger.info("Updating AO/CO statistics on master")
            update_ao_statistics(self.master_port, restore_db)
        else:
            table_filter_file = self.create_filter_file() # returns None if nothing to filter

            restore_line = self._build_restore_line(restore_timestamp, 
                                                    restore_db, compress, 
                                                    self.master_port, self.ddboost, 
                                                    self.no_plan, table_filter_file, self.no_ao_stats)
            logger.info('gp_restore commandline: %s: ' % restore_line)
            Command('Invoking gp_restore', restore_line).run(validateAfter=True)

            if table_filter_file: 
                self.remove_filter_file(table_filter_file)

        if not self.no_analyze:
            self._analyze(restore_db, self.master_port)

    def _analyze(self, restore_db, master_port):
        conn = None
        logger.info('Commencing analyze of %s database, please wait' % restore_db)
        try:
            dburl = dbconn.DbURL(port=master_port, dbname=restore_db)
            conn = dbconn.connect(dburl)
            execSQL(conn, 'analyze')
            conn.commit()
        except Exception, e:
            logger.warn('Issue with analyze of %s database' % restore_db)
        else:
            logger.info('Analyze of %s completed without error' % restore_db)
        finally:
            if conn is not None:
                conn.close()
    
    def create_filter_file(self):
        if self.restore_tables is None:
            return None

        table_filter_file = create_temp_file_with_tables(self.restore_tables)

        addresses = get_all_segment_addresses(self.master_port)

        scp_file_to_hosts(addresses, table_filter_file, self.batch_default)

        return table_filter_file

    def remove_filter_file(self, filename):
        addresses = get_all_segment_addresses(self.master_port)

        try:
            cmd = 'rm -f %s' % filename
            run_pool_command(addresses, cmd, self.batch_default, check_results=False)
        except Exception as e:
            logger.info("cleaning up filter table list file failed: %s" % e.__str__())

    def _restore_global(self, restore_timestamp, master_datadir, backup_dir): 
        logger.info('Commencing restore of global objects')
        global_file = os.path.join(get_restore_dir(master_datadir, backup_dir), self.dump_dir, restore_timestamp[0:8], "%s%s" % (generate_global_prefix(), restore_timestamp))
        if not os.path.exists(global_file):
            raise Exception('Unable to locate global file %s%s in dump set' % (generate_global_prefix(), restore_timestamp))
        Psql('Invoking global dump', filename=global_file).run(validateAfter=True)

    def _multitry_createdb(self, restore_timestamp, restore_db, master_datadir, backup_dir, master_port):
        no_of_trys = 600 
        for i in range(no_of_trys):
            try:
                self._process_createdb(restore_timestamp, restore_db, master_datadir, backup_dir, master_port)
            except ExceptionNoStackTraceNeeded, e:
                time.sleep(1)
            else:
                return
        raise ExceptionNoStackTraceNeeded('Failed to drop database %s' % restore_db)

    def _process_createdb(self, restore_timestamp, restore_db, master_datadir, backup_dir, master_port): 
        conn = None
        try:
            dburl = dbconn.DbURL(port=master_port)
            conn = dbconn.connect(dburl)
            count = execSQLForSingleton(conn, "select count(*) from pg_database where datname='%s';" % restore_db)

            if count == 1:
                logger.info("Dropping database %s" % restore_db)
                try:
                    cursor=conn.cursor()
                    cursor.execute("commit")                          # hack to move drop stmt out of implied transaction
                    cursor.execute("drop database %s" % restore_db)
                    cursor.close()
                except Exception, e:
                    logger.exception("Could not create database %s" % restore_db)
                    raise ExceptionNoStackTraceNeeded('Failed to drop database %s' % restore_db)
                else:
                    logger.info('Dropped database %s' % restore_db)
        finally:
            if conn is not None:
                conn.close()

        createdb_file = generate_createdb_filename(master_datadir, backup_dir, restore_timestamp)
        logger.info('Invoking sql file: %s' % createdb_file)
        Psql('Invoking schema dump', filename=createdb_file).run(validateAfter=True)

    def _build_restore_line(self, restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_stats): 
        user = getpass.getuser()
        hostname = socket.gethostname()    # TODO: can this just be localhost? bash was using `hostname`
        path = os.path.join(self.dump_dir, restore_timestamp[0:8])
        if self.backup_dir is not None:
            path = os.path.join(self.backup_dir, path)
        restore_line = "gp_restore -i -h %s -p %s -U %s --gp-d=%s --gp-i" % (hostname, master_port, user, path)
        if backup_utils.dump_prefix:
            logger.info("Adding --prefix")
            restore_line += " --prefix=%s" % backup_utils.dump_prefix
        if ddboost:
            path = os.path.join(self.master_datadir, path)
        restore_line += " --gp-k=%s --gp-r=%s --gp-l=p" % (restore_timestamp, path)
        if table_filter_file:
            restore_line += " --gp-f=%s" % table_filter_file
        if compress:
            restore_line += " --gp-c"
        restore_line += " -d %s" % restore_db
        if ddboost:
            restore_line += " --ddboost"
        if no_plan:
            restore_line += " -a"
        if no_stats:
            restore_line += " --gp-nostats"
        return restore_line

    def _build_schema_only_restore_line(self, restore_timestamp, restore_db, compress, master_port, metadata_filename, table_filter_file):
        user = getpass.getuser()
        hostname = socket.gethostname()    # TODO: can this just be localhost? bash was using `hostname`
        path = os.path.join(DUMP_DIR, restore_timestamp[0:8])
        if self.backup_dir is not None:
            path = os.path.join(self.backup_dir, path)
        restore_line = "gp_restore -i -h %s -p %s -U %s --gp-d=%s --gp-i" % (hostname, master_port, user, path)
        restore_line += " --gp-k=%s --gp-r=%s --gp-l=p -s %s" % (restore_timestamp, path, metadata_filename)
        if backup_utils.dump_prefix:
            logger.info("Adding --prefix")
            restore_line += " --prefix=%s" % backup_utils.dump_prefix
        if table_filter_file:
            restore_line += " --gp-f=%s" % table_filter_file
        if compress:
            restore_line += " --gp-c"
        restore_line += " -d %s" % restore_db
        return restore_line

class ValidateRestoreDatabase(Operation):
    """ TODO: add other checks. check for _process_createdb? """
    def __init__(self, restore_timestamp, master_datadir, backup_dir, master_port, dump_dir, ddboost):
        self.restore_timestamp = restore_timestamp
        self.master_datadir = master_datadir
        self.master_port = master_port
        self.dump_dir = dump_dir
        self.ddboost = ddboost
        self.backup_dir = backup_dir
    def execute(self):
        (restore_timestamp, restore_db, compress) = ValidateTimestamp(self.restore_timestamp, self.master_datadir, self.backup_dir, self.dump_dir, self.ddboost).run() 
        if not self.ddboost:
            ValidateSegments(restore_timestamp, compress, self.master_port, self.backup_dir, self.dump_dir).run()
        return (restore_timestamp, restore_db, compress)



class ValidateTimestamp(Operation):
    def __init__(self, candidate_timestamp, master_datadir, backup_dir, dump_dir, ddboost = False):
        self.master_datadir = master_datadir
        self.backup_dir = backup_dir
        self.candidate_timestamp = candidate_timestamp
        self.dump_dir = dump_dir
        self.ddboost = ddboost

    def execute(self):
        path = os.path.join(get_restore_dir(self.master_datadir, self.backup_dir), self.dump_dir, self.candidate_timestamp[0:8])
        createdb_file = generate_createdb_filename(self.master_datadir, self.backup_dir, self.candidate_timestamp)
        if not CheckFile(createdb_file).run():
            raise ExceptionNoStackTraceNeeded("Dump file %s does not exist on Master" % createdb_file)
        restore_db = GetDbName(createdb_file).run()
        if not self.ddboost:
            compressed_file = os.path.join(path, "%s%s.gz" % (generate_master_dbdump_prefix(), self.candidate_timestamp))
            compress = os.path.exists(compressed_file)
        else:
            compressed_file = os.path.join(path, "%s%s%s.gz" % (generate_master_dbdump_prefix(), self.candidate_timestamp, POST_DATA_SUFFIX))
            compress = CheckFile(compressed_file).run() 
        return (self.candidate_timestamp, restore_db, compress)



class ValidateSegments(Operation):
    def __init__(self, restore_timestamp, compress, master_port, backup_dir, dump_dir):
        self.restore_timestamp = restore_timestamp
        self.compress = compress
        self.master_port = master_port
        self.dump_dir = dump_dir
        self.backup_dir = backup_dir
    def execute(self): 
        """ TODO: Improve with grouping by host and ParallelOperation dispatch. """
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)   
        primaries = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]
        dump_count = 0
        for seg in primaries:
            if seg.isSegmentDown():
                """ Why must every Segment function have the word Segment in it ?! """
                raise ExceptionNoStackTraceNeeded("Host %s dir %s dbid %d marked as invalid" % (seg.getSegmentHostName(), seg.getSegmentDataDirectory(), seg.getSegmentDbId()))

            path = os.path.join(get_restore_dir(seg.getSegmentDataDirectory(), self.backup_dir), self.dump_dir, self.restore_timestamp[0:8])
            host = seg.getSegmentHostName()
            path = os.path.join(path, "%s0_%d_%s" % (generate_dbdump_prefix(), seg.getSegmentDbId(), self.restore_timestamp))
            if self.compress:
                path += ".gz"
            exists = CheckRemotePath(path, host).run()
            if not exists:
                raise ExceptionNoStackTraceNeeded("No dump file on %s at %s" % (seg.getSegmentHostName(), path))

def validate_tablenames(table_list):
    for restore_table in table_list:
        if '.' not in restore_table:
            raise Exception("No schema name supplied for %s, removing from list of tables to restore" % restore_table)

class ValidateRestoreTables(Operation):
    def __init__(self, restore_tables, restore_db, master_port): 
        self.restore_tables = restore_tables
        self.restore_db = restore_db
        self.master_port = master_port
    def execute(self): 
        existing_tables = []
        table_counts = []
        conn = None
        try:
            dburl = dbconn.DbURL(port=self.master_port, dbname=self.restore_db)
            conn = dbconn.connect(dburl)
            for restore_table in self.restore_tables:
                schema, table = restore_table.split('.')
                count = execSQLForSingleton(conn, "select count(*) from pg_class, pg_namespace where pg_class.relname = '%s' and pg_class.relnamespace = pg_namespace.oid and pg_namespace.nspname = '%s'" % (table, schema))
                if count == 0:
                    logger.warn("Table %s does not exist in database %s, removing from list of tables to restore" % (table, self.restore_db))
                    continue

                count = execSQLForSingleton(conn, "select count(*) from %s.%s" % (schema, table))
                if count > 0:
                    logger.warn('Table %s has %d records %s' % (restore_table, count, WARN_MARK))
                existing_tables.append(restore_table)
                table_counts.append((restore_table, count))
        finally:
            if conn is not None:
                conn.close()

        if len(existing_tables) == 0:
            raise ExceptionNoStackTraceNeeded("Have no tables to restore")
        logger.info("Have %d tables to restore, will continue" % len(existing_tables))

        return (existing_tables, table_counts)


class CopyPostData(Operation):
    ''' Copy _post_data when using fake timestamp. 
        The same operation can be done with/without ddboost, because
        the _post_data file is always kept on the master, not on the dd server '''
    def __init__(self, restore_timestamp, fake_timestamp, compress, master_datadir, dump_dir, backup_dir):
        self.restore_timestamp = restore_timestamp
        self.fake_timestamp = fake_timestamp
        self.compress = compress
        self.master_datadir = master_datadir
        self.dump_dir = dump_dir
        self.backup_dir = backup_dir
    def execute(self):
         # Build master _post_data file:
        restore_dir = get_restore_dir(self.master_datadir, self.backup_dir)
        real_post_data = os.path.join(restore_dir, self.dump_dir, self.restore_timestamp[0:8], "%s%s%s" % (generate_master_dbdump_prefix(), self.restore_timestamp, POST_DATA_SUFFIX))
        fake_post_data = os.path.join(restore_dir, self.dump_dir, self.fake_timestamp[0:8], "%s%s%s" % (generate_master_dbdump_prefix(), self.fake_timestamp, POST_DATA_SUFFIX))
        if (self.compress):
            real_post_data = real_post_data + ".gz"
            fake_post_data = fake_post_data + ".gz"
        shutil.copy(real_post_data, fake_post_data)
        

class GetDbName(Operation):
    def __init__(self, createdb_file):
        self.createdb_file = createdb_file
    def execute(self):
        f = open(self.createdb_file, 'r')
        # assumption: 'CREATE DATABASE' line will reside within the first 50 lines of the gp_cdatabase_1_1_* file
        for line_no in range(0, 50):
            line = f.readline()
            if not line:
                break
            if line.startswith("CREATE DATABASE"):
                restore_db = line.split()[2]
                return restore_db
        else:
            raise GetDbName.DbNameGiveUp()
        raise GetDbName.DbNameNotFound()

    class DbNameNotFound(Exception): pass
    class DbNameGiveUp(Exception): pass



class RecoverRemoteDumps(Operation):
    def __init__(self, host, path, restore_timestamp, compress, restore_global, batch_default, master_datadir, master_port):
        self.host = host
        self.path = path
        self.restore_timestamp = restore_timestamp
        self.compress = compress
        self.restore_global = restore_global
        self.batch_default = batch_default
        self.master_datadir = master_datadir
        self.master_port = master_port
    def execute(self): 
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)
        from_host, from_path = self.host, self.path
        logger.info("Commencing remote database dump file recovery process, please wait...")
        segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True) or seg.isSegmentMaster()]
        pool = WorkerPool(numWorkers = min(len(segs), self.batch_default))
        for seg in segs:
            if seg.isSegmentMaster():
                file = '%s%s' % (generate_master_dbdump_prefix(), self.restore_timestamp)
            else:
                file = '%s0_%d_%s' % (generate_dbdump_prefix(), seg.getSegmentDbId(), self.restore_timestamp)
            if self.compress:
                file += '.gz'

            to_host = seg.getSegmentHostName()
            to_path = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, self.restore_timestamp[0:8])
            if not CheckRemoteDir(to_path, to_host).run():
                logger.info('Creating directory %s on %s' % (to_path, to_host))
                try:
                    MakeRemoteDir(to_path, to_host).run()
                except OSError, e:
                    raise ExceptionNoStackTraceNeeded("Failed to create directory %s on %s" % (to_path, to_host))
   
            logger.info("Commencing remote copy from %s to %s:%s" % (from_host, to_host, to_path))
            pool.addCommand(Scp('Copying dump for seg %d' % seg.getSegmentDbId(),
                            srcFile=os.path.join(from_path, file),
                            dstFile=os.path.join(to_path, file),
                            srcHost=from_host,
                            dstHost=to_host))
        createdb_file = '%s%s' % (generate_createdb_prefix(), self.restore_timestamp)
        to_path = os.path.join(self.master_datadir, DUMP_DIR, self.restore_timestamp[0:8])
        pool.addCommand(Scp('Copying schema dump',
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, createdb_file),
                            dstFile=generate_createdb_filename(self.master_datadir, None, self.restore_timestamp)))

        report_file = "%s%s.rpt" % (generate_dbdump_prefix(), self.restore_timestamp)
        pool.addCommand(Scp('Copying report file',
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, report_file),
                            dstFile=os.path.join(to_path, report_file)))

        post_data_file = "%s%s%s" % (generate_master_dbdump_prefix(), self.restore_timestamp, POST_DATA_SUFFIX)
        if self.compress:
            post_data_file += ".gz"
        pool.addCommand(Scp('Copying post data schema dump',
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, post_data_file),
                            dstFile=os.path.join(to_path, post_data_file)))
        if self.restore_global:
            global_file = "%s%s" % (generate_global_prefix(), self.restore_timestamp)
            pool.addCommand(Scp("Copying global dump",
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, global_file),
                            dstFile=os.path.join(to_path, global_file)))
        pool.join()
        pool.check_results()



class GetDumpTables(Operation):
    def __init__(self, restore_timestamp, master_datadir, backup_dir, dump_dir, ddboost): 
        self.master_datadir = master_datadir
        self.restore_timestamp = restore_timestamp
        self.dump_dir = dump_dir
        self.ddboost = ddboost
        self.backup_dir = backup_dir

    def execute(self): 
        (restore_timestamp, restore_db, compress) = ValidateTimestamp(master_datadir = self.master_datadir,
                                                                      backup_dir = self.backup_dir, 
                                                                      candidate_timestamp = self.restore_timestamp,
                                                                      dump_dir = self.dump_dir,
                                                                      ddboost = self.ddboost).run()
        dump_file = os.path.join(get_restore_dir(self.master_datadir, self.backup_dir), self.dump_dir, restore_timestamp[0:8], "%s%s" % (generate_master_dbdump_prefix(), restore_timestamp))
        if compress:
            dump_file += '.gz'

        if self.ddboost:
            from_file = os.path.join(self.dump_dir, restore_timestamp[0:8], "%s%s" % (generate_master_dbdump_prefix(), restore_timestamp))
            if compress:
                from_file += '.gz'
            ret = []
            schema = ''
            owner = '' 
            if compress:
                cmd = Command('DDBoost copy of master dump file',
                          'gpddboost --readFile --from-file=%s | gunzip | grep -e "SET search_path = " -e "-- Data for Name: " -e "COPY "' 
                            % (from_file))
            else:
                cmd = Command('DDBoost copy of master dump file',
                          'gpddboost --readFile --from-file=%s | grep -e "SET search_path = " -e "-- Data for Name: " -e "COPY "' 
                            % (from_file))

            # TODO: The code below is duplicated from the code for local restore
            #       We need to refactor this. Probably use the same String IO interfaces 
            #       to extract lines in both the cases.
            cmd.run(validateAfter = True)
            line_list = cmd.get_results().stdout.splitlines()
            for line in line_list:
                if line.startswith("SET search_path = "):
                    line = line[len("SET search_path = ") : ]
                    if ", pg_catalog;" in line:
                        schema = line[ : line.index(", pg_catalog;")]
                    else:
                        schema = "pg_catalog"
                elif line.startswith("-- Data for Name: "):
                    owner = line[line.index("; Owner: ") + 9 : ].rstrip()
                elif line.startswith("COPY "):
                    table = line[5:]
                    if table.rstrip().endswith(") FROM stdin;"):
                        if table.startswith("\""):
                            table = table[: table.index("\" (") + 1]
                        else:
                            table = table[: table.index(" (")]
                    else:
                        table = table[: table.index(" FROM stdin;")]
                    table = table.rstrip()  
                    ret.append( (schema, table, owner) )
            return ret
        else: 
            f = None
            schema = ''
            owner = ''
            ret = []
            try:
                if compress:
                    f = gzip.open(dump_file, 'r')
                else:
                    f = open(dump_file, 'r')

                while True:
                    line = f.readline()
                    if not line:
                        break
                    if line.startswith("SET search_path = "):
                        line = line[len("SET search_path = ") : ]
                        if ", pg_catalog;" in line:
                            schema = line[ : line.index(", pg_catalog;")]
                        else:
                            schema = "pg_catalog"
                    elif line.startswith("-- Data for Name: "):
                        owner = line[line.index("; Owner: ") + 9 : ].rstrip()
                    elif line.startswith("COPY "):
                        table = line[5:]
                        if table.rstrip().endswith(") FROM stdin;"):
                            if table.startswith("\""):
                                table = table[: table.index("\" (") + 1]
                            else:
                                table = table[: table.index(" (")]
                        else:
                            table = table[: table.index(" FROM stdin;")]
                        table = table.rstrip()
                        ret.append( (schema, table, owner) )
            finally:
                if f is not None:
                    f.close()
            return ret
