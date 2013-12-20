# coding: utf-8 

import thread
from gppylib.commands.base import Command, ExecutionError, REMOTE, WorkerPool
from gppylib.db import dbconn
from gppylib.test.behave_utils.utils import run_gpcommand, getRows

comment_start_expr = '-- '
comment_expr = '-- Name: '
comment_data_expr_a = '-- Data: '
comment_data_expr_b = '-- Data for Name: '
len_start_comment_expr = len(comment_start_expr)

@given('the user locks "{table_name}" in "{lock_mode}" using connection "{conn}" on "{dbname}"')
@when('the user locks "{table_name}" in "{lock_mode}" using connection "{conn}" on "{dbname}"')
@then('the user locks "{table_name}" in "{lock_mode}" using connection "{conn}" on "{dbname}"')
def impl(context, table_name, lock_mode, conn, dbname):
    query = "begin; lock table %s in %s" % (table_name, lock_mode)
    conn = dbconn.connect(dbconn.DbURL(dbname=dbname))
    dbconn.execSQL(conn, query) 
    context.conn = conn

@when('the user runs the "{cmd}" in a worker pool "{poolname}"')
@then('the user runs the "{cmd}" in a worker pool "{poolname}"')
def impl(context, cmd, poolname):
    command = Command(name='run gpcrondump in a separate thread', cmdStr=cmd)
    pool = WorkerPool(numWorkers=1)
    pool.addCommand(command)
    if not hasattr(context, 'pool'):
        context.pool = {}
    context.pool[poolname] = pool
    context.cmd = cmd
 
@given('the "{backup_pg}" has a lock on the pg_class table in "{dbname}"')
@when('the "{backup_pg}" has a lock on the pg_class table in "{dbname}"')
@then('the "{backup_pg}" has a lock on the pg_class table in "{dbname}"')
def impl(context, dbname, backup_pg):
    seg_count = 1

    query = """select count(*) 
             from pg_locks 
             where relation in (select oid from pg_class where relname='pg_class') 
                   and locktype='relation' and mode='ExclusiveLock'"""

    row_count = getRows(dbname, query)[0][0]
    if row_count != seg_count:
        raise Exception("Incorrect (number of) lock/locks on pg_class, expected count = %s, received count = %s" % (seg_count, row_count))

@then('the worker pool "{poolname}" is cleaned up')
@when('the worker pool "{poolname}" is cleaned up')
def impl(context, poolname):
    pool = context.pool[poolname]
    if pool:
        pool.join()
        for context.cmd in pool.getCompletedItems():
            result = context.cmd.get_results()
            context.ret_code = result.rc 
            context.stdout_message = result.stdout
            context.error_message = result.stderr 
        pool.haltWork()    
        pool.joinWorkers() 
    else:
        raise Exception('Worker pool is None.Probably behave step to initialize the worker pool is missing.')

@then('the user drops "{tablename}" in "{dbname}" in a worker pool "{poolname}"')
@when('the user drops "{tablename}" in "{dbname}" in a worker pool "{poolname}"')
def impl(context, tablename, dbname, poolname):
    pool = WorkerPool(numWorkers=1)
    cmd = Command(name='drop a table in a worker pool', cmdStr="""psql -c "DROP TABLE %s" -d %s""" % (tablename, dbname))
    pool.addCommand(cmd)
    if not hasattr(context, 'pool'):
        context.pool = {}
    context.pool[poolname] = pool
    

@given('the user closes the connection "{conn_name}"')
@when('the user closes the connection "{conn_name}"')
@then('the user closes the connection "{conn_name}"')
def impl(context, conn_name):
    query = """ROLLBACK;"""
    dbconn.execSQL(context.conn, query) 
    context.conn.close()

@given('verify that "{backup_pg}" has no lock on the pg_class table in "{dbname}"')
@when('verify that "{backup_pg}" has no lock on the pg_class table in "{dbname}"')
@then('verify that "{backup_pg}" has no lock on the pg_class table in "{dbname}"')
def impl(context, backup_pg, dbname):
    query = """select count(*) 
             from pg_locks 
             where relation in (select oid from pg_class where relname='pg_class') 
                   and locktype='relation' and mode='ExclusiveLock'"""

    row_count = getRows(dbname, query)[0][0]
    if row_count != 0:
        raise Exception("Found a ExclusiveLock on pg_class")

@given('there is a "{tabletype}" table "{table_name}" with compression "{compression_type}" in "{dbname}" with data and {rowcount} rows')
@when('there is a "{tabletype}" table "{table_name}" with compression "{compression_type}" in "{dbname}" with data and {rowcount} rows')
@then('there is a "{tabletype}" table "{table_name}" with compression "{compression_type}" in "{dbname}" with data and {rowcount} rows')
def impl(context, tabletype, table_name, compression_type, dbname, rowcount):
    populate_regular_table_data(context, tabletype, table_name, compression_type, dbname, int(rowcount))

@given('verify the metadata dump file syntax under "{directory}" for comments and types')
@when('verify the metadata dump file syntax under "{directory}" for comments and types')
@then('verify the metadata dump file syntax under "{directory}" for comments and types')
def impl(context, directory):
    names = ["Name", "Data", "Data for Name"]
    types = ["TABLE", "TABLE DATA", "EXTERNAL TABLE", "ACL", "CONSTRAINT", "COMMENT", "PROCEDURAL LANGUAGE", "SCHEMA"]
    master_dump_dir = directory if len(directory.strip()) != 0 else master_data_dir
    timestamp = context.backup_timestamp
    metadata_file = '%s/db_dumps/%s/gp_dump_1_1_%s.gz' % (master_dump_dir, timestamp[0:8], timestamp)
    tmp_metadata_file = '/tmp/behave_metadata_file'

    cmd = Command(name="Unzip conetnts of metadata dump file to temp file", cmdStr='zcat %s > %s' % (metadata_file, tmp_metadata_file))
    cmd.run(validateAfter=True)

    try:
        with open(tmp_metadata_file, 'r') as fd:
            lines = fd.readlines() 
            if len(lines) == 0:
                raise Exception('Metadata file has no data')
            for line in lines:
                if (line[:3] == comment_start_expr):
                    if (line.startswith(comment_expr) or line.startswith(comment_data_expr_a) or line.startswith(comment_data_expr_b)):
                        name_k, type_k, schema_k = get_comment_keys(line)
                        if (name_k not in names and type_k != "Type" and schema_k != "Schema"):
                            raise Exception("Unknown key in the comment line of the metdata_file '%s'. Please check and confirm if the key is correct" % (metadata_file))
                        name_v, type_v, schema_v = get_comment_values(line)
                        if (type_v not in types):
                            raise Exception("Value of Type in the comment line '%s' of the metadata_file '%s' does not fall under the expected list %s. Please check if the value is correct" %(type_v, metadata_file, types))
    finally:
        if os.path.exists(tmp_metadata_file):
            os.remove(tmp_metadata_file)

def get_comment_keys(line):
    try:
        temp = line[len_start_comment_expr:]
        tokens = temp.strip().split(';')
        name = tokens[0].split(':')[0].strip()
        type = tokens[1].split(':')[0].strip()
        schema = tokens[2].split(':')[0].strip()
    except:
        return (None, None, None)
    return (name, type, schema) 

def get_comment_values(line):
    try:
        temp = line[len_start_comment_expr:]
        tokens = temp.strip().split(';')
        name = tokens[0].split(':')[1].strip()
        type = tokens[1].split(':')[1].strip()
        schema = tokens[2].split(':')[1].strip()
    except:
        return (None, None, None)
    return (name, type, schema) 

@given('{command} should print {out_msg} to stdout {num} times')
@when('{command} should print {out_msg} to stdout {num} times')
@then('{command} should print {out_msg} to stdout {num} times')
def impl(context, command, out_msg, num):
    msg_list = context.stdout_message.split('\n')
    msg_list = [x.strip() for x in msg_list]

    count = msg_list.count(out_msg)
    if count != int(num):
        raise Exception("Expected %s to occur %s times. Found %d" % (out_msg, num, count))
