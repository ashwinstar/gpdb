import os
import time

from gppylib.commands.base import Command, ExecutionError
from gppylib.db import dbconn
from gppylib.gparray import GpArray
from gppylib.test.behave_utils.utils import bring_nic_down, bring_nic_up, \
                                            create_database, check_err_msg, check_stdout_msg, \
                                            check_return_code, create_int_table, \
                                            drop_database, drop_database_if_exists, \
                                            start_database_if_not_started, stop_database_if_started, \
                                            run_gpcommand, are_segments_synchronized

@given('the database is running')
def impl(context): 
    start_database_if_not_started(context)
    if context.exception:
        raise context.exception

@given('the database is not running')
def impl(context): 
    stop_database_if_started(context)
    if context.exception:
        raise context.exception

@given('database "{dbname}" exists')
def impl(context, dbname):
    if not create_database(context, dbname):
        raise Exception("Failed to create database %s" % dbname)

@given('we have determined the first segment hostname')
def impl(context):
    gparray = GpArray.initFromCatalog(dbconn.DbURL())
    segment_hostlist = sorted(gparray.get_hostlist(includeMaster=False)) 
    if not segment_hostlist:
        raise Exception('segment_hostlist was empty')
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
    
@given('there is {table_type} table {table_name} in "{dbname}" with data')
def impl(context, table_type, table_name, dbname):
    if not create_database(context, dbname):
        raise Exception("Failed to create database %s" % dbname)
    if not create_table(context, table_type, table_name, dbname):
        raise Exception("Failed to create table %s in db %s" % (table_name, dbname))

@given('there are no AO tables in "{dbname}"')
def impl(context, dbname):
    drop_ao_tables(dbname) 

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

@then('{env_var} environment variable should be restored')
def impl(context, env_var):
    if not hasattr(context, 'orig_env'):
        raise Exception('%s can not be reset' % env_var)

    if env_var not in context.orig_env:
        raise Exception('%s can not be reset.' % env_var)

    os.environ[env_var] = context.orig_env[env_var]

    del context.orig_env[env_var]

@when('the user runs "{command}"')
@then('the user runs "{command}"')
def impl(context, command):
    run_gpcommand(context, command)
 
@then('{command} should print {err_msg} error message')
def impl(context, command, err_msg):
    check_err_msg(context, err_msg)

@then('{command} should print {out_msg} to stdout')
def impl(context, command, out_msg):
    check_stdout_msg(context, out_msg)

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

@then('database health check should pass')
def impl(context):
    
    drop_database_if_exists(context, 'testdb')
    create_database(context, 'testdb')
    create_int_table(context, 't1')
    drop_database(context, 'testdb')

    
@then('the segments are synchronized')
def impl(context):

    times = 30
    sleeptime = 10

    for i in range(times):
        if are_segments_synchronized():
            return
        time.sleep(sleeptime)

    raise Exception('segments are not in sync after %d seconds' % (times * sleeptime))

