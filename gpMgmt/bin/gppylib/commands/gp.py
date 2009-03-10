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
from gppylib.commands import pg


logger = get_default_logger()

#TODO:  need a better way of managing environment variables.
GPHOME=os.environ.get('GPHOME')
SRC_GPPATH="source %s/greenplum_path.sh;" % GPHOME


def check_passive_standby(name,dir):
    cmd=FileContainsTerm(name,'gpsync',"%s/postmaster.opts" % dir)
    cmd.run(validateAfter=False)
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

#-----------------------------------------------        
class PySync(Command):
    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None):
        psync_executable=GPHOME + "/bin/lib/pysync.py"
        self.cmdStr="%s %s %s %s:%s" % (SRC_GPPATH,psync_executable,srcDir,dstHost,dstDir)        
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)


#-----------------------------------------------
class MasterStart(Command):
    def __init__(self, name, dataDir, port, restrictedMode=False,max_connections=1,
                 disableMasterMirror=False,utilityMode=False, ctxt=LOCAL, remoteHost=None):
        self.dataDir=dataDir
        self.port=port
        self.utilityMode=utilityMode
        cmdfrags=["%s $GPHOME/bin/%s -w -D %s -o \" -i -p %s" % (SRC_GPPATH,"pg_ctl",dataDir,port)]
        
        if utilityMode:
            cmdfrags.append(" -c gp_role=utility ")
        
        if restrictedMode:
            cmdfrags.append(" -c superuser_reserved_connections=%s " % max_connections)
        
        if disableMasterMirror:
            cmdfrags.append(" -y ")    
        
        cmdfrags.append("\" start ")    
        
        cmdStr=" ".join(cmdfrags)
        
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,dataDir,port,restrictedMode=False,max_connections=1,
                 disableMasterMirror=False,utilityMode=False):
        cmd=MasterStart(name,dataDir,port,restrictedMode,max_connections,disableMasterMirror,utilityMode)        
        cmd.run(validateAfter=True)

#-----------------------------------------------
class MasterStop(Command):
    def __init__(self,name,dataDir,ctxt=LOCAL,remoteHost=None):
        self.dataDir=dataDir
        cmdStr="%s $GPHOME/bin/%s -w -D %s stop" % (SRC_GPPATH,"pg_ctl",dataDir)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,dataDir):
        cmd=MasterStop(name,dataDir)
        cmd.run(validateAfter=True)
        
        
#-----------------------------------------------        
class SegmentStart(Command):      
    def __init__(self, name, dataDir, utilityMode=False, nowait=False,port=None,ctxt=LOCAL, remoteHost=None):
        self.dataDir=dataDir
        cmdfrags=['%s %s -D %s -l %s/pg_log/bootstrap.log' % (SRC_GPPATH,pg.PG_CTL,dataDir,dataDir)]
        portOption = ""
        if port:
            portOption = "-p %s" % port
        if not nowait:
            cmdfrags.append('-w')
        if utilityMode:
            cmdfrags.append("-o '-i -c gp_role=utility %s' start 2>&1" % portOption)
        else:
            cmdfrags.append("-o '-i %s' start 2>&1" % portOption)
        self.cmdStr=' '.join(cmdfrags)
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,dataDir,utilityMode=False,nowait=False,port=None):
        cmd=SegmentStart(name,dataDir,utilityMode,nowait,port)
        cmd.run(validateAfter=True)
    
    @staticmethod
    def remote(name,remoteHost,dataDir,utilityMode=False,nowait=False,port=None):
        cmd.SegmentStart(name,dataDir,utilityMode,nowait,port,ctxt=REMOTE, remoteHost=remoteHost)
        cmd.run(validateAfter=True)

#-----------------------------------------------        
class SegmentStop(Command):
    def __init__(self, name, dataDir, ctxt=LOCAL, remoteHost=None):
        self.cmdStr='%s $GPHOME/bin/pg_ctl stop -D %s' % (SRC_GPPATH,dataDir)
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

#-----------------------------------------------
class GpSegStartCmd(Command):
    def __init__(self, name, gphome, dblist, collatelist, gpversion,ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.dblist=dblist
        self.dirlist=[]
        for db in dblist:
            self.dirlist.append(db.datadir)
            
        dirstr=" -D ".join(self.dirlist)        
        colstr=":".join(collatelist)
        self.cmdStr="%s $GPHOME/sbin/gpsegstart.py -D %s -C %s -V '%s'" % (SRC_GPPATH,dirstr,colstr,gpversion)
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
#-----------------------------------------------
class GpInitSystem(Command):
    def __init__(self,name,configFile,hostsFile, ctxt=LOCAL, remoteHost=None):
        self.configFile=configFile
        self.hostsFile=hostsFile
        self.cmdStr="%s $GPHOME/bin/gpinitsystem -a -c %s -h %s" % (SRC_GPPATH,configFile,hostsFile)
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)

#-----------------------------------------------
class GpDeleteSystem():
    def __init__(self,name,datadir, ctxt=LOCAL, remoteHost=None):
        self.datadir=datadir
        self.input="y\ny\n"
        self.cmdStr="%s $GPHOME/bin/gpdeletesystem -d %s -f " % (SRC_GPPATH,datadir)
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost,stdin=self.input)

#-----------------------------------------------
class GpStart(Command):
    def __init__(self, name, masterOnly=False, ctxt=LOCAL, remoteHost=None):
        self.cmdStr="%s $GPHOME/bin/gpstart -a" % (SRC_GPPATH)
        if masterOnly:
            self.cmdStr += " -m"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False):
        cmd=GpStart(name,masterOnly)
        cmd.run(validateAfter=True)
       
#-----------------------------------------------           
class GpStop(Command):
    def __init__(self, name, masterOnly=False, restart=False, fast=False, force=False,ctxt=LOCAL, remoteHost=None):
        self.cmdStr="%s $GPHOME/bin/gpstop -af" % SRC_GPPATH
        if masterOnly:
            self.cmdStr += " -m"
        if restart:
            self.cmdStr += " -r"
        if fast:
            self.cmdStr += " -f"
        if force:
            self.cmdStr += " -z"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False, restart=False, fast=False, force=False):
        cmd=GpStop(name,masterOnly)
        cmd.run(validateAfter=True)
        
#-----------------------------------------------        
class Psql(Command):
    def __init__(self, name, query, database='template1', utilityMode=False, ctxt=LOCAL, remoteHost=None):
        env = ''
        if utilityMode:
            env = 'PGOPTIONS="-c gp_session_role=utility"'
        cmdStr = '%s %s $GPHOME/bin/psql -c "%s" %s' % (SRC_GPPATH,env, query, database)
        # Need to escape " for REMOTE or it'll interfere with ssh 
        if ctxt == REMOTE:
            cmdStr = cmdStr.replace('"', '\\"')
        self.cmdStr=cmdStr
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)

#-----------------------------------------------
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

#-----------------------------------------------        
class ConfigExpansionSeg(Command):
    def __init__(self, name, dataDir, port, numSegs, contentId, dbId, newSegment=False, tarFile=None, ctxt=LOCAL, remoteHost=None):
        cmdStr = '%s $GPHOME/bin/lib/gpconfexpseg -d %s -s %s -c %d -i %d' % (SRC_GPPATH,dataDir, numSegs, contentId, dbId)                
        if port:
            cmdStr += ' -p %s' % port
        if newSegment:
            cmdStr += ' -n'
        if tarFile:
            cmdStr += ' -t %s' % tarFile
            
        Command.__init__(self, name, cmdStr, ctxt, remoteHost)

#-----------------------------------------------
class GpVersion(Command):
    def __init__(self,name,gphome,ctxt=LOCAL,remoteHost=None):
        self.gphome=gphome
        self.cmdStr="%s $GPHOME/bin/postgres --gp-version" % SRC_GPPATH
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
    
    def get_version(self):
        return self.results.stdout.strip()

    @staticmethod
    def local(name,gphome):
        cmd=GpVersion(name,gphome)
        cmd.run(validateAfter=True)
        return cmd.get_version()
    
    
        
def distribute_tarball(queue,list,tarball):
        logger.debug("distributeTarBall start")        
        for db in list:
            (head,tail)=os.path.split(db.datadir)
            scp_cmd=RemoteCopy("copy master",tarball,db.get_hostname(),head)            
            queue.addCommand(scp_cmd)
        queue.join()        
        queue.check_results()        
        logger.debug("distributeTarBall finished")



#=-=-=-=-=-=-=-=-=-= Bash Migration Helper Functions =-=-=-=-=-=-=-=-

def start_standbymaster(host,datadir,port):
    #TODO: check if there is already a process running
    
    #start the syncmaster
    
    #make sure its started.
    pass

# these match names from gp_bash_functions.sh
def chk_gpdb_id():
    
    #TODO: make sure that our user is the same one that owns the GP binaries.
    #TODO: make sure that our group is the same group that owns the GP binaries
    
    #TODO:  that the USER variable is set.
    
    
    pass        

def chk_local_db_running(datadir,port):
    """Perform a few checks to see if the db is running.  We 1st look at:
       1) /tmp/.s.PGSQL.<PORT> and /tmp/.s.PGSQL.<PORT>.lock
       2) DATADIR/postmaster.pid
       3) netstat 
       
       - if all 3 are negative we say no.       
       - if all 3 are positive we say yes.       
       - if they don't all agree then we throw an Error
    
    """    
    cmd=FileDirExists('check for master PID file',datadir + '/postmaster.pid')
    cmd.run(validateAfter=True)    
    master_pid_exists = cmd.filedir_exists()
    
    
    cmd=FileDirExists('check for /tmp/.s.PGSQL file file',"/tmp/.s.PGSQL.%s" % port)
    cmd.run(validateAfter=True)    
    tmpfile_exists = cmd.filedir_exists()
        
        
    netstat_port_active = PgPortIsActive.local('check netstat for master port',"/tmp/.s.PGSQL.%s" % port, port)

    logger.debug("master_pid_exists: %s tmpfile_exists: %s netstat port: %s" %\
                (master_pid_exists,tmpfile_exists,netstat_port_active))
    
    return (master_pid_exists,tmpfile_exists,netstat_port_active)

    if master_pid_exists and tmpfile_exists and netstat_port_active:
        return True
    if not master_pid_exists and not tmpfile_exists and not netstat_port_active:
        return False
    
    msg="Not sure if database is running in datadir %s on port %s.\n" % (datadir,port)
    
    if master_pid_exists:
        msg += "Found a PID file in %s/postmaster.pid\n" % datadir
    else:
        msg += "Did not find a PID file in %s/postmaster.pid\n" % datadir
    
    if tmpfile_exists:
        msg += "Found a tmpfile in /tmp/.s.PGSQL.%s\n" % port
    else:
        msg += "Did not find a tmpfile in /tmp/.s.PGSQL.%s\n" % port
        
    if netstat_port_active:
        msg += "Found a netstat entry for .s.PGSQL.%s" % port
    else:
        msg += "Did not find a netstat entry for .s.PGSQL.%s" % port
        
    raise Exception(msg)
                

            
    
    
    