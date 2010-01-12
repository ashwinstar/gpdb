try:
    
    import os, sys
    from optparse import Option, OptionParser 
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.gparray import *
    from gppylib.gphostcache import *
    from gppylib.gplog import *
    from gppylib.commands.unix import *
    from gppylib.commands.gp import *
    from gppylib.db import dbconn
    from gppylib.userinput import *
    from pygresql.pgdb import DatabaseError

except ImportError, e:    
    sys.exit('Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e))

EXECNAME = os.path.split(__file__)[-1]

class SqlCommands:
    def __init__(self):
        self.sqlCmds={}
        self.sqlCmds["gpBackoffProcPerSegment"] = "select setting from pg_settings  where name = \'gp_backoff_procs_per_segment\'"
        self.sqlCmds["gpMasterHost"]="select hostname from gp_configuration where content=-1 and dbid=1 and  isprimary=\'t\'"
    
    def getSqlCmd(self,sqlName, params=None):
        if None == params:
            return self.sqlCmds[sqlName]


        
        