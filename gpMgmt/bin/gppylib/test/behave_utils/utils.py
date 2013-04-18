#!/usr/bin/env python
import re, os, time, filecmp

from gppylib.commands.gp import GpStart, chk_local_db_running
from gppylib.commands.base import Command, ExecutionError, REMOTE
from gppylib.db import dbconn
from gppylib.gparray import GpArray, MODE_SYNCHRONIZED

PARTITION_START_DATE = '2010-01-01'
PARTITION_END_DATE = '2013-01-01'

GET_APPENDONLY_DATA_TABLE_INFO_SQL ="""SELECT ALL_DATA_TABLES.oid, ALL_DATA_TABLES.schemaname, ALL_DATA_TABLES.tablename, OUTER_PG_CLASS.relname as tupletable FROM(
SELECT ALLTABLES.oid, ALLTABLES.schemaname, ALLTABLES.tablename FROM 
    (SELECT c.oid, n.nspname AS schemaname, c.relname AS tablename FROM pg_class c, pg_namespace n 
    WHERE n.oid = c.relnamespace) as ALLTABLES,
    (SELECT n.nspname AS schemaname, c.relname AS tablename
    FROM pg_class c LEFT JOIN pg_namespace n ON n.oid = c.relnamespace
    LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace
    WHERE c.relkind = 'r'::"char" AND c.oid > 16384 AND (c.relnamespace > 16384 or n.nspname = 'public') 
    EXCEPT
    ((SELECT x.schemaname, x.partitiontablename FROM 
    (SELECT distinct schemaname, tablename, partitiontablename, partitionlevel FROM pg_partitions) as X,
    (SELECT schemaname, tablename maxtable, max(partitionlevel) maxlevel FROM pg_partitions group by (tablename, schemaname))
 as Y    
    WHERE x.schemaname = y.schemaname and x.tablename = Y.maxtable and x.partitionlevel != Y.maxlevel) 
    UNION (SELECT distinct schemaname, tablename FROM pg_partitions))) as DATATABLES 
WHERE ALLTABLES.schemaname = DATATABLES.schemaname and ALLTABLES.tablename = DATATABLES.tablename AND ALLTABLES.oid not in (select reloid from pg_exttable)
) as ALL_DATA_TABLES, pg_appendonly, pg_class OUTER_PG_CLASS
    WHERE ALL_DATA_TABLES.oid = pg_appendonly.relid 
    AND OUTER_PG_CLASS.oid = pg_appendonly.segrelid
"""

GET_ALL_AO_DATATABLES_SQL = """
    %s AND pg_appendonly.columnstore = 'f'
""" % GET_APPENDONLY_DATA_TABLE_INFO_SQL

GET_ALL_CO_DATATABLES_SQL = """
    %s AND pg_appendonly.columnstore = 't'
""" % GET_APPENDONLY_DATA_TABLE_INFO_SQL

master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
if master_data_dir is None:
    raise Exception('MASTER_DATA_DIRECTORY is not set')

def execute_sql(dbname, sql):
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, sql)
        conn.commit()

def has_exception(context):
    if not hasattr(context, 'exception'):
        return False
    if context.exception:
        return True
    else:
        return False

def run_command(context, command):
    context.exception = None
    cmd = Command(name='run %s' % command, cmdStr='%s' % command)
    try:
        cmd.run(validateAfter=True)
    except ExecutionError, e:
        context.exception = e

    result = cmd.get_results()
    context.ret_code = result.rc 
    context.stdout_message = result.stdout
    context.error_message = result.stderr 

def run_gpcommand(context, command):
    context.exception = None
    cmd = Command(name='run %s' % command, cmdStr='$GPHOME/bin/%s' % command)
    try:
        cmd.run(validateAfter=True)
    except ExecutionError, e:
        context.exception = e

    result = cmd.get_results()
    context.ret_code = result.rc 
    context.stdout_message = result.stdout
    context.error_message = result.stderr 

def check_stdout_msg(context, msg):
    if msg not in context.stdout_message:
        err_str = "Expected stdout string '%s' and found: '%s'" % (msg, context.stdout_message)
        raise Exception(err_str)

def check_string_not_present_stdout(context, msg):
    if msg in context.stdout_message:
        err_str = "Did not expect stdout string '%s' but found: '%s'" % (msg, context.stdout_message)
        raise Exception(err_str)

def check_err_msg(context, err_msg):
    if not hasattr(context, 'exception'):
        raise Exception('An exception was not raised and it was expected')
    if err_msg not in context.error_message:
        err_str = "Expected error string '%s' and found: '%s'" % (err_msg, context.error_message)
        raise Exception(err_str)

def check_return_code(context, ret_code):
    if context.ret_code != int(ret_code):
        emsg = ""
        if context.error_message:
            emsg += context.error_message
        raise Exception("expected return code '%s' does not equal acutal return code '%s' %s" % (ret_code, context.ret_code, emsg))

def check_database_is_running(context):
    if not 'PGPORT' in os.environ:
        raise Exception('PGPORT should be set')

    pgport = int(os.environ['PGPORT'])

    running_status = chk_local_db_running(master_data_dir, pgport) 
    gpdb_running = running_status[0] and running_status[1] and running_status[2] and running_status[3]
    
    return gpdb_running

def start_database_if_not_started(context):
    if not check_database_is_running(context):
        start_database(context)

def start_database(context):
    run_gpcommand(context, 'gpstart -a')
    if context.exception:
        raise context.exception

def stop_database_if_started(context):
    if check_database_is_running(context):
        stop_database(context)

def stop_database(context):
    run_gpcommand(context, 'gpstop -M fast -a')
    if context.exception:
        raise context.exception

def getRows(dbname, exec_sql):
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        curs = dbconn.execSQL(conn, exec_sql)
        results = curs.fetchall()
    return results

def check_db_exists(dbname):
    LIST_DATABASE_SQL = 'select datname from pg_database'  
  
    results = []  
    with dbconn.connect(dbconn.DbURL()) as conn:
        curs = dbconn.execSQL(conn, LIST_DATABASE_SQL)
        results = curs.fetchall()

    for result in results:
        if result[0] == dbname:
            return True
 
    return False

def create_database_if_not_exists(context, dbname):
    if not check_db_exists(dbname):
        create_database(context, dbname)

def create_database(context, dbname):

    LOOPS = 10
    for i in range(LOOPS):
        context.exception = None

        run_gpcommand(context, 'createdb %s' % dbname)

        if context.exception:
            time.sleep(1)
            continue

        if check_db_exists(dbname):
            return

        time.sleep(1)

    if context.exception:
        raise context.exception

    raise Exception("createdb for '%s' failed after %d attempts" % (dbname, LOOPS))

def clear_all_saved_data_verify_files(context):
    current_dir = os.getcwd()
    data_dir = os.path.join(current_dir, './gppylib/test/data')
    cmd = 'rm %s/*' % data_dir
    run_command(context, cmd)

def get_table_data_to_file(filename, tablename, dbname):
    current_dir = os.getcwd()
    filename = os.path.join(current_dir, './gppylib/test/data', filename)
    data_sql = "COPY (select gp_segment_id, * from gp_dist_random('%s') order by column1) TO '%s'" %(tablename.strip(), filename)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, data_sql)
        conn.commit()

def diff_backup_restore_data(context, backup_file, restore_file):
    if not filecmp.cmp(backup_file, restore_file):
        raise Exception('%s and %s do not match' % (backup_file, restore_file))
    
def validate_restore_data(context, tablename, dbname):
    filename = tablename.strip() + "_restore"
    get_table_data_to_file(filename, tablename, dbname)
    current_dir = os.getcwd()
    backup_file = os.path.join(current_dir, './gppylib/test/data', tablename.strip() + "_backup")
    restore_file = os.path.join(current_dir, './gppylib/test/data', tablename.strip() + "_restore")
    diff_backup_restore_data(context, backup_file, restore_file)


def validate_db_data(context, dbname, expected_table_count):
    tbls = get_table_names(dbname)
    if len(tbls) != expected_table_count:
        raise Exception("db %s does not have expected number of tables %d != %d" % (dbname, expected_table_count, len(tbls)))
    for t in tbls:
        name = "%s.%s" % (t[0], t[1])
        validate_restore_data(context, name, dbname)

def get_segment_hostnames(context, dbname):
    sql = "select distinct(hostname) from gp_segment_configuration where content != -1;"
    return getRows(dbname, sql)     

def backup_db_data(context, dbname):
    tbls = get_table_names(dbname)
    for t in tbls:
        nm = "%s.%s" % (t[0], t[1])
        backup_data(context, nm, dbname)

def backup_data(context, tablename, dbname):
    filename = tablename + "_backup"
    get_table_data_to_file(filename, tablename, dbname)

def check_partition_table_exists(context, dbname, schemaname, table_name, table_type=None, part_level=1, part_number=1):
    partitions = get_partition_names(schemaname, table_name, dbname, part_level, part_number)
    if not partitions:
        return False
    return check_table_exists(context, dbname, partitions[0][0].strip(), table_type) 

def check_table_exists(context, dbname, table_name, table_type=None):
    SQL = """
            select oid::regclass, relkind, relstorage, reloptions \
            from pg_class \
            where oid = '%s'::regclass; \
          """ % table_name

    table_row = None 
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        try:
            table_row = dbconn.execSQLForSingletonRow(conn, SQL) 
        except Exception as e:
            context.exception = e
            return False

    if table_type is None:
        return True

    if table_row[2] == 'a':
        original_table_type = 'ao'
    elif table_row[2] == 'c':
        original_table_type = 'co'
    elif table_row[2] == 'h':
        original_table_type = 'heap'
    elif table_row[2] == 'x':
        original_table_type = 'external'
    else:
        raise Exception('Unknown table type %s' % table_row[2]) 

    if original_table_type != table_type.strip():
        return False

    return True

def drop_external_table_if_exists(context, table_name, dbname):
    if check_table_exists(context, table_name=table_name, dbname=dbname, table_type='external'):
        drop_external_table(context, table_name=table_name, dbname=dbname)

def drop_table_if_exists(context, table_name, dbname):
    if check_table_exists(context, table_name=table_name, dbname=dbname):
        drop_table(context, table_name=table_name, dbname=dbname)

def drop_external_table(context, table_name, dbname):
    SQL = 'drop external table %s' % table_name
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, SQL)
        conn.commit()

    if check_table_exists(context, table_name=table_name, dbname=dbname, table_type='external'):
        raise Exception('Unable to successfully drop the table %s' % table_name) 

def drop_table(context, table_name, dbname):
    SQL = 'drop table %s' % table_name
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, SQL)
        conn.commit()

    if check_table_exists(context, table_name=table_name, dbname=dbname):
        raise Exception('Unable to successfully drop the table %s' % table_name) 
 
def check_schema_exists(context, schema_name, dbname):
    schema_check_sql = "select * from pg_namespace where nspname='%s';" % schema_name
    if len(getRows(dbname, schema_check_sql)) < 1:
        return False
    return True

def drop_schema_if_exists(context, schema_name, dbname):
    if check_schema_exists(context, schema_name, dbname):
        drop_schema(context, schema_name, dbname)

def drop_schema(context, schema_name, dbname):
    SQL = 'drop schema %s cascade' % schema_name
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, SQL)
        conn.commit()
    if check_schema_exists(context, schema_name, dbname):
        raise Exception('Unable to successfully drop the schema %s' % schema_name) 
 
def validate_table_data_on_segments(context, tablename, dbname):

    seg_data_sql = "select gp_segment_id, count(*) from gp_dist_random('%s') group by gp_segment_id;" % tablename
    
    rows = getRows(dbname, seg_data_sql)     
    for row in rows:
        if row[1] == '0' :
           raise Exception('Data not present in segment %s' % row[0]) 
   
def get_table_names(dbname):
    sql = """
            SELECT n.nspname AS schemaname, c.relname AS tablename\
            FROM pg_class c\
            LEFT JOIN pg_namespace n ON n.oid = c.relnamespace\
            LEFT JOIN pg_tablespace t ON t.oid = c.reltablespace\
            WHERE c.relkind = 'r'::"char" AND c.oid > 16384 AND (c.relnamespace > 16384 or n.nspname = 'public')
          """

    return getRows(dbname, sql)

def get_partition_tablenames(tablename, dbname, part_level = 1):
 
    child_part_sql = "select partitiontablename from pg_partitions where tablename='%s' and partitionlevel=%s;" % (tablename, part_level)
    rows = getRows(dbname, child_part_sql)
    return rows
     
def get_partition_names(schemaname, tablename, dbname, part_level, part_number):
    part_num_sql = "select partitiontablename from pg_partitions where schemaname='%s' and tablename='%s' and partitionlevel=%s and partitionposition=%s;" % (schemaname, tablename, part_level, part_number)
    rows = getRows(dbname, part_num_sql)
    return rows
     
def validate_part_table_data_on_segments(context, tablename, part_level, dbname):
    
    rows = get_partition_tablenames(tablename, dbname, part_level)
    for part_tablename in rows :
        seg_data_sql = "select gp_segment_id, count(*) from gp_dist_random('%s') group by gp_segment_id;" % part_tablename[0]
        rows = getRows(dbname, seg_data_sql)        
        for row in rows:
            if row[1] == '0' :
               raise Exception('Data not present in segment %s' % row[0]) 

def validate_mixed_partition_storage_types(context, tablename, dbname):
    partition_names = get_partition_tablenames(tablename, dbname, part_level = 1)
    for position, partname in enumerate(partition_names):
        if position in(0, 2, 5, 7): 
            storage_type = 'c' 
        elif position in(1, 3, 6, 8): 
            storage_type = 'a' 
        else:
            storage_type = 'h' 
        for part in partname:
            validate_storage_type(context, part, storage_type, dbname)
    
def validate_storage_type(context, partname, storage_type, dbname):
    
    storage_type_sql = "select oid::regclass, relstorage from pg_class where oid = '%s'::regclass;" % (partname)
    rows = getRows(dbname, storage_type_sql)
    for row in rows:
        if row[1].strip() != storage_type.strip():
            raise Exception("The storage type of the partition %s is not as expected %s "% (row[1], storage_type))

def create_mixed_storage_partition(context, tablename, dbname):
    
    table_definition = 'Column1 int, Column2 varchar(20), Column3 date'
    create_table_str = "Create table %s (%s) Distributed randomly \
                        Partition by list(Column2)  \
                        Subpartition by range(Column3) Subpartition Template ( \
                        subpartition s_1  start(date '2010-01-01') end(date '2010-05-01') with (appendonly=true, orientation=column, compresstype=quicklz, compresslevel=1), \
                        subpartition s_2  start(date '2010-05-01') end(date '2010-10-01') with (appendonly=true, orientation=row, compresstype=zlib, compresslevel=1), \
                        subpartition s_3  start(date '2010-10-01') end(date '2011-05-01') with (appendonly=true, orientation=column), \
                        subpartition s_4  start(date '2011-05-01') end(date '2011-10-01') with (appendonly=true, orientation=row), \
                        subpartition s_5  start(date '2011-10-01') end(date '2013-01-01') ) \
                        (partition p1 values('backup') , partition p2 values('restore')) \
                        ;" % (tablename, table_definition)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, create_table_str)
        conn.commit()

    populate_partition(tablename, '2010-01-01', dbname, 0)

def modify_partition_data(context, tablename, dbname, partitionnum):

    # ONLY works for partition 1 to 3
    if partitionnum == 1:
        year = '2010'
    elif partitionnum == 2:
        year = '2011'
    elif partitionnum == 3:
        year = '2012'
    else:
        raise Exception("BAD PARAM to modify_partition_data %s" % partitionnum)

    sql = "insert into %s values(90, 'backup', '%s-12-30')" % (tablename, year)
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        for i in range(10):
            dbconn.execSQL(conn, sql)
        conn.commit()

def modify_data(context, tablename, dbname):

    sql = 'insert into %s select * from %s' % (tablename, tablename)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, sql)
        conn.commit()


def create_partition(context, tablename, storage_type, dbname, compression_type=None, partition=True):

    interval = '1 year'

    table_definition = 'Column1 int, Column2 varchar(20), Column3 date'
    create_table_str = "Create table " + tablename + "(" + table_definition + ")"
    storage_type_dict = {'ao':'row', 'co':'column'}

    part_table = " Distributed Randomly Partition by list(Column2) \
                    Subpartition by range(Column3) Subpartition Template \
                    (start (date '%s') end (date '%s') every (interval '%s')) \
                    (Partition p1 values('backup') , Partition p2 values('restore')) " \
                    %(PARTITION_START_DATE, PARTITION_END_DATE, interval)

    if storage_type == "heap":
        create_table_str = create_table_str
        if partition:
           create_table_str = create_table_str + part_table

    elif storage_type == "ao" or storage_type == "co":
        create_table_str = create_table_str + " WITH(appendonly = true, orientation = %s) " % storage_type_dict[storage_type]
        if compression_type is not None:
            create_table_str = create_table_str[:-2] + ", compresstype = " + compression_type + ") "
        if partition:
            create_table_str = create_table_str + part_table

    create_table_str = create_table_str + ";" 

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, create_table_str)
        conn.commit()

    populate_partition(tablename, PARTITION_START_DATE, dbname, 0)

# same data size as populate partition, but different values
def populate_partition_diff_data_same_eof(tablename, dbname):
    populate_partition(tablename, PARTITION_START_DATE, dbname, 1)

def populate_partition_same_data(tablename, dbname):
    populate_partition(tablename, PARTITION_START_DATE, dbname, 0)

def populate_partition(tablename, start_date, dbname, data_offset):

    insert_sql_str = "insert into %s select i+%d, 'backup', i + date '%s' from generate_series(0,1094) as i" %(tablename, data_offset, start_date)
    insert_sql_str += "; insert into %s select i+%d, 'restore', i + date '%s' from generate_series(0,1094) as i" %(tablename, data_offset, start_date)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, insert_sql_str)
        conn.commit()

def create_indexes(context, table_name, indexname, dbname):

    btree_index_sql = "create index btree_%s on %s using btree(column1);" % (indexname, table_name)
    bitmap_index_sql = "create index bitmap_%s on %s using bitmap(column3);" % (indexname, table_name)
    index_sql = btree_index_sql + bitmap_index_sql
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, index_sql)
        conn.commit()
    validate_index(context, table_name, dbname)

def validate_index(context, table_name, dbname):
    index_sql = "select count(indexrelid::regclass) from pg_index, pg_class where indrelid = '%s'::regclass group by indexrelid;" % table_name
    rows = getRows(dbname, index_sql)
    if len(rows) != 2:
        raise Exception('Index creation was not successful. Expected 2 rows does not match %d rows' % result)
   
def create_schema(context, schema_name, dbname):
    if not check_schema_exists(context, schema_name, dbname):
        schema_sql = "create schema %s" % schema_name
        with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
            dbconn.execSQL(conn, schema_sql)
            conn.commit()
     
def create_int_table(context, table_name, table_type='heap', dbname='testdb'):
    CREATE_TABLE_SQL = None
    NROW = 1000

    table_type = table_type.upper() 
    if table_type == 'AO':
        CREATE_TABLE_SQL = 'create table %s WITH(APPENDONLY=TRUE) as select generate_series(1,%d) as c1' % (table_name, NROW)
    elif table_type == 'CO':
        CREATE_TABLE_SQL = 'create table %s WITH(APPENDONLY=TRUE, orientation=column) as select generate_series(1, %d) as c1' % (table_name, NROW)
    elif table_type == 'HEAP':
        CREATE_TABLE_SQL = 'create table %s as select generate_series(1, %d) as c1' % (table_name, NROW)

    if CREATE_TABLE_SQL is None:
        raise Exception('Invalid table type specified')
  
    SELECT_TABLE_SQL = 'select count(*) from %s' % table_name 
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, CREATE_TABLE_SQL)
        conn.commit()
         
        result = dbconn.execSQLForSingleton(conn, SELECT_TABLE_SQL)
        if result != NROW:
            raise Exception('Integer table creation was not successful. Expected %d does not match %d' %(NROW, result))
       
def drop_database(context, dbname):

    LOOPS = 10
    for i in range(LOOPS):
        context.exception = None

        run_gpcommand(context, 'dropdb %s' % dbname)

        if context.exception:
            time.sleep(1)
            continue

        if not check_db_exists(dbname):
            return

        time.sleep(1)

    if context.exception:
        raise context.exception

    raise Exception('db exists after dropping: %s' % dbname)

def drop_database_if_exists(context, dbname):
    if check_db_exists(dbname):
        drop_database(context, dbname)

def run_on_all_segs(context, dbname, query):
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    primary_segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary()]

    for seg in primary_segs:
        with dbconn.connect(dbconn.DbURL(dbname=dbname, hostname=seg.getSegmentHostName(), port=seg.getSegmentPort()), utility=True) as conn: 
            dbconn.execSQL(conn, query)  
            conn.commit()

def get_nic_up(hostname, nic):
    address = hostname + '-cm'
    cmd = Command(name='ifconfig nic', cmdStr='sudo /sbin/ifconfig %s' % nic, remoteHost=address, ctxt=REMOTE)
    cmd.run(validateAfter=True)

    return 'UP' in cmd.get_results().stdout
   
def bring_nic_down(hostname, nic):
    address = hostname + '-cm'
    cmd = Command(name='bring down nic', cmdStr='sudo /sbin/ifdown %s' % nic, remoteHost=address, ctxt=REMOTE)
    cmd.run(validateAfter=True)
    
    if get_nic_up(hostname, nic):
        raise Exception('Unable to bring down nic %s on host %s' % (nic, hostname))

def bring_nic_up(hostname, nic):
    address = hostname + '-cm'
    cmd = Command(name='bring up nic', cmdStr='sudo /sbin/ifup %s' % nic, remoteHost=address, ctxt=REMOTE)
    cmd.run(validateAfter=True)
    
    if not get_nic_up(hostname, nic):
        raise Exception('Unable to bring up nic %s on host %s' % (nic, hostname))

def are_segments_synchronized():
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    segments = gparray.getDbList()
    for seg in segments:
        if seg.mode != MODE_SYNCHRONIZED:
            return False 
    return True

def get_distribution_policy(dbname):
    filename = dbname.strip() + "_dist_policy_backup"
    get_dist_policy_to_file(filename, dbname)

def get_dist_policy_to_file(filename, dbname):
    dist_policy_sql = " \
            SELECT \
                c.relname as tablename, p.attrnums as distribution_policy \
            FROM \
                pg_class c \
                INNER JOIN \
                gp_distribution_policy p \
                ON (c.relfilenode = p.localoid) \
                AND \
                c.relstorage != 'x' \
            ORDER BY c.relname"

    current_dir = os.getcwd()
    filename = os.path.join(current_dir, './gppylib/test/data', filename)
    data_sql = "COPY (%s) TO '%s'" %(dist_policy_sql, filename)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        dbconn.execSQL(conn, data_sql)
        conn.commit()

def validate_distribution_policy(context, dbname):
    filename = dbname.strip() + "_dist_policy_restore"
    get_dist_policy_to_file(filename, dbname)
    current_dir = os.getcwd()
    backup_file = os.path.join(current_dir, './gppylib/test/data', dbname.strip() + "_dist_policy_backup")
    restore_file = os.path.join(current_dir, './gppylib/test/data', dbname.strip() + "_dist_policy_restore")
    diff_backup_restore_data(context, backup_file, restore_file)
   
def check_empty_table(tablename, dbname):
    NUM_ROWS_QUERY = 'select count(*) from %s' % tablename
    # We want to bubble up the exception so that if table does not exist, the test fails
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        result = dbconn.execSQLForSingleton(conn, NUM_ROWS_QUERY)

    if result != 0:
        raise Exception('Unexpected rows in table %s.%s, rows = %d' % (dbname, tablename, result))
     
def get_master_hostname(dbname='template1'):
    master_hostname_sql = "select distinct hostname from gp_segment_configuration where content=-1"
    return getRows(dbname, master_hostname_sql)

def get_hosts_and_datadirs(dbname='template1'):
    get_hosts_and_datadirs_sql = "select hostname, fselocation from gp_segment_configuration, pg_filespace_entry where fsedbid = dbid and role='p';"
    return getRows(dbname, get_hosts_and_datadirs_sql)

def get_hosts(dbname='template1'):
    get_hosts_sql = "select distinct hostname from gp_segment_configuration where role='p';"
    return getRows(dbname, get_hosts_sql)

def get_backup_dir_for_host(hostname, dbname='template1'):
    get_backup_dir_sql = "select f.fselocation from pg_filespace_entry f inner join gp_segment_configuration g on \
                            f.fsedbid=g.dbid and g.role='p' and g.hostname = '%s'" % (hostname)
    return getRows(dbname, get_backup_dir_sql)
   
def cleanup_dir(context, host, location):
    cleanup_cmd = "gpssh -h %s -e 'rm -rf %s/db_dumps'" % (host, location)
    run_command(context, cleanup_cmd)
    if context.exception:
        raise context.exception
    
def cleanup_backup_files(context, dbname, location=None):
    hosts = get_hosts(dbname)
    for host in hosts:
        if location:
            cleanup_dir(context, host[0], location)
        else:
            dirs = get_backup_dir_for_host(host[0], dbname)
            for dir in dirs:
                cleanup_dir(context, host[0], dir[0])

def cleanup_report_files(context, master_data_dir):
    if not master_data_dir:
        raise Exception("master_data_dir not specified in cleanup_report_files")
    if master_data_dir.strip() == '/':
        raise Exception("Can't call cleanup_report_files on root directory")

    file_pattern = "gp_*.rpt"
    cleanup_cmd = "rm -f %s/%s" % (master_data_dir, file_pattern)
    run_command(context, cleanup_cmd)
    if context.exception:
        raise context.exception

def truncate_table(dbname, tablename):
    TRUNCATE_SQL = 'TRUNCATE %s' % tablename
    execute_sql(dbname, TRUNCATE_SQL)

def verify_truncate_in_pg_stat_last_operation(context, dbname, oid):
    VERIFY_TRUNCATE_SQL = """SELECT *
                             FROM pg_stat_last_operation
                             WHERE objid = %d and staactionname = 'TRUNCATE' """ % oid
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        row = dbconn.execSQLForSingletonRow(conn, VERIFY_TRUNCATE_SQL)
    if len(row) != 7:
        raise Exception('Invalid number of colums %d' % len(row))
    if row[2] != 'TRUNCATE':
        raise Exception('Actiontype not expected TRUNCATE "%s"' % row[2])
    if row[5]:
        raise Exception('Subtype for TRUNCATE operation is not empty %s' % row[5])

def verify_truncate_not_in_pg_stat_last_operation(context, dbname, oid):
    VERIFY_TRUNCATE_SQL = """SELECT count(*)
                             FROM pg_stat_last_operation
                             WHERE objid = %d and staactionname = 'TRUNCATE' """ % oid
    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        thecount = dbconn.execSQLForSingleton(conn, VERIFY_TRUNCATE_SQL)
        if thecount != 0:
            raise Exception("Found %s rows from query '%s' should be 0" % (thecount, VERIFY_TRUNCATE_SQL))

def get_table_oid(context, dbname, schema, tablename):
    OID_SQL = """SELECT c.oid 
                 FROM pg_class c, pg_namespace n
                 WHERE c.relnamespace = n.oid AND c.relname = '%s' AND n.nspname = '%s'""" % (tablename, schema)

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:
        oid = dbconn.execSQLForSingleton(conn, OID_SQL)

    return oid

def insert_numbers(dbname, tablename, lownum, highnum):
    sql = "insert into %s select generate_series(%s, %s)" % (tablename, lownum, highnum)
    execute_sql(dbname, sql)

def verify_integer_tuple_counts(context, filename):
    with open(filename, 'r') as fp:
        for line in fp:
            tupcount = line.split(',')[-1].strip()
            if re.match("^\d+?\.\d+?$", tupcount) is not None: 
                raise Exception('Expected an integer tuplecount in file %s found float' % filename)

def create_fake_pg_aoseg_table(context, table, dbname):
    sql = """CREATE TABLE %s(segno int, 
                             eof double precision,
                             tupcount double precision,
                             varblockcount double precision,
                             eofuncompressed double precision)""" % table
    execute_sql(dbname, sql)

def insert_row(context, row_values, table, dbname):
    sql = """INSERT INTO %s values(%s)""" % (table, row_values)
    execute_sql(dbname, sql)

def copy_file_to_all_db_hosts(context, filename):

    hosts_set = set()
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    for seg in gparray.getDbList():
        if seg.isSegmentPrimary():
            hosts_set.add(seg.getSegmentAddress())

    hostfile = '/tmp/copy_host_file.behave' 
    with open(hostfile, 'w') as fd:
        for h in hosts_set:
            fd.write('%s\n' % h)
    
    cmd = 'gpscp -f %s %s =:%s' % (hostfile, filename, filename)
    run_command(context, cmd)
    if context.exception:
        raise Exception("FAIL: '%s' '%s'" % (cmd, context.exception.__str__()))

    os.remove(hostfile)

def create_large_num_partitions(table_type, table_name, db_name):
    if table_type == "ao":
        condition = "with(appendonly=true)"
    elif table_type == "co":
        condition = "with(appendonly=true, orientation=column)"
    else:
        condition = ""

    create_large_partitions_sql = """
                                    create table %s (column1 int, column2 int) %s partition by range(column1) subpartition by range(column2) subpartition template(start(1) end(75) every(1)) (start(1) end(75) every(1))
                                  """ % (table_name, condition)
    execute_sql(db_name, create_large_partitions_sql)

    verify_table_exists_sql = """select count(*) from pg_class where relname = '%s'""" % table_name
    num_rows = getRows(db_name, verify_table_exists_sql)[0][0] 
    if num_rows != 1:
        raise Exception('Creation of table "%s:%s" failed. Num rows in pg_class = %s' % (db_name, table_name, num_rows))

def validate_num_restored_tables(context, num_tables, dbname):
    tbls = get_table_names(dbname)

    count_query = """select count(*) from %s"""
    num_validate_tables = 0
    for t in tbls:
        name = '%s.%s' % (t[0], t[1])
        count = getRows(dbname, count_query % name)[0][0]
        if count == 0:
            continue
        else:
            validate_restore_data(context, name, dbname)
            num_validate_tables += 1

    if num_validate_tables != int(num_tables.strip()):
        raise Exception('Invalid number of tables were restored. Expected "%s", Actual "%s"' % (num_tables, num_validate_tables)) 

def get_partition_list(partition_type, dbname):
    if partition_type == 'ao':
        sql = GET_ALL_AO_DATATABLES_SQL
    elif partition_type == 'co':
        sql = GET_ALL_CO_DATATABLES_SQL

    partition_list = getRows(dbname, sql) 
    for line in partition_list:
        if len(line) != 4:
            raise Exception('Invalid results from query to get all AO tables: [%s]' % (','.join(line)))
    return partition_list

def verify_stats(dbname, partition_info):

    for (oid, schemaname, partition_name, tupletable) in partition_info:
        tuple_count_sql = "select to_char(sum(tupcount::bigint), '999999999999999999999') from pg_aoseg.%s" % tupletable
        tuple_count = getRows(dbname, tuple_count_sql)[0][0]
        if tuple_count:
            tuple_count = tuple_count.strip()
        else:
            tuple_count = '0'
        validate_tuple_count(dbname, schemaname, partition_name, tuple_count)

def validate_tuple_count(dbname, schemaname, partition_name, tuple_count):    
    sql = 'select count(*) from %s.%s' % (schemaname, partition_name)
    row_count = getRows(dbname, sql)[0][0]
    if int(row_count) != int(tuple_count):
        raise Exception('Stats for the table %s.%s does not match. Stat count "%s" does not match the actual tuple count "%s"' % (schemaname, partition_name, tuple_count, row_count))
    
    
def validate_no_aoco_stats(context, dbname, table):

    with dbconn.connect(dbconn.DbURL(dbname=dbname)) as conn:

        sql = "select relname from pg_class where oid in (select segrelid from pg_appendonly where relid in (select oid from pg_class where relname = '%s'))" % table
        tname = dbconn.execSQLForSingleton(conn, sql)
        sql = "select tupcount from pg_aoseg.%s" % tname.strip()
        rows = getRows(dbname, sql)
        if len(rows) != 0:
            raise Exception("%s has stats of %d rows in %s table and should be 0" % (table, int(rows[0][0]), tname))
 

