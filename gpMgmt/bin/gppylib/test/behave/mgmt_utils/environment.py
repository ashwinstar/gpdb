from gppylib.test.behave_utils.utils import drop_database_if_exists, start_database_if_not_started

def before_scenario(context, scenario):
    start_database_if_not_started(context)
    drop_database_if_exists(context, 'testdb')

def after_scenario(context, scenario):
    start_database_if_not_started(context)
