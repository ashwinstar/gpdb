#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

"""
TODO: docs!
"""
import os

from gppylib.gplog import *
from base import *
from unix import *



logger = get_default_logger()

#TODO:  need a better way of managing environment variables.
GPHOME=os.environ.get('GPHOME')


def check_passive_standby(name,dir):
    cmd=FileContainsTerm(name,'gpsync',"%s/postmaster.opts" % dir)
    cmd.run(validateAfter=True)
    return cmd.contains_term()

def get_max_dbid(name,conn):
    try:
        curs=conn.cursor()
        curs.execute("SELECT max(dbid) FROM gp_configuration")
        rows = curs.fetchall()
        if len(rows) != 1:
            raise Exception, 'Failed to retrieve maximum dbid from catalog'
        return rows[0][0]
    finally:
        curs.close()
        
class PySync(Command):
    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None):
        psync_executable=GPHOME + "/bin/lib/pysync.py"
        self.cmdStr="%s %s %s:%s" % (psync_executable,srcDir,dstHost,dstDir)        
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)


        
class SegmentStart(Command):      
    def __init__(self, name, dataDir, port, utilityMode=False, ctxt=LOCAL, remoteHost=None):
        if utilityMode:
            self.cmdStr='pg_ctl -w -D %s -l %s.log -o \'-i -p %d -c gp_role=utility\' start 2>&1' % \
                (dataDir, dataDir, port)
        else:
            raise Exception, "Not implemented: SegmentStart in non-utility mode"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

        
class SegmentStop(Command):
    def __init__(self, name, dataDir, ctxt=LOCAL, remoteHost=None):
        self.cmdStr='pg_ctl stop -D %s' % dataDir
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)



class GpStart(Command):
    def __init__(self, name, masterOnly=False, ctxt=LOCAL, remoteHost=None):
        cmdStr="gpstart -a"
        if masterOnly:
            cmdStr += " -m"
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False):
        cmd=GpStart(name,masterOnly)
        cmd.run(validateAfter=True)
       
           
class GpStop(Command):
    def __init__(self, name, masterOnly=False, ctxt=LOCAL, remoteHost=None):
        cmdStr="gpstop -af"
        if masterOnly:
            cmdStr += " -m"
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    @staticmethod
    def local(name,masterOnly=False):
        cmd=GpStop(name,masterOnly)
        cmd.run(validateAfter=True)
        
        
class Psql(Command):
    def __init__(self, name, query, database='template1', utilityMode=False, ctxt=LOCAL, remoteHost=None):
        env = ''
        if utilityMode:
            env = 'PGOPTIONS="-c gp_session_role=utility"'
        cmdStr = '%s psql -c "%s" %s' % (env, query, database)
        # Need to escape " for REMOTE or it'll interfere with ssh 
        if ctxt == REMOTE:
            cmdStr = cmdStr.replace('"', '\\"')
        self.cmdStr=cmdStr
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)


class ModifyPostgresqlConfSetting(Command):
    def __init__(self, name, file, optName, optVal, optType='string', ctxt=LOCAL, remoteHost=None):
        cmdStr = None
        if optType == 'number':
#            cmdStr = 'perl -p -i.bak -e \'s/^%s[ ]*=[ ]*\\d+/%s=%d/\' %s' % (optName, optName, optVal, file)
            cmdStr = "perl -p -i.bak -e 's/^%s[ ]*=[ ]*\\d+/%s=%d/' %s" % (optName, optName, optVal, file)
        elif optType == 'string':
            cmdStr = "perl -i -p -e \"s/^%s[ ]*=[ ]*'[^']*'/%s='%s'/\" %s" % (optName, optName, optVal, file)
        else:
            raise Exception, "Invalid optType for ModifyPostgresqlConfSetting"
        self.cmdStr = cmdStr
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)
        
      

class GpVersion(Command):
    def __init__(self,name,ctxt=LOCAL,remoteHost=None):
        self.cmdStr="$GPHOME/bin/postgres --gp-version"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
    
    def get_version(self):
        return self.results.stdout

        
def distribute_tarball(queue,list,tarball):
        logger.debug("distributeTarBall start")        
        for db in list:
            (head,tail)=os.path.split(db.datadir)
            scp_cmd=RemoteCopy("copy master",tarball,db.get_hostname(),head)            
            queue.addCommand(scp_cmd)
        queue.join()        
        queue.check_results()        
        logger.debug("distributeTarBall finished")
       
