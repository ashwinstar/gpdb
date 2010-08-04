#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

from gppylib.commands.base import *
from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
from time import strftime, sleep
import bisect
import copy
import datetime
import os
import pprint
import signal
import sys
import tempfile
import threading
import traceback



sys.path.append(os.path.join(os.path.dirname(__file__), "../bin"))
sys.path.append(os.path.join(os.path.dirname(__file__), "../bin/lib"))


try:
    from pysync import *
    from gppylib.commands.unix import *
    from gppylib.commands.gp import *
    from gppylib.commands.pg import PgControlData
    from gppylib.gparray import GpArray, get_host_interface
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.gplog import *
    from gppylib.db import dbconn
    from gppylib.db import catalog
    from gppylib.userinput import *
    from pygresql.pgdb import DatabaseError
    from pygresql import pg
    from gppylib.gpcoverage import GpCoverage
except ImportError, e:
    sys.exit('ERROR: Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e))

#
# Constants
#

EXECNAME = os.path.split(__file__)[-1]
FULL_EXECNAME = os.path.abspath( __file__ )

HOME_DIRECTORY = os.path.expanduser("~")

GPADMINLOGS_DIRECTORY = HOME_DIRECTORY + "/gpAdminLogs"

DESCRIPTION = ("""Repair utility to re-sync primary and mirror files.""")

_help  = [""" TODO add help """]

_usage = """ TODO add usage """

TEN_MEG = 10485760
ONE_GIG = 128  * TEN_MEG
TEN_GIG = 1024 * TEN_MEG

PID_FILE = "gprepairmirrorseg.pid"

# Keep the value of the name/value dictionary entry the same length for printing.
categoryAction = {}
categoryAction['ao']      = 'COPY     '
categoryAction['heap:']    = 'COPY     '
categoryAction['btree:']   = 'COPY     '
categoryAction['extra_p:'] = 'COPY     '
categoryAction['extra_m:'] = 'DELETE   '
categoryAction['unknown:'] = 'NO ACTION'


#-------------------------------------------------------------------------------
def prettyPrintFiles(resycFiles):
  
  fileIndex = 0
  fileListLength = len(resyncFile.fileList)
  ACTION      = "ACTION"
  SOURCE_HOST = "SOURCE HOST  "
  SOURCE_FILE = "SOURCE FILE  "
  TARGET_HOST = "TARGET HOST  "
  TARGET_FILE = "TARGET FILE  "
  maxac = len(ACTION)
  maxsh = len(SOURCE_HOST)
  maxsf = len(SOURCE_FILE)
  maxth = len(TARGET_HOST)
  maxtf = len(TARGET_FILE)
  
  for fileIndex in range(fileListLength):
      if maxsh < len(resycFiles.getSourceHost(fileIndex)):
         maxsh = len(resycFiles.getSourceHost(fileIndex))
      if maxsf < len(resycFiles.getSourceFile(fileIndex)):
         maxsf = len(resycFiles.getSourceFile(fileIndex))
      if maxth < len(resycFiles.getTargetHost(fileIndex)):
         maxth = len(resycFiles.getTargetHost(fileIndex))
      if maxtf < len(resycFiles.getTargetFile(fileIndex)):
         maxtf = len(resycFiles.getTargetFile(fileIndex))  
  
  line = ACTION + " " 
  line = line + SOURCE_HOST + " " * (maxsh - len(SOURCE_HOST))
  line = line + SOURCE_FILE + " " * (maxsf - len(SOURCE_FILE))
  line = line + TARGET_HOST + " " * (maxth - len(TARGET_HOST))
  line = line + TARGET_FILE + " " * (maxtf - len(TARGET_FILE))

  logger.info("")
  logger.info(line)
  line = "-" * maxac
  line = line + "-" * maxsh
  line = line + "-" * maxsf
  line = line + "-" * maxth
  line = line + "-" * maxtf
  logger.info(line)
  for fileIndex in range(fileListLength):
      #print ""
      #print "resycFiles.getCategory(fileIndex) =" + str(resycFiles.getCategory(fileIndex))
      #print ""
      line = categoryAction[resycFiles.getCategory(fileIndex)] + " "
      line = line + resycFiles.getSourceHost(fileIndex) 
      line = line + " " * (maxsh - len(resycFiles.getSourceHost(fileIndex)))
      line = line + resycFiles.getSourceFile(fileIndex) 
      line = line + " " * (maxsf - len(resycFiles.getSourceFile(fileIndex)))
      line = resycFiles.getTargetHost(fileIndex) 
      line = line + " " * (maxth - len(resycFiles.getTargetHost(fileIndex)))
      line = resycFiles.getTargetFile(fileIndex) 
      line = line + " " * (maxtf - len(resycFiles.getTargetFile(fileIndex)))           
  logger.info("")


#-------------------------------------------------------------------------------
def sshBusy(cmd):
    """ 
      This function will check the results of a Command to see if ssh was too busy.
      It will return False if the command completed, successfully or not, 
      and a retry is not possible or necessary. 
    """
    retValue = False
    results = cmd.get_results()
    resultStr = results.printResult()
    if results.rc != 0:
       if resultStr.find("ssh_exchange_identification: Connection closed by remote host") != -1:
          retValue = True
       else:
          retValue = False
    else:
       retValue = False
    return retValue



#-------------------------------------------------------------------------------
def runAndCheckCommandComplete(cmd):
    """ 
      This function will run a Command and return False if ssh was too busy.
      It will return True if the command completed, successfully or not, 
      if ssh wasn't busy. 
    """
    retValue = True
    cmd.run(validateAfter = False)
    if sshBusy(cmd) == True:
       """ Couldn't make the connection. put in a delay, and return"""
       self.logger.debug("gprepairmirrorseg ssh is busy... need to retry the command: " + str(cmd))
       time.sleep(1)
       retValue = False
    else:
       retValue = True
    return retValue


#-------------------------------------------------------------------------------                                                             
def parseargs():
    parser = OptParser( option_class = OptChecker
                      , description  = ' '.join(DESCRIPTION.split())
                      , version      = '%prog version $Revision: #12 $'
                      )
    parser.setHelp(_help)
    parser.set_usage('%prog ' + _usage)
    parser.remove_option('-h')

    parser.add_option('-f', '--file', default='',
                      help='the name of a file containing the re-sync file list.')
    parser.add_option('-v','--verbose', action='store_true',
                      help='debug output.', default=False)
    parser.add_option('-h', '-?', '--help', action='help',
                        help='show this help message and exit.', default=False)
    parser.add_option('--usage', action="briefhelp")    

    """
     Parse the command line arguments
    """
    (options, args) = parser.parse_args()

    if len(args) > 0:
        logger.error('Unknown argument %s' % args[0])
        parser.exit()

    options.master_data_directory = get_masterdatadir()

    return options, args

#-------------------------------------------------------------------------------
def sig_handler(sig, arg):
    print "Handling signal..."
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGHUP, signal.SIG_DFL)

    # raise sig
    os.kill(os.getpid(), sig)


#-------------------------------------------------------------------------------
def create_pid_file(info_data_directory):
    """Creates gprepairmirrorseg pid file"""
    try:
        fp = open(info_data_directory + '/' + PID_FILE, 'w')
        fp.write(str(os.getpid()))
    except IOError:
        raise
    finally:
        if fp: fp.close()


#-------------------------------------------------------------------------------
def remove_pid_file(info_data_directory):
    """Removes upgrademirror pid file"""
    try:
        os.unlink(info_data_directory + '/' + PID_FILE)
    except:
        pass


#-------------------------------------------------------------------------------
def is_gprepairmirrorseg_running(info_data_directory):
    """Checks if there is another instance of gprepairmirrorseg running"""
    is_running = False
    try:
        fp = open(info_data_directory + '/' + PID_FILE, 'r')
        pid = int(fp.readline().strip())
        fp.close()
        is_running = check_pid(pid)
    except IOError:
        pass
    except Exception, msg:
        raise

    return is_running


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class InvalidStatusError(Exception): pass


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class ValidationError(Exception): pass


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class GPResyncFile:
    """ 
      This class represents the information stored in the resync file.
      The expected file format is: 
      
        <category> <good-segment-host>:<good-segment-file> <bad segment host>:<bad segment file>

      where <category> is one of:

        ao       - An ao table
        heap     - A heap table
        btree    - A btree
        unknown  - An unknown file type
        extra_p  - An extra file on the primary, which is the same as the <good-segment>
        extra_m  - An extra file on the mirror, which is the same as the <bad-segment>
    """
    

    def __init__(self, filename):
        self.filename = filename
        self.fileList = []
        self.readfile()

    #-------------------------------------------------------------------------------
    def __str__(self):
       tempStr = "self.filename = " + str(self.filename) + '\n'
       return tempStr

    #-------------------------------------------------------------------------------
    def readfile(self):
        try:
            file = None
            file = open(self.filename, 'r')

            for line in file:
                line = line.strip()
                (category, goodseg, badseg) = line.split()
                (goodseghost, goodsegfile) = goodseg.split(":")
                (badseghost, badsegfile) = badseg.split(":")
                self.fileList.append([category, goodseghost, goodsegfile, badseghost, badsegfile])
            file.close()
        except IOError, ioe:
            logger.error("Can not read file %s. Exception: %s" % (self.filename, str(ioe)))
            raise Exception("Unable to read file: %s" % self.filename)
        finally:
            if file != None:
               file.close()

    #-------------------------------------------------------------------------------
    def getEntry(self, index):
        logger.debug("Entering getEntry, index = %s" % str(index))
        return self.fileList[index]


    #-------------------------------------------------------------------------------
    def getCategory(self, index):
        return self.fileList[index][0]

    #-------------------------------------------------------------------------------
    def getSourceHost(self, index):
        return self.fileList[index][1]

    #-------------------------------------------------------------------------------
    def getSourceFile(self, index):
        return self.fileList[index][2]
    
    #-------------------------------------------------------------------------------
    def getTargetHost(self, index):
        return self.fileList[index][3]    

    #-------------------------------------------------------------------------------
    def getTargetFile(self, index):
        return self.fileList[index][4]


#------------------------------------------------------------------------
#-------------------------------------------------------------------------                    
class RemoteCopyPreserve(Command):
    def __init__( self
                , name
                , srcDirectory
                , dstHost
                , dstDirectory
                , ctxt = LOCAL
                , remoteHost = None
                ):
        self.srcDirectory = srcDirectory
        self.dstHost = dstHost
        self.dstDirectory = dstDirectory        
        cmdStr="%s -rp %s %s:%s" % (findCmdInPath('scp'),srcDirectory,dstHost,dstDirectory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class MoveDirectoryContents(Command):
    """ This class moves the contents of a local directory."""

    def __init__( self
                , name
                , srcDirectory
                , dstDirectory
                , ctxt = LOCAL
                , remoteHost = None
                ):
        self.srcDirectory = srcDirectory
        self.srcDirectoryFiles = self.srcDirectory + "." + 'dirfilelist' 
        self.dstDirectory = dstDirectory
        ls = findCmdInPath("ls")
        cat = findCmdInPath("cat")
        xargs = findCmdInPath("xargs")
        mv = findCmdInPath("mv")
        cmdStr = "%s -1 %s > %s" % (ls, self.srcDirectory, self.srcDirectoryFiles)
        cmdStr = cmdStr + ";%s %s" % (cat, self.srcDirectoryFiles)
        cmdStr = cmdStr + " | %s -I xxx %s %s/xxx %s" % (xargs, mv, self.srcDirectory, self.dstDirectory)
        cmdStr = cmdStr + "; rm %s" % (self.srcDirectoryFiles)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class PySyncPlus(PySync):
    """
    This class is really just PySync but it records all the parameters passed in.
    """

    def __init__( self
                , name
                , srcDir
                , dstHost
                , dstDir
                , ctxt = LOCAL
                , remoteHost = None
                , options = None
                ):
        self.namePlus = name
        self.srcDirPlus = srcDir
        self.dstHostPlus = dstHost
        self.dstDirPlus = dstDir
        self.ctxtPlus = ctxt
        self.remoteHostPlus = remoteHost
        self.optionsPlus = options
        PySync.__init__( self
                       , name = name
                       , srcDir = srcDir
                       , dstHost = dstHost
                       , dstDir = dstDir
                       , ctxt = ctxt
                       , remoteHost = remoteHost
                       , options = options 
                       )
        self.destinationHost = dstHost


#-------------------------------------------------------------------------------
#--------------------------------- Main ----------------------------------------
#-------------------------------------------------------------------------------
""" 
   This the the main body of code for gprepairmirrorseg.
"""

coverage = GpCoverage()
coverage.start()

try:
  # setup signal handlers so we can clean up correctly
  signal.signal(signal.SIGTERM, sig_handler)
  signal.signal(signal.SIGHUP, sig_handler)
  
  logger = get_default_logger()
  applicationName = EXECNAME
  setup_tool_logging( appName = applicationName
                    , hostname = getLocalHostname()
                    , userName = getUserName()
                    )

  options, args = parseargs()

  if options.file == None or len(options.file) == 0:
     logger.error('The --file command line argument is required')
     raise Exception("Unable to continue")

  if options.verbose:
     enable_verbose_logging()

  if is_gprepairmirrorseg_running(options.master_data_directory):
     logger.error('gprepairmirrorseg is already running.  Only one instance')
     logger.error('of gprepairmirrorseg is allowed at a time.')
     remove_pid = False
     sys.exit(1)
  else:
     create_pid_file(options.master_data_directory)

  resyncFile = GPResyncFile(options.file)  

  prettyPrintFiles(resyncFile)

  logger.info("gprepairmirror seg will attempt to re-sync the following files:")
  
  fileIndex = 0
  fileListLength = len(resyncFile.fileList)
  
  logger.info("")
  logger.info("source host        source file       target host     target file")
  logger.info("-----------        -----------       -----------     -----------")
  for fileIndex in range(fileListLength):
      logger.info()
                         
     
  sys.exit(0)

except Exception,e:
    logger.error("gprepairmirrorseg failed: %s \n\nExiting..." % str(e) )
    traceback.print_exc()
    sys.exit(3)

except KeyboardInterrupt:
    # Disable SIGINT while we shutdown.
    signal.signal(signal.SIGINT,signal.SIG_IGN)

    # Re-enabled SIGINT
    signal.signal(signal.SIGINT,signal.default_int_handler)

    sys.exit('\nUser Interrupted')

except Exception, e:
  print "FATAL Exception: " + str(e)
  traceback.print_exc()
  sys.exit(1)


finally:
    try:
        if remove_pid:
            remove_pid_file(options.master_data_directory)
    except Exception:
        pass
    logger.info("gprepairmirrorseg exit")

    coverage.stop()
    coverage.generate_report()
    
