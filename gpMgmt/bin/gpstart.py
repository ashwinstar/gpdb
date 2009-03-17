#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

import figleaf

from gppylib.db import dbconn
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.gplog import *
from gppylib.db import dbconn
from gppylib import gparray
from gppylib import userinput
from gppylib.db import catalog
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import base
from gppylib.commands import pg
from gppylib import pgconf

description = (""" 
Starts a GPDB Array.
""")

_help = ["""
TODO: Items""",

"*  _process_segment_start(): better handling of commands with > 1 return codes",

"*  timeout on how long to wait to try and start something",

"* honor the -B flag",

"* implement gp.start_standbymaster",

"* implement gp.chk_gpdb_id",

"* dump more identification in _process_segment_start()"]

EXECNAME = os.path.split(__file__)[-1]

DEFAULT_NUM_WORKERS=64

#---------------------------------------------------------------
class GpStartError(Exception): pass

#---------------------------------------------------------------
class SegStartStatus():
    """Tracks result of trying to start an individual segment database"""
    def __init__(self,db,started=False,reason=None,failedCmd=None):
        self.db=db
        self.started=started
        self.reason=reason
        self.failedCmd=failedCmd
    
    def __str__(self):
        if self.started:
            return "DBID:%d  STARTED" % self.db.dbid
        else:
            return "DBID:%d  FAILED  host:'%s' datadir:'%s' with reason:'%s'" % (self.db.dbid,self.db.hostname,self.db.datadir,self.reason)
        
#---------------------------------------------------------------
class GpStart():
    
    ######
    def __init__(self,restricted,start_standby,trust):
        self.restricted=restricted
        self.start_standby=start_standby
        self.trust=trust
        self.pool = None
        pass
    
    ######
    def prepare(self):
        logger.info("Gathering information and validating the environment...")
        self.gphome=gp.get_gphome()
        self.master_datadir=gp.get_masterdatadir()        
        if not self.trust:                    
            gp.check_permissions()                    
        self._read_postgresqlconf()        
        if not self.trust:
            gp.standby_check(self.master_datadir)                 
        self._check_version()
        self._check_master_running()
        
        if not os.path.exists(self.master_datadir + '/pg_log'):
            os.mkdir(self.master_datadir + '/pg_log')

    
    ######
    def _read_postgresqlconf(self):
        logger.debug("Obtaining master's port from master data directory")
        pgconf_dict = pgconf.readfile(self.master_datadir + "/postgresql.conf")        
        self.port = pgconf_dict.int('port')        
        logger.debug("Read from postgresql.conf port=%s" % self.port)   
        self.max_connections = pgconf_dict.int('max_connections') 
        logger.debug("Read from postgresql.conf max_connections=%s" % self.max_connections)
  
    
    ######
    def _check_version(self):
        if self.trust:
            self.gpversion="no version info"
        else:    
            self.gpversion=gp.GpVersion.local('local GP software version check',self.gphome)
            logger.info("local Greenplum Version: '%s'" % self.gpversion)
  
    
    ######
    def _check_master_running(self):
        if os.path.exists(self.master_datadir + '/postmaster.pid'):
            logger.warning("postmaster.pid file exists on Master, checking if recovery startup required")
            self._recovery_startup()
  
           
    ######
    def _recovery_startup(self):
        logger.info("Commencing recovery startup checks")
        
        lockfile="/tmp/.s.PGSQL.%s" % self.port
        tmpfile_exists = os.path.exists(lockfile)
        
        netstat_port_active = unix.PgPortIsActive.local('check netstat for master port',
                                                        lockfile, self.port)
        
        if tmpfile_exists and netstat_port_active:
            logger.info("Have lock file %s and a process running on port %s" % (lockfile,self.port))
            raise GpStartError("Master instance process running")
        elif tmpfile_exists and not netstat_port_active:
            logger.info("Have lock file %s but no process running on port %s" % (lockfile,self.port))            
        elif not tmpfile_exists and netstat_port_active:
            logger.info("No lock file %s but a process running on port %s" % (lockfile,self.port))
            pass
        elif not tmpfile_exists and not netstat_port_active:
            logger.info("No socket connection or lock file in /tmp found for port=%s" % self.port)
        
        logger.info("No Master instance process, entering recovery startup mode")
        
        if tmpfile_exists:
            logger.info("Clearing Master instance lock files")        
            os.remove(lockfile)
        
        postmaster_pid_file = "%s/postmaster.pid" % self.master_datadir
        if os.path.exists(postmaster_pid_file): 
            logger.info("Clearing Master instance pid file")
            os.remove("%s/postmaster.pid" % self.master_datadir)
        
        self.startMaster()
        
        logger.info("Commencing forced instance shutdown")
        
        gp.GpStop.local("forcing master shutdown", masterOnly=True, fast=True, force=True)
        
    
    
    ######
    def summarize_actions(self):
        logger.info("--------------------------")
        logger.info("Master instance parameters")
        logger.info("--------------------------")
        logger.info("Database                 = %s" % self.gparray.master )
        logger.info("Master Port              = %s" % self.port )
        logger.info("Master directory         = %s" % self.master_datadir )
        
        if self.gparray.standbyMaster:
            if self.start_standby:
                logger.info("Master standby start     = Off")
            else:
                logger.info("Master standby start     = On")
        else:
            logger.warning("Master standby        = Off ")
        
        logger.info("--------------------------------------")
        logger.info("Segment instances that will be started")
        logger.info("--------------------------------------")
        
        format_string=self._get_format_string()
        logger.info(format_string % ("Host","Datadir","Port","Status"))
        for db in self.gparray.getSegDbList():                
            logger.info(format_string % (db.hostname,db.datadir,db.port,db.get_status()))
        
    
    ######
    def _get_format_string(self):
        host_len=0
        dir_len=0
        port_len=0
        for db in self.gparray.getSegDbList():
            if len(db.hostname) > host_len:
                host_len=len(db.hostname)
            if len(db.datadir) > dir_len:
                dir_len=len(db.datadir)
            if len(str(db.port)) > port_len:
                port_len=len(str(db.port))
        
        return "%-" + str(host_len) + "s  %-" + str(dir_len) + "s  %-" + str(port_len) + "s  %s"
    
    
    ######
    def startMaster(self):    
        logger.info("Start Master instance in admin mode")    
        cmd=gp.MasterStart('master in utility mode',self.master_datadir,self.port,utilityMode=True)
        cmd.run()
        
        if cmd.get_results().rc != 0:
            logger.fatal("Failed to start Master instance in admin mode")
            cmd.validate()
        
        logger.info("Obtaining Greenplum Master catalog information")
        
        logger.info("Obtaining Segment details from master...")
        self.dburl = dbconn.DbURL()
        self.conn = dbconn.connect(self.dburl, utility=True)
        rows = dbconn.getConfig(self.conn)
        self.gparray = gparray.GpArray(rows)
        
        logger.debug("Gathering collation settings...")
        (self.lc_collate,self.lc_monetary,self.lc_numeric)=catalog.getCollationSettings(self.conn)
        
        if not self.trust:
            self.ftsmode = catalog.getSessionGUC(self.conn,'gp_fault_action')
            logger.debug("FTS Mode: %s" % self.ftsmode)
        
        self.conn.close()
        pass
    
    
    ######
    def start(self):        
        failed_seg_status = []
        workers=min(len(self.gparray.get_hostlist()),64)
        self.pool = base.WorkerPool(numWorkers=workers)
        
        if os.path.exists(self.master_datadir + "/gpexpand.status") and not self.restricted:            
            raise GpStartError("Found a System Expansion Setup in progress. Please run 'gpexpand --rollback'")
        
        
        self.standby_was_started = self._start_standby()
        
        segs = self.gparray.get_valid_segdbs()
        invalid_segs=self.gparray.get_invalid_segdbs()                                
        hostdict=self._build_hostdict(segs)  
        
        if not self.trust:                                         
            failed_seg_status.extend(self._ping_segment_hosts(hostdict))
                                    
        self._start_segments(hostdict)                
        success_seg_status=self._process_segment_start(hostdict,failed_seg_status)    
        self._print_segment_start(segs,failed_seg_status,success_seg_status)

        self.start_final_master()        

        self.cleanup()
    
    
    
    ######
    def _build_hostdict(self,segs):
        hostcache=gp.readGpHostCache(self.gparray,self.pool)
        hostdict={}
                
        for db in segs:
            if db.hostname in hostcache:
                realhost=hostcache[db.hostname]
            else:
                realhost = db.hostname
                
            if realhost not in hostdict:                
                hostdict[realhost] = [db] 
            else:
                hostdict[realhost].append(db)
        return hostdict

    ######
    def _get_invalid_segments(self):
        invalid_segs = self.gparray.get_invalid_segdbs()
                
        if len(invalid_segs) == 0:
            logger.info("No segments marked as invalid")
        else:
            for db in invalid_segs:
                logger.warning("Skipping segment as its marked invalid. Segment has "
                               "dbid=%s content=%s on host=%s with datadir=%s " %\
                               (db.dbid,db.content,db.hostname,db.datadir) )
        return invalid_segs        
        
    
    ######
    def _start_segments(self,hostdict):
        logger.info("Commencing parallel segment instance startup, please wait...")
        for key in hostdict.iterkeys():
            dblist = hostdict[key]
            
            logger.debug("Dispatching command to start segments on host: %s" % key)
            cmd=gp.GpSegStartCmd("remote segment starts on host '%s'" % key, self.gphome, dblist, 
                                 [self.lc_collate,self.lc_monetary,self.lc_numeric],self.gpversion,self.trust,
                                 ctxt=base.REMOTE, remoteHost=key)
            self.pool.addCommand(cmd)
            
        self.pool.join()
        pass
    
    
    ######
    def _process_segment_start(self,hostdict,failed_seg_status):
        logger.info("Process results...")
        success_seg_status=[]
        cmds=self.pool.getCompletedItems()
        for cmd in cmds:
            if cmd.get_results().rc == 0 or cmd.get_results().rc == 1:                            
                cmdout = cmd.get_results().stdout
                lines=cmdout.split('\n')
                for line in lines:
                    if line.startswith("STATUS"):
                        fields=line.split('--')
                        dir = fields[1].split(':')[1]
                        started = fields[2].split(':')[1]
                        reasonStr = fields[3].split(':')[1]
                        
                        if started.lower() == 'false':
                            success=False
                        else:
                            success=True
                            
                        dbs = hostdict[cmd.remoteHost]
                        for db in dbs:
                            if db.datadir == dir:
                                if success:
                                    success_seg_status.append( SegStartStatus(db,started=True,reason=reasonStr,failedCmd=cmd) )
                                else:
                                    failed_seg_status.append( SegStartStatus(db,started=False,reason=reasonStr,failedCmd=cmd) )                        
            else:
                #TODO:  better handling and formatting.
                logger.error("Failed Executing command: '%s' " % cmd.get_results())
            
                
                    
        self.pool.empty_completed_items()
        return success_seg_status
        
    ######
    def _print_segment_start(self,segs,failed_seg_status,success_seg_status):
        started = len(segs) - len(failed_seg_status)
        logger.info("-------------------------------------")
        if len(failed_seg_status) > 0:
            for failure in failed_seg_status:
                logger.info(failure)
        for stat in success_seg_status:
            logger.debug(stat)            
        logger.info("-------------------------------------")
        logger.info("Successfully started %d of %d segment instances" % (started,len(segs)))

    
    ######
    def _ping_segment_hosts(self,hostdict):
        failedSegs=[]
                                 
        for p in unix.Ping.ping_list(hostdict.keys()):
            self.pool.addCommand(p)
        self.pool.join()        
        cmds=self.pool.getCompletedItems()
        
        
        for cmd in cmds:
            if cmd.get_results().rc != 0:
                host=cmd.hostToPing
                logger.info("Ping to host: '%s' FAILED" % host)
                logger.debug("  ping details: %s" % cmd)
                
                dblist=hostdict[host]
                del hostdict[host]                
                
                alternateHost=None
                
                
                for db in dblist:
                    pingCmd = base.Ping("dbid: %d" % db.dbid,db.hostname)
                    pingCmd.run()
                    if pingCmd.get_results().rc == 0:
                        alternateHost=db.hostname
                        break
                
                if alternateHost:
                    hostdict[alternateHost]=dblist
                else:
                    #no interface to reach any of the segments
                    for db in dblist:
                        s = SegStartStatus(db,started=False,reason="Failed to Ping Host",failedCmd=cmd)
                        failedSegs.append(s)
                    pass
            
        self.pool.empty_completed_items()
    
        return failedSegs
    
    
    ######
    def start_final_master(self):
        logger.info("Starting Master instance %s directory %s" % (self.gparray.master.hostname,self.master_datadir))
        gp.MasterStart.local("Starting Master instance",self.master_datadir,self.port,
                             restrictedMode=self.restricted,max_connections=self.max_connections,
                             disableMasterMirror=(not self.start_standby) )
        
        
        if pg.DbStatus.local('master instance',self.gparray.master):
            logger.info("Command pg_ctl reports Master %s instance active" % self.gparray.master.hostname)
            
            logger.info("Database successfully started with no errors reported")
        else:
            logger.warning("Command pg_ctl reports Master %s on port %d not running" % (self.gparray.master.datadir,self.gparray.master.port)) 
        
        
    ######
    def cleanup(self):
        if self.pool:
            self.pool.haltWork()
    
    
    ######
    def _start_standby(self):
        if self.start_standby and self.gparray.standbyMaster is not None:
            if self.gparray.standbyMaster.valid:
                try:
                    gp.start_standbymaster(host,datadir,port)
                    return True
                except ExecutionError(e):
                    logger.debug("Error occured while starting the standby master: %s" % e)
                    return False
            else:
                logger.info("No Valid standby master to start")
                return False            
        else:
            logger.info("No standby master configured.  skipping...")
            return False
        
    
          
    
    
#----------------------- Command line option parser ----------------------

def parseargs():
    parser = OptParser(option_class=OptChecker,
                description=' '.join(description.split()),
                version='%prog version $Revision: #12 $')
    parser.setHelp(_help)
    parser.set_usage('%prog [--help] [options] ')
    parser.remove_option('-h')
    
    parser.add_option('-h', '-?', '--help', action='help',
                        help='show this help message and exit')
    parser.add_option('-v','--verbose', action='store_true',
                        help='debug output.')
    
    optgrp2 = OptionGroup(parser, 'Logging options: ')
    parser.add_option_group(optgrp2)
    optgrp2.add_option('-a', dest="interactive" , action='store_false',default=True,
                        help="quiet mode, do not log progress to screen"\
                             "[default:- verbose output to screen]")    
                      
    optgrp2.add_option('-q', '--quiet', action='store_true',
                        help='suppress status messages')
    optgrp2.add_option('--trust', action='store_true', help='do not perform any safety checks... no seatbelts')
    
    optgrp4 = OptionGroup(parser, 'Connection options: ')
    parser.add_option_group(optgrp4)
    
    
    optgrp3 = OptionGroup(parser, 'Database startup options: ')
    parser.add_option_group(optgrp3)
    optgrp3.add_option('-m', '--master_only', action='store_true',
                        help='start master instance only in maintenance mode')
    
    optgrp3.add_option('-y', '--no_standby', dest="start_standby", action='store_false',default=True,
                        help='do not start master standby server')
    optgrp3.add_option('-B', '--parallel', type="int", default=1, metavar="<parallel_processes>",
                        help='number of segment hosts to run in parallel. Default is %d' % DEFAULT_NUM_WORKERS)    
    optgrp3.add_option('-R', '--restricted', action='store_true',
                        help='start in restricted mode. Only users with superuser privilege are allowed to connect.')
        
    parser.set_defaults(verbose=False, filters=[], slice=(None, None))
    
    
    # Parse the command line arguments
    (options, args) = parser.parse_args()
    
        
    
    #-n sanity check
    if options.parallel > 128 or options.parallel < 1:
        parser.print_help()
        parser.exit()
        
        
    return options, args



#------------------------------- Mainline --------------------------------
def check_to_continue():    
    msg = "\nContinue with Greenplum instance startup"                
    ans = userinput.ask_yesno(None,msg,'N')    
            
    if not ans:
        logger.info("User abort requested, Exiting...")
        sys.exit(4)
    pass

    
def main():
    use_figleaf=os.getenv('USE_FIGLEAF', None)
    
    if use_figleaf:
        figleaf.start()
        
    gpstart = None
    try:
        setup_tool_logging(EXECNAME,unix.getLocalHostname(),unix.getUserName())
    
        options, args = parseargs()
        
        logger.info("Starting gpstart with args: %s" % ' '.join(sys.argv[1:]))
        
        
        if options.verbose:
            enable_verbose_logging()
        
        if options.quiet:
            quiet_stdout_logging()
    
        gpstart = GpStart(options.restricted,options.start_standby,options.trust)
        gpstart.prepare()
          
        gpstart.startMaster()    
        logger.info("Master Started...")
        
        if not options.master_only:
            gp.GpStop.local("forcing master shutdown", masterOnly=True, fast=True)
        
        if options.interactive:
            gpstart.summarize_actions()
            check_to_continue()
        
        
        if options.master_only:
            pass                        
        else:
            gpstart.start()
        
        sys.exit(0)    
    except base.ExecutionError, e:
        logger.fatal("Error occurred: %s\n Command was: '%s'\n"
                     "rc=%d, stdout='%s', stderr='%s'" %\
                     (e.summary,e.cmd.cmdStr, e.cmd.results.rc, e.cmd.results.stdout,
                     e.cmd.results.stderr ))
        sys.exit(4)
    except Exception, e:        
        if options.verbose:
            logger.exception("gpstart failed.  exiting..." )
        else:
            logger.fatal("gpstart failed. (Reason='%s') exiting..." % e)
        if gpstart:
            gpstart.cleanup()
        sys.exit(3)
    except KeyboardInterrupt:
        if gpstart:
            gpstart.cleanup()        
        sys.exit('\nUser Interrupted')
    finally:
        if use_figleaf:
            figleaf.stop()
            figleaf.write_coverage(os.path.expanduser("~") + '/.figleaf',append=True)



logger = get_default_logger()    
if __name__ == '__main__':
    main()
