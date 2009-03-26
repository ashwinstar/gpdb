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
import pg


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
    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None, options=None):
        psync_executable=GPHOME + "/bin/lib/pysync.py"
        self.cmdStr="%s %s %s %s %s:%s" % (SRC_GPPATH,
                                           psync_executable,
                                           options if options else "",
                                           srcDir,
                                           dstHost,
                                           dstDir)
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)


#-----------------------------------------------
class MasterStart(Command):
    def __init__(self, name, dataDir, port, restrictedMode=False,max_connections=1,
                 disableMasterMirror=False,utilityMode=False, ctxt=LOCAL, remoteHost=None):
        self.dataDir=dataDir
        self.port=port
        self.utilityMode=utilityMode
        cmdfrags=["%s $GPHOME/bin/%s -w -D %s -o \" -E -i -p %s" % (SRC_GPPATH,"pg_ctl",dataDir,port)]
        
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
        cmdfrags=['%s %s -D %s -l %s/pg_log/startup.log ' % (SRC_GPPATH,pg.PG_CTL,dataDir,dataDir)]
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
        cmd=SegmentStart(name,dataDir,utilityMode,nowait,port,ctxt=REMOTE, remoteHost=remoteHost)
        cmd.run(validateAfter=True)

#-----------------------------------------------        
class SegmentStop(Command):
    def __init__(self, name, dataDir,mode='smart', ctxt=LOCAL, remoteHost=None):        
        cmdfrags=['%s $GPHOME/bin/pg_ctl stop -D %s -m %s' % (SRC_GPPATH,dataDir,mode)]           
        cmdStr=' '.join(cmdfrags)    
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name, dataDir):
        cmd=SegmentStop(name, dataDir)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class GpSegStartCmd(Command):
    def __init__(self, name, gphome, dblist, collatelist, gpversion,trust=False,ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.dblist=dblist
        self.dirlist=[]
        for db in dblist:
            self.dirlist.append(db.datadir)
            
        dirstr=" -D ".join(self.dirlist)        
        colstr=":".join(collatelist)
        cmdStr="%s $GPHOME/sbin/gpsegstart.py -D %s -C %s -V '%s'" % (SRC_GPPATH,dirstr,colstr,gpversion)
        if trust:
            cmdStr += " --trust"
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)



#-----------------------------------------------
class GpSegStopCmd(Command):
    def __init__(self, name, gphome, dblist, mode, ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.dblist=dblist
        self.dirlist=[]
        self.mode=mode
        for db in dblist:
            self.dirlist.append(db.datadir)
            
        dirstr=" -D ".join(self.dirlist)        
        colstr=":".join(collatelist)
        self.cmdStr="%s $GPHOME/sbin/gpsegstop.py -D %s -m %s'" % (SRC_GPPATH,dirstr,mode)
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)


#-----------------------------------------------
class GpStandbyStart(Command):
    def __init__(self,name,datadir,port,ctxt=LOCAL, remoteHost=None):
        self.datadir=datadir
        cmdStr="%s exec \"$GPHOME/bin/gpsyncmaster -D %s -p %d -i \" >> standby.logfile 2>&1 &" % (SRC_GPPATH,datadir,port)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def remote(name,host,datadir,port):
        cmd=GpStandbyStart(name,datadir,port,ctxt=REMOTE,remoteHost=host)
        cmd.run(validateAfter=True)
        return cmd
        
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
    def __init__(self, name, masterOnly=False, restricted=False, ctxt=LOCAL, remoteHost=None):
        self.cmdStr="%s $GPHOME/bin/gpstart -a" % (SRC_GPPATH)
        if masterOnly:
            self.cmdStr += " -m"
        if restricted:
            self.cmdStr += " -R"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False,restricted=False):
        cmd=GpStart(name,masterOnly,restricted)
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
        return cmd
        
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
    def __init__(self, name, dataDir, port, numSegs, contentId, dbId, newSegment=False, tarFile=None, verbose=False,ctxt=LOCAL, remoteHost=None):
        cmdStr = '%s $GPHOME/bin/lib/gpconfexpseg -d %s -s %s -c %d -i %d' % (SRC_GPPATH,dataDir, numSegs, contentId, dbId)                
        if port:
            cmdStr += ' -p %s' % port
        if newSegment:
            cmdStr += ' -n'
        if tarFile:
            cmdStr += ' -t %s' % tarFile
        if verbose:
            cmdStr += ' -v '
            
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



def readGpHostCache(gparray,pool):
    
    hostcache={}
    path=os.path.expanduser("~") + "/.gphostcache"
    if os.path.isfile(path):
        try:
            file = open(path, 'r')
            
            for line in file:
                (interface,hostname)=line.split(':')
                hostcache[interface.strip()]=hostname.strip()                    
        except IOError:
            raise Exception(' file %s not found' % path)                    
        finally:
            file.close()
    else:
        #let's create one
        hostlist=gparray.get_hostlist()
        cmdlist=[]
        for host in hostlist:
            cmd=Hostname('host lookup',ctxt=REMOTE,remoteHost=host)
            pool.addCommand(cmd)
            cmdlist.append(cmd)
        pool.join()
        pool.check_results()
        
        
        file = open(path, 'w')
        
        for cmd in cmdlist:
            if cmd.remotehost not in hostcache:
                file.write("%s:%s\n" % (cmd.remotehost,cmd.get_hostname()))
                hostcache[cmd.remotehost]=cmd.get_hostname()
        file.flush()
        file.close()
        
    return hostcache        




class GpError(Exception): pass 

######            
def get_user():
    logger.debug("Checking if LOGNAME or USER env variable is set.")
    username = os.environ.get('LOGNAME') or os.environ.get('USER')
    if not username:
        raise GpError('Environment Variable LOGNAME or USER not set')
    return username

    
def get_gphome():   
    logger.debug("Checking if GPHOME env variable is set.")
    gphome=os.getenv('GPHOME',None)
    if not gphome:            
        raise GpError('Environment Variable GPHOME not set')
    return gphome
  
    
######
def get_masterdatadir():
    logger.debug("Checking if MASTER_DATA_DIRECTORY env variable is set.")
    master_datadir = os.environ.get('MASTER_DATA_DIRECTORY')
    if master_datadir is None:
        raise GpError("Environment Variable MASTER_DATA_DIRECTORY not set!")
    return master_datadir


          
######
def check_permissions(username):
    logger.debug("--Checking that current user can use GP binaries")
    chk_gpdb_id(username)   
  
  
        
######
def standby_check(master_datadir):
    logger.debug("---Make sure we aren't a passive standby master")
    if (check_passive_standby('check if passive standby',master_datadir)):
        raise GpError('Cannot run this utility on the standby instance')
  
    
    
    
#=-=-=-=-=-=-=-=-=-= Bash Migration Helper Functions =-=-=-=-=-=-=-=-

def start_standbymaster(host,datadir,port):
    
    logger.info("Checking if standby master is running on host: %s  in directory: %s" % (host,datadir))
    pidfileexists=FileDirExists.remote('check for standby PID file',host,datadir + '/postmaster.pid')
    lockfileexists=FileDirExists.remote('check for standby lock file',host,"/tmp/.s.PGSQL.%s" % port)
    
    if pidfileexists and lockfileexists:
        raise Exception("Standby instance running. postmaster.pid and /tmp/.s.PGSQL.%s exists" % port)
    
    #TODO: any other cases to handle
        
    cmd=GpStandbyStart.remote('start standby master',host,datadir,port)
    
    logger.debug("Starting standby: %s" % cmd )
    
    #TODO: make sure its started.
    pass


# these match names from gp_bash_functions.sh
def chk_gpdb_id(username):
    path="%s/bin/initdb" % GPHOME
    if not os.access(path,os.X_OK):
        raise GpError("File permission mismatch.  The current user %s does not have sufficient"
                      " privileges to run the Greenplum binaries and management utilities." % username )
    pass        


def chk_local_db_running(datadir,port):
    """Perform a few checks to see if the db is running.  We 1st look at:
       1) /tmp/.s.PGSQL.<PORT> and /tmp/.s.PGSQL.<PORT>.lock
       2) DATADIR/postmaster.pid
       3) netstat 
       
       Returns tuple in format (master_pid_exists, tmpfile_exists, port_active)
    
    """    
    cmd=FileDirExists('check for master PID file',datadir + '/postmaster.pid')
    cmd.run(validateAfter=True)    
    master_pid_exists = cmd.filedir_exists()
    
    
    cmd=FileDirExists('check for /tmp/.s.PGSQL file file',"/tmp/.s.PGSQL.%d" % port)
    cmd.run(validateAfter=True)    
    tmpfile_exists = cmd.filedir_exists()
        
        
    netstat_port_active = PgPortIsActive.local('check netstat for master port',"/tmp/.s.PGSQL.%d" % port, port)

    logger.debug("master_pid_exists: %s tmpfile_exists: %s netstat port: %s" %\
                (master_pid_exists,tmpfile_exists,netstat_port_active))
    
    return (master_pid_exists,tmpfile_exists,netstat_port_active)    
    

def get_local_db_mode(master_data_dir):
    """ Gets the mode Greenplum is running in.
        Possible return values are:
            'NORMAL'
            'RESTRICTED'
            'UTILITY'
    """
    mode = 'NORMAL'
    
    if not os.path.exists(master_data_dir + '/postmaster.pid'):
        raise Exception('Greenplum database appears to be stopped')
    
    try:
        fp = open(master_data_dir + '/postmaster.opts', 'r')
        optline = fp.readline()
        if optline.find('superuser_reserved_connections') > 0:
            mode = 'RESTRICTED'
        elif optline.find('gp_role=utility') > 0:
            mode = 'UTILITY'
    except OSError:
        raise Exception('Failed to open %s.  Is Greenplum Database running?' % master_data_dir + '/postmaster.opts')
    except IOError:
        raise Exception('Failed to read options from %s' % master_data_dir + '/postmaster.opts')
    finally:
        if fp: fp.close()
    
    return mode
