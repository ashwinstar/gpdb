#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
#
# Internal Use Function.
#
#
import os, sys, time

progname = os.path.split(sys.argv[0])[-1]

if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % progname)


from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

import figleaf

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib import gplog
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib import pgconf

description = (""" 
stops a set of one or more segment databases.
""")

_help = ["""
TODO: add txt for gpstop!
"""]

EXECNAME = os.path.split(__file__)[-1]

#-------------------------------------------------------------------------
class SegStopStatus():
    def __init__(self,datadir,stopped,reason):
        self.datadir=datadir
        self.stopped=stopped
        self.reason=reason
    
    def __str__(self):
        return "STATUS--DIR:%s--STARTED:%s--REASON:%s" % (self.datadir,self.stopped,self.reason)
    
#-------------------------------------------------------------------------    
class GpSegStop():
    ######
    def __init__(self,dblist,mode,gpversion):
        self.dblist=dblist
        self.mode=mode
        self.expected_gpversion=gpversion
        self.gphome=os.path.abspath(os.pardir)
        self.actual_gpversion=gp.GpVersion.local('local GP software version check',self.gphome)
        if self.actual_gpversion != self.expected_gpversion:
            raise Exception("Local Software Version does not match what is expected.\n"
                            "The local software version is: '%s'\n"
                            "But we were expecting it to be: '%s'\n"
                            "Please review and correct" % (self.actual_gpversion,self.expected_gpversion))                
        pass
    
    ######
    def stop(self):
        results=[]
        failures=[]
        
        
        for db in self.dblist:
            fields=db.split(':')
            datadir=fields[0]
            port=fields[1]
            cmd=gp.SegmentStop('segment shutdown' , datadir,mode=self.mode)
            cmd.run()
            
            res = cmd.get_results()
            
            if res.rc == 0:
                results.append(SegStopStatus(datadir,True,"Shutdown Succeeded"))                
            else:     
                #read in /tmp/.s.PGXXX file
                name="failed segment '%s'" % db
                (succeeded,mypid,file_datadir)=pg.ReadPostmasterTempFile.local(name,port).getResults()
                
                if succeeded and file_datadir == datadir:
                    if unix.check_pid(mypid):
                        os.kill(mypid,3)  #SIGQUIT
                        time.sleep(5)
                        
                        if unix.check_pid(mypid):
                            os.kill(mypid,6)  #SIGABORT  ? should this be 11 SIGSEGV?                      
                            time.sleep(5)
                        
                        if not unix.check_pid(mypid):
                            lockfile="/tmp/.s.PGSQL.%s" % port    
                            logger.info("Clearing segment instance lock files")        
                            os.remove(lockfile)
                
                results.append(SegStopStatus(datadir,False,"Shutdown failed: rc: %d stdout: %s stderr: %s" % 
                                             (res.rc,res.stdout,res.stderr)))
        
        #Log the results!
        status = '\nCOMMAND RESULTS\n'
        
        for result in results:
            status += str(result) + "\n"
        
        logger.info(status)
        
        return failures
    
    
    ######
    def cleanup(self):
        pass
        
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
    parser.add_option("-D","--db",dest="dblist", action="append", type="string")
    parser.add_option("-V", "--gp-version", dest="gpversion",metavar="GP_VERSION",
                      help="expected software version")
    parser.add_option("-m", "--mode", dest="mode",metavar="<MODE>",
                      help="how to shutdown. modes are smart,fast, or immediate")
    
    parser.set_defaults(verbose=False, filters=[], slice=(None, None))
    
    # Parse the command line arguments
    (options, args) = parser.parse_args()
        
    return options, args



#-------------------------------------------------------------------------
    
def main():
    use_figleaf=os.getenv('USE_FIGLEAF', None)
    
    if use_figleaf:
        figleaf.start()
        
    try:
        gplog.setup_tool_logging(EXECNAME,unix.getLocalHostname(),unix.getUserName(),nonuser=True)
                            
        options, args = parseargs()
        
        if options.verbose:
            gplog.enable_verbose_logging()        
        if options.quiet:
            gplog.quiet_stdout_logging()

        logger.info("Starting gpsegstop with args: %s" % ' '.join(sys.argv[1:]))
        
        gpsegstop = GpSegStop(options.dblist,options.mode,options.gpversion)        
        failures = gpsegstop.stop()
        
        gpsegstop.cleanup()
        if failures:
            sys.exit(1)
        else:    
            sys.exit(0)

    except Exception, e:
        logger.exception("gpsegstop.py failed. (Reason=%s) exiting..." % e)
        sys.exit(3)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if use_figleaf:
            figleaf.stop()
            figleaf.write_coverage(os.path.expanduser("~") + '/.figleaf',append=True)

#------------------------------------------------------------------------- 
logger = gplog.get_default_logger()    
if __name__ == '__main__':
    main()     
