#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
#
# Internal Use Function.
#
import os, sys

progname = os.path.split(sys.argv[0])[-1]

if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % progname)


from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

import figleaf

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib import gplog
from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib import pgconf


description = (""" 
This utility is NOT SUPPORTED and is for internal-use only.

Starts a set of one or more segment databases.
""")

_help = ["""
Utility should only be used by other GP utilities.  

Return codes:
  0 - All segments started successfully
  1 - At least one segment didn't start successfully
  2 - Fatal error to script during startup.  unknown state.

"""]

EXECNAME = os.path.split(__file__)[-1]

#-------------------------------------------------------------------------
class SegStartStatus():
    def __init__(self,datadir,started,reason):
        self.datadir=datadir
        self.started=started
        self.reason=reason
    
    def __str__(self):
        return "STATUS--DIR:%s--STARTED:%s--REASON:%s" % (self.datadir,self.started,self.reason)


#-------------------------------------------------------------------------    
class GpSegStart:
    ######
    def __init__(self,dblist,gpversion,collation,mirroringMode):
        self.dblist=dblist
        self.mirroringMode = mirroringMode
        self.expected_gpversion=gpversion
        # There is an issue with python 2.6.2 on linux that can cause start to 
        # hang if it is multithreaded.
        if unix.curr_platform == unix.LINUX:
            self.pool = base.WorkerPool(numWorkers=1)
        else:
            self.pool = base.WorkerPool(numWorkers=len(dblist))
        self.expected_lc_collate=None
        self.expected_lc_monetary=None
        self.expected_lc_numeric=None

        self.gphome=os.path.abspath(os.pardir)
        
        self.actual_gpversion=gp.GpVersion.local('local GP software version check',self.gphome)
        if self.actual_gpversion != self.expected_gpversion:
            raise Exception("Local Software Version does not match what is expected.\n"
                            "The local software version is: '%s'\n"
                            "But we were expecting it to be: '%s'\n"
                            "Please review and correct" % (self.actual_gpversion,self.expected_gpversion))
                            
        
        collation_strings=collation.split(':')
        if len(collation_strings) != 3:
            raise Exception("Invalid collation string specified!")
        (self.expected_lc_collate,self.expected_lc_monetary,self.expected_lc_numeric)=collation_strings
        pass

    ######
    def start(self): 
        results=[]
        failures=False
        directoryToSegStartDataMap = {}
        
        if self.mirroringMode == 'quiescent':
            nowait = True
        else:
            nowait = False

        for itm in self.dblist:
            (segDataDir, segPort, segDbId, segContentId, segNumContentsInCluster) = itm.split(':')
            seg = gp.SegmentToStart(segDataDir, segPort, segDbId, segContentId, segNumContentsInCluster,
                                    self.mirroringMode, nowait)
            directoryToSegStartDataMap[seg.dataDir] = seg

        # validate that the directories all exist and run recovery startup if needed
        for dir in directoryToSegStartDataMap.keys():
            if os.path.isdir(dir):
                if not os.path.exists(dir + '/pg_log'):
                    os.mkdir(dir + '/pg_log')
                    
                if os.path.exists(dir + '/postmaster.pid'):
                    logger.warning("postmaster.pid file exists, checking if recovery startup required")
                    res = gp.recovery_startup(dir) # todo MIRRORING -- pass dbId, contentId, etc into recovery startup?
                    if res:
                        results.append(SegStartStatus(dir,False,res))
                        del directoryToSegStartDataMap[dir]
                        failures=True                                              
            else:
                logger.info("Segment data directory does not exist for: '%s'" % dir)
                results.append(SegStartStatus(dir,False,"segment data directory does not exist"))
                del directoryToSegStartDataMap[dir]
                failures=True      
        
        # now do the start
        logger.info("Starting segments...")       
        for dir, segmentToStart in directoryToSegStartDataMap.iteritems():
            cmd=gp.SegmentStart("Starting seg at dir %s" % dir, segmentToStart)
            self.pool.addCommand(cmd)  
              
        self.pool.join()
        
        cmds=self.pool.getCompletedItems()
        for cmd in cmds:
            res=cmd.get_results()
            if res.rc != 0:
                #TODO: read in last entries in startup.log
                status=SegStartStatus(cmd.segmentToStart.dataDir,False,"PG_CTL failed.\nstdout:%s\nstderr:%s\n" % (res.stdout,res.stderr))
                results.append(status)
                del directoryToSegStartDataMap[cmd.segmentToStart.dataDir]
                failures=True
        
        self.pool.empty_completed_items()
    
        if self.mirroringMode == 'quiescent' or self.mirroringMode == 'mirror':
            # todo MIRRORING: figure out a way to validate even in quiescent or mirror mode -- 
            # we are currently not waiting at all when starting in quiescent mode
            for datadir,segmentToStart in directoryToSegStartDataMap.iteritems():
                results.append(SegStartStatus(datadir,True,"Start Succeeded"))
        else:
            logger.info("Validating segments...")
            for datadir,segmentToStart in directoryToSegStartDataMap.iteritems():
                cmd = gp.GpSegCheckLocaleAndConnect('check on %s' % datadir,
                                                datadir,
                                                segmentToStart.port,
                                                self.expected_lc_collate,
                                                self.expected_lc_monetary,
                                                self.expected_lc_numeric)
                self.pool.addCommand(cmd)
            
            self.pool.join()
        
            cmds=self.pool.getCompletedItems()
            for cmd in cmds:
                (success,error_str)=cmd.getResults()
            
                if success:
                    results.append(SegStartStatus(cmd.dataDir,True,"Start Succeeded"))
                else:
                    results.append(SegStartStatus(cmd.dataDir,False,error_str))
    
        
        #Log the results!
        status = '\nCOMMAND RESULTS\n'
        
        for result in results:
            status += str(result) + "\n"
        
        logger.info(status)
        
        return failures
    
    def cleanup(self):
        if self.pool:
            self.pool.haltWork()
    
#-------------------------------------------------------------------------
def parseargs():
    parser = OptParser(option_class=OptChecker,
                description=' '.join(description.split()),
                version='%prog version $Revision: #12 $')
    parser.setHelp(_help)
    parser.set_usage('%prog [--help] [options] ')
    parser.remove_option('-h')
    
    parser.add_option('-h', '-?', '--help', action='help',help='show this help message and exit')
    parser.add_option('-v','--verbose', action='store_true',help='debug output.')
    parser.add_option('-q', '--quiet', action='store_true',help='suppress status messages')
    parser.add_option("-C", "--collation", type="string", 
                        help="values for lc_collate, lc_monetary, lc_numeric separated by :")
    parser.add_option("-D","--dblist",dest="dblist", action="append", type="string")
    parser.add_option("-M","--mirroringmode",dest="mirroringMode", type="string")
    parser.add_option("-V", "--gp-version", dest="gpversion",metavar="GP_VERSION",
                        help="expected software version")      

    parser.set_defaults(verbose=False, filters=[], slice=(None, None))
    
    # Parse the command line arguments
    (options, args) = parser.parse_args()
        
    return options, args



#-------------------------------------------------------------------------
    
def main():
    use_figleaf=os.getenv('USE_FIGLEAF', None)
    
    if use_figleaf:
        figleaf.start()
    gpsegstart=None
    
    try:
        gplog.setup_tool_logging(EXECNAME,unix.getLocalHostname(),unix.getUserName(),nonuser=True)
        
        options, args = parseargs()
        
        if options.verbose:
            gplog.enable_verbose_logging()        
        if options.quiet:
            gplog.quiet_stdout_logging()

        logger.info("Starting gpsegstart with args: %s" % ' '.join(sys.argv[1:]))
        
        logger.info("options.gpversion: %s" % options.gpversion )
        
        gpsegstart = GpSegStart(options.dblist,options.gpversion,options.collation,options.mirroringMode)
        
        failures = gpsegstart.start()
        
        
        if failures:
            sys.exit(1)
        else:    
            sys.exit(0)

    except Exception, e:
        logger.exception("gpsegstart.py failed. (Reason=%s) exiting..." % e)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if gpsegstart:
            gpsegstart.cleanup()
        if use_figleaf:
            figleaf.stop()
            figleaf.write_coverage(os.path.expanduser("~") + '/.figleaf',append=True)


#------------------------------------------------------------------------- 
logger = gplog.get_default_logger()    
if __name__ == '__main__':
    main()

 
