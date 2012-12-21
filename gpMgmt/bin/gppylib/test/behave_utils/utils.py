#!/usr/bin/env python
import os, time

from gppylib.commands.gp import GpStart, chk_local_db_running
from gppylib.commands.base import Command, ExecutionError, REMOTE
from gppylib.db import dbconn
from gppylib.gparray import GpArray, MODE_SYNCHRONIZED

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
        err_str = "Expected stdout string '%s' and found: '%s'" % (err_msg, context.stdout_message)
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

    if not 'MASTER_DATA_DIRECTORY' in os.environ:
        raise Exception('MASTER_DATA_DIRECTORY should be set')

    pgport = int(os.environ['PGPORT'])
    master_data_dir = os.environ['MASTER_DATA_DIRECTORY']

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
    run_gpcommand(context, 'gpstop -a')
    if context.exception:
        raise context.exception

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

def create_database(context, dbname):
    run_gpcommand(context, 'createdb %s' % dbname)

    if context.exception:
        raise context.exception

    if not check_db_exists(dbname):
        return Exception("After createdb '%s' db does not exist" % dbname)


def create_int_table(context, table_name, table_type='heap', dbname='testdb'):
    CREATE_TABLE_SQL = None
    NROW = 1000
 
    if table_type == 'AO':
        CREATE_TABLE_SQL = 'create table %s WITH(APPENDONLY=TRUE) as select generate_series(1,%d) as c1' % (table_name, NROW)
    elif table_type == 'CO':
        CREATE_TABLE_SQL = 'create table %s WITH(APPENDONLY=TRUE, orientation=column) as select generate_series(1, %d) as c1' % (table_name, NROW)
    elif table_type == 'heap':
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
    run_gpcommand(context, 'dropdb %s' % dbname)

    if context.exception:
        raise context.exception

    if check_db_exists(dbname):
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
