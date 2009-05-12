#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

"""
TODO: docs!
"""
import os

from gppylib.gplog import *
from gppylib.db import dbconn
from gppylib.db import catalog
from base import *
from unix import *
import pg
from gppylib import pgconf


logger = get_default_logger()

#TODO:  need a better way of managing environment variables.
GPHOME=os.environ.get('GPHOME')



def check_passive_standby(name,dir):
    path = os.path.join(dir, 'postmaster.opts')
    if os.path.exists(path):
        cmd=FileContainsTerm(name,'gpsync', path)
        cmd.run(validateAfter=False)
        return cmd.contains_term()
    else:
        return 0


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
        self.cmdStr="%s %s %s %s:%s" % (psync_executable,
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
        if disableMasterMirror:
            standby="-y"
        else:
            standby=""
        
        if utilityMode:
            gp_mode_flags=" -c gp_role=utility "
        else:
            gp_mode_flags=" -E "    
            
        cmdfrags=["$GPHOME/bin/pg_ctl -w -D %s  -l %s/pg_log/startup.log -o \" %s %s -i -p %s" %\
                  (dataDir,dataDir,standby,gp_mode_flags,port)]
        
        
        if restrictedMode:
            cmdfrags.append(" -c superuser_reserved_connections=%s" % max_connections)
        
        
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
    def __init__(self,name,dataDir,mode='smart',ctxt=LOCAL,remoteHost=None):
        self.dataDir=dataDir
        cmdStr="$GPHOME/bin/pg_ctl -w -D %s -m %s stop" % (dataDir,mode)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,dataDir):
        cmd=MasterStop(name,dataDir)
        cmd.run(validateAfter=True)
        
        
#-----------------------------------------------        
class SegmentStart(Command):      
    def __init__(self, name, dataDir, utilityMode=False, nowait=False,port=None,ctxt=LOCAL, remoteHost=None):
        self.dataDir=dataDir
        cmdfrags=['$GPHOME/bin/pg_ctl -D %s -l %s/pg_log/startup.log ' % (dataDir,dataDir)]
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
        cmdfrags=['$GPHOME/bin/pg_ctl stop -D %s -m %s' % (dataDir,mode)]           
        cmdStr=' '.join(cmdfrags)    
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name, dataDir,mode='smart'):
        cmd=SegmentStop(name, dataDir,mode)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class GpSegStartCmd(Command):
    def __init__(self, name, gphome, gphost, collatelist, gpversion,verbose=False,ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.gphost=gphost
        self.dblist=gphost.dbs
        self.dirlist=[]
        for db in self.dblist:
            self.dirlist.append(db.datadir)
            
        dirstr=" -D ".join(self.dirlist)        
        colstr=":".join(collatelist)
        if verbose:
            setverbose=" -v "
        else:
            setverbose=""
            
        cmdStr="$GPHOME/sbin/gpsegstart.py %s -D %s -C %s -V '%s'" % (setverbose,dirstr,colstr,gpversion)
        
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)



#-----------------------------------------------
class GpSegStopCmd(Command):
    def __init__(self, name, gphome, gphost, version,mode,verbose=False, ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.gphost=gphost
        self.dblist=gphost.dbs
        self.dirportlist=[]
        self.mode=mode
        self.version=version
        for db in self.dblist:
            self.dirportlist.append(db.datadir + ':' + str(db.port))
            
            
        dirstr=" -D ".join(self.dirportlist)
        if verbose:
            setverbose=" -v "
        else:
            setverbose=""
            
        self.cmdStr="$GPHOME/sbin/gpsegstop.py %s -D %s -m %s -V '%s'"  %\
                        (setverbose,dirstr,mode,version)
                        
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)


#-----------------------------------------------
class GpStandbyStart(Command):
    def __init__(self,name,datadir,port,ctxt=LOCAL, remoteHost=None):
        self.datadir=datadir
        logfile="%s/pg_log/startup.log" % datadir
        cmdStr="exec $GPHOME/bin/gpsyncmaster -D %s -p %d -i  >> %s 2>&1 &" % (datadir,port,logfile)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def remote(name,host,datadir,port):
        cmd=GpStandbyStart(name,datadir,port,ctxt=REMOTE,remoteHost=host)
        cmd.run(validateAfter=True)
        return cmd
    
    def validate(self):
        if self.results.rc != 0:
            raise ExecutionError("non-zero rc: %d" % self.results.rc, self)
        if len(self.results.stderr) != 0:
            raise ExecutionError("messages on stderr: %s" % self.results.stderr, self)
        
#-----------------------------------------------
class GpInitSystem(Command):
    def __init__(self,name,configFile,hostsFile, ctxt=LOCAL, remoteHost=None):
        self.configFile=configFile
        self.hostsFile=hostsFile
        self.cmdStr="$GPHOME/bin/gpinitsystem -a -c %s -h %s" % (configFile,hostsFile)
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)

#-----------------------------------------------
class GpDeleteSystem():
    def __init__(self,name,datadir, ctxt=LOCAL, remoteHost=None):
        self.datadir=datadir
        self.input="y\ny\n"
        self.cmdStr="$GPHOME/bin/gpdeletesystem -d %s -f " % (datadir)
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost,stdin=self.input)

#-----------------------------------------------
class GpStart(Command):
    def __init__(self, name, masterOnly=False, restricted=False, verbose=False,ctxt=LOCAL, remoteHost=None):
        self.cmdStr="$GPHOME/bin/gpstart -a" 
        if masterOnly:
            self.cmdStr += " -m"
        if restricted:
            self.cmdStr += " -R"
        if verbose:
            self.cmdStr += " -v"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False,restricted=False):
        cmd=GpStart(name,masterOnly,restricted)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class NewGpStart(Command):
    def __init__(self, name, masterOnly=False, restricted=False, verbose=False,nostandby=False,ctxt=LOCAL, remoteHost=None):
        self.cmdStr="$GPHOME/bin/gpstart -a" 
        if masterOnly:
            self.cmdStr += " -m"
        if restricted:
            self.cmdStr += " -R"
        if verbose:
            self.cmdStr += " -v"
        if nostandby:
            self.cmdStr += " -y"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False,restricted=False,verbose=False,nostandby=False):
        cmd=NewGpStart(name,masterOnly,restricted,verbose,nostandby)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class NewGpStop(Command):
    def __init__(self, name, masterOnly=False, restart=False, fast=False, force=False, verbose=False, ctxt=LOCAL, remoteHost=None):
        self.cmdStr="$GPHOME/bin/gpstop -a"
        if masterOnly:
            self.cmdStr += " -m"
        if verbose:
            self.cmdStr += " -v"
        if fast:
            self.cmdStr += " -f"
        if restart:
            self.cmdStr += " -r"
        if force:
            self.cmdStr += " -M immediate"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False, restart=False, fast=False, force=False, verbose=False):
        cmd=NewGpStop(name,masterOnly,restart, fast, force, verbose)
        cmd.run(validateAfter=True)
        
#-----------------------------------------------           
class GpStop(Command):
    def __init__(self, name, masterOnly=False, verbose=False, quiet=False, restart=False, fast=False, force=False, datadir=None,ctxt=LOCAL, remoteHost=None):
        self.cmdStr="$GPHOME/bin/gpstop -a"
        if masterOnly:
            self.cmdStr += " -m"
        if restart:
            self.cmdStr += " -r"
        if fast:
            self.cmdStr += " -f"
        if force:
            self.cmdStr += " -M immediate"
        if datadir:
            self.cmdStr += " -d %s" % datadir
        if verbose:
            self.cmdStr += " -v"
        if quiet:
            self.cmdStr += " -q"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def local(name,masterOnly=False, verbose=False, quiet=False,restart=False, fast=False, force=False, datadir=None):
        cmd=GpStop(name,masterOnly,verbose,quiet,restart,fast,force,datadir)
        cmd.run(validateAfter=True)
        return cmd
        
#-----------------------------------------------        
class Psql(Command):
    def __init__(self, name, query, database='template1', utilityMode=False, ctxt=LOCAL, remoteHost=None):
        env = ''
        if utilityMode:
            env = 'PGOPTIONS="-c gp_session_role=utility"'
        cmdStr = '%s $GPHOME/bin/psql -c "%s" %s' % (env, query, database)
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
            cmdStr = "perl -p -i.bak -e 's/^%s[ ]*=[ ]*\\d+/%s=%d/' %s" % (optName, optName, optVal, file)
        elif optType == 'string':
            cmdStr = "perl -i -p -e \"s/^%s[ ]*=[ ]*'[^']*'/%s='%s'/\" %s" % (optName, optName, optVal, file)
        else:
            raise Exception, "Invalid optType for ModifyPostgresqlConfSetting"
        self.cmdStr = cmdStr
        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)

#-----------------------------------------------        
class ConfigExpansionSeg(Command):
    def __init__(self, name, confinfo, newSegments=False, tarFile=None, verbose=False,ctxt=LOCAL, remoteHost=None):
        cmdStr = '$GPHOME/bin/lib/gpconfexpseg -c \"%s\"' % (confinfo)                
        if newSegments:
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
        self.cmdStr="$GPHOME/bin/postgres --gp-version"
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)
    
    def get_version(self):
        return self.results.stdout.strip()

    @staticmethod
    def local(name,gphome):
        cmd=GpVersion(name,gphome)
        cmd.run(validateAfter=True)
        return cmd.get_version()
    
    
#-----------------------------------------------
class GpSegCheckLocaleAndConnect(Command):
    def __init__(self,name,datadir,expected_lc_collate,expected_lc_monetary,expected_lc_numeric):
        self.name=name
        self.datadir=datadir
        self.expected_lc_collate=expected_lc_collate
        self.expected_lc_monetary=expected_lc_monetary
        self.expected_lc_numeric=expected_lc_numeric
        self.error_str=""
        self.success=False
        self.cmdStr="GpSegCheckLocaleAndConnect"
        pass
    
    
    def run(self,validateAfter=False):
        pgconf_dict = pgconf.readfile(self.datadir + "/postgresql.conf")        
        port = pgconf_dict.int('port')
        dburl = dbconn.DbURL(port=port,dbname='template1')
        
        logger.debug("%s connecting to %s" % (self.datadir,dburl))
        conn = dbconn.connect(dburl, utility=True)
            
        (lc_collate,lc_monetary,lc_numeric)=catalog.getCollationSettings(conn)
        if lc_collate != self.expected_lc_collate:                    
            self.error_str+="Segment's value of lc_collate does not match the master.\n" \
                            " Master had value: '%s' while this segment has: '%s'\n" %\
                            (self.expected_lc_collate,lc_collate)                
        if lc_monetary != self.expected_lc_monetary:
            self.error_str += "Segment's value of lc_monetary does not match the master.\n"\
                              " Master had value: '%s' while this segment has: '%s'" %\
                            (self.expected_lc_monetary,lc_monetary)
        if lc_numeric != self.expected_lc_numeric:
            self.error_str += "Segment's value of lc_numeric does not match the master.\n" \
                              " Master had value: '%s' while this segment has: '%s'" %\
                              (self.expected_lc_numeric,lc_numeric)
                
        conn.close()
        logger.debug("%s connection closed" % self.datadir)
        
        if len(self.error_str) == 0:
            self.success = True
        else:
            self.sucess = False
    
    def set_results(self,results):
        raise ExecutionError("TODO:  must implement")
    
    def get_results(self):
        raise ExecutionError("TODO:  must implement")
    
    def was_successful(self):
        raise ExecutionError("TODO:  must implement")
    
    def validate(self):
        raise ExecutionError("TODO:  must implement")      

    def getResults(self):
        return (self.success,self.error_str)

        

#-----------------------------------------------        
def distribute_tarball(queue,list,tarball):
        logger.debug("distributeTarBall start")        
        for db in list:
            (head,tail)=os.path.split(db.datadir)
            scp_cmd=RemoteCopy("copy master",tarball,db.get_hostname(),head)            
            queue.addCommand(scp_cmd)
        queue.join()        
        queue.check_results()        
        logger.debug("distributeTarBall finished")




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
    logger.info("Starting standby master")
    
    logger.info("Checking if standby master is running on host: %s  in directory: %s" % (host,datadir))
    res = recovery_startup(datadir)
    
    if res:
        logger.warning("Unable to cleanup previously started standby: '%s'" % res)
        
    #create a pg_log directory if necessary
    CreateDirIfNecessary.remote('create standby logdir if needed', host, datadir + "/pg_log")
    
        
    cmd=GpStandbyStart.remote('start standby master',host,datadir,port)    
    logger.debug("Starting standby: %s" % cmd )
    
    logger.debug("Starting standby master results: %s" % cmd.get_results() )
    
    if cmd.get_results().rc == 0:
        return True
    else:
        return False
    
    pass

######
def recovery_startup(datadir):
    """ investigate a db that may still be running """
        
    pid=read_postmaster_pidfile(datadir)
    if check_pid(pid):
        str="found postmaster with pid: %d for datadir: %s still running" % (pid,datadir)
        logger.info(str)
        
        logger.info("attempting to shutdown db with datadir: %s" % datadir )
        cmd=SegmentStop('db shutdown' , datadir,mode='fast')
        cmd.run()
        
        if check_pid(pid):
            str="unable to stop postmaster with pid: %d for datadir: %s still running" % (pid,datadir)
            logger.info(str)            
            return str
        else:
            logger.info("shutdown of db successful with datadir: %s" % datadir)
            return None
    else:
        #if we get this far it means we don't have a pid and need to do some cleanup.
        pgconf_dict = pgconf.readfile(datadir + "/postgresql.conf")        
        port = pgconf_dict.int('port')  
        
        lockfile="/tmp/.s.PGSQL.%s" % port
        tmpfile_exists = os.path.exists(lockfile)
        
        
        logger.info("No db instance process, entering recovery startup mode")
        
        if tmpfile_exists:
            logger.info("Clearing db instance lock files")        
            os.remove(lockfile)
        
        postmaster_pid_file = "%s/postmaster.pid" % datadir
        if os.path.exists(postmaster_pid_file): 
            logger.info("Clearing db instance pid file")
            os.remove("%s/postmaster.pid" % datadir)
    
        return None


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

######
def read_postmaster_pidfile(datadir):
    pid=0
    f = None
    try:        
        f = open(datadir + '/postmaster.pid')
        pid = int(f.readline().strip())
    except Exception:
        pass                
    finally:
        if f: f.close()
    return pid
            
        
        
        
    f = open(file,'r')
    pid = int(f.readline().strip())
    f.close()
    return pid
    
