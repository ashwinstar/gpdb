#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved.
#

"""
TODO: docs!
"""
import os, pickle, base64

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
    def __init__(self, name, dataDir, port, dbid, numContentsInCluster, 
                 restrictedMode=False, max_connections=1,
                 disableMasterMirror=False,
                 utilityMode=False, ctxt=LOCAL, remoteHost=None):
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

        cmdfrags=["$GPHOME/bin/pg_ctl -w -D %s -l %s/pg_log/startup.log -o \" %s %s -i -M master -p %s -b %s -C -1 -z %s --silent-mode=true" % \
                  (dataDir,dataDir,standby,gp_mode_flags,port,dbid,numContentsInCluster)]


        if restrictedMode:
            cmdfrags.append(" -c superuser_reserved_connections=%s" % max_connections)


        cmdfrags.append("\" start ")

        cmdStr=" ".join(cmdfrags)

        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name,dataDir,port,dbid, numContentsInCluster, restrictedMode=False,max_connections=1,
                 disableMasterMirror=False,utilityMode=False):
        cmd=MasterStart(name,dataDir,port,dbid, numContentsInCluster, restrictedMode,max_connections,disableMasterMirror,utilityMode)
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
    """
    SegmentStart is used to start a single segment.

    Note: Most code should probably use GpSegStartCmd instead which starts up
    all of the segments on a specified GpHost.
    """

    def __init__(self, name, gpdb, numContentsInCluster, mirroringMode, 
                 utilityMode=False, ctxt=LOCAL, remoteHost=None, noWait=False):

        # This is referenced from calling code
        self.segment = gpdb

        # Interesting data from our input segment
        dbid    = gpdb.getSegmentDbId()
        content = gpdb.getSegmentContentId()
        port    = gpdb.getSegmentPort()
        datadir = gpdb.getSegmentDataDirectory()
        logfile = os.path.join(datadir, 'pg_log', 'startup.log')


        cmdfrags=['$GPHOME/bin/pg_ctl']
        cmdfrags.append('-D %s' % datadir)
        cmdfrags.append('-l %s' % logfile)

        if not noWait:
            cmdfrags.append('-w')

        portStr = ""
        if port:
            portStr = "-p %s" % port

        utilityModeStr = ""
        if ( utilityMode ):
            utilityModeStr = "-c gp_role=utility"

        cmdfrags.append("-o '-i %s %s --silent-mode=true -M %s -b %s -C %s "
                        "-z %s' start 2>&1" %
                        (portStr, utilityModeStr, mirroringMode, dbid,
                         content, numContentsInCluster))
        self.cmdStr=' '.join(cmdfrags)

        Command.__init__(self, name, self.cmdStr, ctxt, remoteHost)

    @staticmethod
    def local(name, gpdb, numContentsInCluster, mirroringMode, utilityMode=False):
        cmd=SegmentStart(name, gpdb, numContentsInCluster, mirroringMode, utilityMode)
        cmd.run(validateAfter=True)

    @staticmethod
    def remote(name, remoteHost, gpdb, numContentsInCluster, mirroringMode, utilityMode=False):
        cmd=SegmentStart(name, gpdb, numContentsInCluster, mirroringMode, utilityMode,
                         ctxt=REMOTE, remoteHost=remoteHost)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class SendFilerepTransitionMessage(Command):
    def __init__(self, name, inputFile, port=None,ctxt=LOCAL, remoteHost=None, dataDir=None):
        if not remoteHost:
            remoteHost = "localhost"
        self.cmdStr='$GPHOME/bin/gp_primarymirror -h %s -p %s -i %s' % (remoteHost,port,inputFile)
        self.dataDir = dataDir
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name,inputFile,port=None,remoteHost=None):
        cmd=SendFilerepTransitionMessage(name, inputFile, port, LOCAL, remoteHost)
        cmd.run(validateAfter=True)
        return cmd

    @staticmethod
    def remote(name,remoteHost,inputFile,port=None):
        cmd=SendFilerepTransitionMessage(name, inputFile, port, REMOTE, remoteHost)
        cmd.run(validateAfter=True)
        return cmd

#-----------------------------------------------
class SegmentStop(Command):
    def __init__(self, name, dataDir,mode='smart', nowait=False, ctxt=LOCAL, remoteHost=None):
        cmdfrags=['$GPHOME/bin/pg_ctl stop -D %s -m %s' % (dataDir,mode)]
        if not nowait:
            cmdfrags.append('-w')
        cmdStr=' '.join(cmdfrags)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name, dataDir,mode='smart'):
        cmd=SegmentStop(name, dataDir,mode)
        cmd.run(validateAfter=True)

#-----------------------------------------------
class GpSegStartCmd(Command):
    def __init__(self, name, gphome, segments, collatelist, gpversion,
                 mirroringMode, numContentsInCluster, verbose=False,
                 ctxt=LOCAL, remoteHost=None):

        # Referenced by calling code, create a clone
        self.dblist = [x for x in segments]

        dirlist=[]
        for seg in segments:
            dirlist.append("-D '%s'" % repr(seg))

        dirstr=" ".join(dirlist)
        colstr=":".join(collatelist)
        if verbose:
            setverbose="-v"
        else:
            setverbose=""

        cmdStr= " ".join([
                "$GPHOME/sbin/gpsegstart.py",
                setverbose,
                "-C %s" % colstr,
                "-M %s" % mirroringMode,
                "-V '%s'" % gpversion,
                "-n %s" % numContentsInCluster,
                dirstr
                ])

        logger.debug(cmdStr)

        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

class GpSegChangeMirrorModeCmd(Command):
    def __init__(self, name, gphome, collatelist, gpversion, dbs, targetMode,
                 transitionMessageFilePath, verbose=False, ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.dblist=dbs
        self.dirlist=[]
        for db in dbs:
            datadir = db.getSegmentDataDirectory()
            port = db.getSegmentPort()
            self.dirlist.append(datadir + ':' + str(port))

        dirstr=" -D ".join(self.dirlist)
        colstr=":".join(collatelist)
        if verbose:
            setverbose=" -v "
        else:
            setverbose=""

        cmdStr="$GPHOME/sbin/gpsegtoprimaryormirror.py %s -D %s -C %s -M %s -t %s -V '%s'" % \
                (setverbose,dirstr,colstr,targetMode,transitionMessageFilePath,gpversion)

        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

#-----------------------------------------------
class GpSegStopCmd(Command):
    def __init__(self, name, gphome, version,mode,dbs,verbose=False, ctxt=LOCAL, remoteHost=None):
        self.gphome=gphome
        self.dblist=dbs
        self.dirportlist=[]
        self.mode=mode
        self.version=version
        for db in dbs:
            datadir = db.getSegmentDataDirectory()
            port = db.getSegmentPort()
            self.dirportlist.append(datadir + ':' + str(port))

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
        self.port=port
        cmdStr="exec $GPHOME/sbin/gpstandbystart.sh %s %d" % (datadir,port)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def remote(name,host,datadir,port):
        cmd=GpStandbyStart(name,datadir,port,ctxt=REMOTE,remoteHost=host)
        cmd.run(validateAfter=True)
        return cmd

    def validate(self, expected_rt=0):
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
class GpDeleteSystem(Command):
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
    def __init__(self, name, masterOnly=False, restricted=False, verbose=False,nostandby=False,ctxt=LOCAL, remoteHost=None, masterDirectory=None):
        self.cmdStr="$GPHOME/bin/gpstart -a"
        if masterOnly:
            self.cmdStr += " -m"
        if restricted:
            self.cmdStr += " -R"
        if verbose:
            self.cmdStr += " -v"
        if nostandby:
            self.cmdStr += " -y"
        if masterDirectory:
            self.cmdStr += " -d " + masterDirectory

        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name,masterOnly=False,restricted=False,verbose=False,nostandby=False,
              masterDirectory=None):
        cmd=NewGpStart(name,masterOnly,restricted,verbose,nostandby,
                       masterDirectory=masterDirectory)
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
class GpCleanSegmentDirectories(Command):
    """
    Clean all of the directories for a set of segments on the host
    """
    def __init__(self, name, segmentsToClean, ctxt, remoteHost):
        pickledSegmentsStr = base64.urlsafe_b64encode(pickle.dumps(segmentsToClean))
        cmdStr = "$GPHOME/bin/lib/gpcleansegmentdir.py -p %s" % pickledSegmentsStr
        Command.__init__(self, name, cmdStr, ctxt, remoteHost)

#-----------------------------------------------
def buildExpansionSegmentInfo(segments):
    """
    Build the new segment info that can be used to get the confinfo argument to pass to ConfigExpansionSeg

    """
    result = {}
    for seg in segments:
        hostname = seg.getSegmentHostName()
        if result.has_key(hostname):
            result[hostname] += ','
        else:
            result[hostname] = ''

        result[hostname] += '%s:%d' %  (seg.getSegmentDataDirectory(), seg.getSegmentPort())
    return result

#-----------------------------------------------
class ConfigExpansionSeg(Command):
    def __init__(self, name, confinfo, newSegments=False, tarFile=None,
                 batchSize=None, verbose=False,ctxt=LOCAL, remoteHost=None):
        cmdStr = '$GPHOME/bin/lib/gpconfexpseg -c \"%s\"' % (confinfo)
        if newSegments:
            cmdStr += ' -n'
        if tarFile:
            cmdStr += ' -t %s' % tarFile
        if verbose:
            cmdStr += ' -v '
        if batchSize:
            cmdStr += ' -B %s' % batchSize

        Command.__init__(self, name, cmdStr, ctxt, remoteHost)

#-----------------------------------------------
class GpVersion(Command):
    def __init__(self,name,gphome,ctxt=LOCAL,remoteHost=None):
        self.gphome=gphome
        self.cmdStr="%s/bin/postgres --gp-version" % gphome
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

    def get_version(self):
        return self.results.stdout.strip()

    @staticmethod
    def local(name,gphome):
        cmd=GpVersion(name,gphome)
        cmd.run(validateAfter=True)
        return cmd.get_version()

#-----------------------------------------------
class GpCatVersion(Command):
    """
    Get the catalog version of the binaries in a given GPHOME
    """
    def __init__(self,name,gphome,ctxt=LOCAL,remoteHost=None):
        self.gphome=gphome
        cmdStr="%s/bin/postgres --catalog-version" % gphome
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    def get_version(self):
        # Version comes out like this:
        #   "Catalog version number:               201002021"
        # We only want the number
        return self.results.stdout.split(':')[1].strip()

    @staticmethod
    def local(name,gphome):
        cmd=GpCatVersion(name,gphome)
        cmd.run(validateAfter=True)
        return cmd.get_version()

#-----------------------------------------------
class GpCatVersionDirectory(Command):
    """
    Get the catalog version of a given database directory
    """
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):
        cmdStr = "$GPHOME/bin/pg_controldata %s" % directory
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    def get_version(self):
        # We got the whole pg_controldata, need to sift through
        # that looking for the catalog version number
        data = {}
        for line in self.results.stdout.split('\n'):
            try:
                (key, value) = line.split(':', 1)
                data[key] = value.strip()
            except:
                pass
        return data['Catalog version number']

    @staticmethod
    def local(name,directory):
        cmd=GpCatVersionDirectory(name,directory)
        cmd.run(validateAfter=True)
        return cmd.get_version()


#-----------------------------------------------
class GpSegCheckLocaleAndConnect(Command):
    def __init__(self,name,datadir,port,expected_lc_collate,expected_lc_monetary,expected_lc_numeric):
        self.datadir=datadir
        self.port=port
        self.expected_lc_collate=expected_lc_collate
        self.expected_lc_monetary=expected_lc_monetary
        self.expected_lc_numeric=expected_lc_numeric
        self.error_str=""
        self.success=False

        Command.__init__(self, name, "GpSegCheckLocaleAndConnect")
        pass


    def run(self,validateAfter=False):

        dburl = dbconn.DbURL(port=self.port,dbname='template1')

        logger.debug("%s connecting to %s" % (self.datadir,dburl))

        conn = None
        while conn is None:
            try:
                conn = dbconn.connect(dburl, utility=True)
            except Exception, ex:
                (exType,exValue,exTraceback) = sys.exc_info()
                # todo MIRRORING: this is bogus because it doesn't handle internationalization.  Should make gpmirrortransition option that just checks if the db has started...then we can wait for it to finish starting
                if str(exValue).strip() == 'FATAL:  the database system is starting up':
                    conn = None
                    time.sleep(1) # wait 1 second to try again
                else:
                    raise ex
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
        seg_datadir = catalog.getSessionGUC(conn, 'data_directory')
        if not os.path.samefile(seg_datadir, self.datadir):
            self.error_str += "Segment's data directory does not match. " \
                              "Expected value %s, " \
                              "Actual value %s" % (self.datadir, seg_datadir)
        conn.close()
        logger.debug("%s connection closed" % self.datadir)

        if len(self.error_str) == 0:
            self.success = True
        else:
            self.sucess = False

    def set_results(self,results):
        raise ExecutionError("TODO:  must implement", self)

    def get_results(self):
        raise ExecutionError("TODO:  must implement", self)

    def was_successful(self):
        raise ExecutionError("TODO:  must implement", self)

    def validate(self, expected_rc=0):
        raise ExecutionError("TODO:  must implement", self)

    def getResults(self):
        return (self.success,self.error_str)


#-----------------------------------------------
class GpSuspendSegmentsOnHost(Command):
    def __init__(self, name, gpconfigstrings, resume, ctxt=LOCAL, remoteHost=None):
        if resume:
            pauseOrResume = "--resume"
        else:
            pauseOrResume = "--pause"

        cmdStr="echo '%s' | $GPHOME/sbin/gpsuspend.py %s" % (gpconfigstrings, pauseOrResume)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

#-----------------------------------------------
class GpAddConfigScript(Command):
    def __init__(self, name, directorystring, entry, value=None, removeonly=False, ctxt=LOCAL, remoteHost=None):
        cmdStr="echo '%s' | $GPHOME/sbin/gpaddconfig.py --entry %s" % (directorystring, entry)
        if value:
            cmdStr = cmdStr + " --value " + value
        if removeonly:
            cmdStr = cmdStr + " --removeonly "

        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-----------------------------------------------
def distribute_tarball(queue,list,tarball):
        logger.debug("distributeTarBall start")
        for db in list:
            hostname = db.getSegmentHostName()
            datadir = db.getSegmentDataDirectory()
            (head,tail)=os.path.split(datadir)
            scp_cmd=RemoteCopy("copy master",tarball,hostname,head)
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
        info_str="found postmaster with pid: %d for datadir: %s still running" % (pid,datadir)
        logger.info(info_str)

        logger.info("attempting to shutdown db with datadir: %s" % datadir )
        cmd=SegmentStop('db shutdown' , datadir,mode='fast')
        cmd.run()

        if check_pid(pid):
            info_str="unable to stop postmaster with pid: %d for datadir: %s still running" % (pid,datadir)
            logger.info(info_str)
            return info_str
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


def pausePg(db):
    """
    This function will pause an instance of postgres which is part of a GPDB

    1) pause the postmaster (this prevents new connections from being made)
    2) get list of processes that are descendent from postmaster process
    3) pause all descendent processes and ignore any failures (a process may have died betwen getting pid list and doing the pauses)
    4) again, get list of processes that are descendent from postmaster process
    5) pause all descendent processes and ignore any failures (do not ignore errors, errors are failures, no pids can die between being paused the first time and getting the pid list)
    """

    datadir = db.getSegmentDataDirectory()
    content = db.getSegmentContentId()
    postmasterPID = read_postmaster_pidfile(datadir)
    if postmasterPID == 0:
        raise Exception, 'print "could not locate postmasterPID during pause'

    Kill.local(name="pausep "+str(content), pid=postmasterPID, signal="STOP")

    decsendentProcessPids = getDescendentProcesses(postmasterPID)

    for killpid in decsendentProcessPids:

        try:
            Kill.local(name="pausep "+str(killpid), pid=killpid, signal="STOP")
        except:
            pass

    decsendentProcessPids = getDescendentProcesses(postmasterPID)
        
    for killpid in decsendentProcessPids:

        Kill.local(name="pausep "+str(killpid), pid=killpid, signal="STOP")

    
def resumePg(db):
    """
    1) resume the processes descendent from the postmaster process
    2) resume the postmaster process
    """

    datadir = db.getSegmentDataDirectory()
    content = db.getSegmentContentId()
    postmasterPID = read_postmaster_pidfile(datadir)
    if postmasterPID == 0:
        raise Exception, 'print "could not locate postmasterPID during resume'

    decsendentProcessPids = getDescendentProcesses(postmasterPID)
        
    for killpid in decsendentProcessPids:

        Kill.local(name="pausep "+str(killpid), pid=killpid, signal="CONT")


    Kill.local(name="pausep "+str(content), pid=postmasterPID, signal="CONT")


def createTempDirectoryName(masterDataDirectory, tempDirPrefix):
    return '%s/%s_%s_%d' % (os.sep.join(os.path.normpath(masterDataDirectory).split(os.sep)[:-1]),
                                tempDirPrefix,
                                datetime.datetime.now().strftime('%m%d%Y'),
                                os.getpid())
