import os
import shutil

from gppylib.test.behave_utils.utils import drop_database_if_exists, start_database_if_not_started,\
                                            create_database, enable_postgis_and_load_test_data,    \
                                            enable_postgis_and_load_test_data_for_postgis_1,       \
                                            run_command, check_user_permissions, run_gpcommand

def before_feature(context, feature):
    drop_database_if_exists(context, 'testdb')
    drop_database_if_exists(context, 'bkdb')
    drop_database_if_exists(context, 'fullbkdb')
    drop_database_if_exists(context, 'schematestdb')

    # enable postgis when running test cases for postgis
    if 'postgis' in feature.tags:
        drop_database_if_exists(context, 'opengeo')
        create_database(context, 'opengeo')
        enable_postgis_and_load_test_data(context)

    if 'postgis_14' in feature.tags:
        drop_database_if_exists(context, 'opengeo')
        create_database(context, 'opengeo')
        enable_postgis_and_load_test_data_for_postgis_1(context)

    if 'gpperfmon' in feature.tags:
        drop_database_if_exists(context, 'gpperfmon')
        pgport = os.getenv('PGPORT', 5432)
        command = "gpperfmon_install --enable --password changeme --port %s" % pgport
        run_gpcommand(context, command)
        run_gpcommand(context, "gpstop -ar")


def before_scenario(context, scenario):
    master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
    fn = os.path.join(master_data_dir, 'dirty_hack.txt')
    if os.path.exists(fn):
        os.remove(fn)
    start_database_if_not_started(context)
    drop_database_if_exists(context, 'testdb')

def after_scenario(context, scenario):
    master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
    fn = os.path.join(master_data_dir, 'dirty_hack.txt')
    if os.path.exists(fn):
        #os.remove(fn)
        pass
    start_database_if_not_started(context)

    home_dir = os.path.expanduser('~')
    if not check_user_permissions(home_dir, 'write') and hasattr(context, 'orig_write_permission')\
                                                     and context.orig_write_permission:
        run_command(context, 'sudo chmod u+w %s' % home_dir) 

    if os.path.isdir('%s/gpAdminLogs.bk' % home_dir):
        shutil.move('%s/gpAdminLogs.bk' % home_dir, '%s/gpAdminLogs' % home_dir)

