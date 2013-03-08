import os
from gppylib.test.behave_utils.utils import drop_database_if_exists, start_database_if_not_started

def before_feature(context, feature):
    drop_database_if_exists(context, 'testdb')
    drop_database_if_exists(context, 'bkdb')
    drop_database_if_exists(context, 'fullbkdb')
    drop_database_if_exists(context, 'schematestdb')

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
