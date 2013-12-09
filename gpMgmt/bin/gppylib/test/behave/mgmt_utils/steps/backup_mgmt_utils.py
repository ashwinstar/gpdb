import thread
from gppylib.commands.base import Command, ExecutionError, REMOTE, WorkerPool
from gppylib.db import dbconn
from gppylib.test.behave_utils.utils import run_gpcommand, getRows

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

