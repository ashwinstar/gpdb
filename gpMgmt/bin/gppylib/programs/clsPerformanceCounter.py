#!/usr/bin/env python
#
# Copyright (c) EMC Inc 2012. All Rights Reserved.
#
# 
# Used to manage performance counters
#

#
# THIS IMPORT MUST COME FIRST
#
# import mainUtils FIRST to get python version check
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.utils import toNonNoneString
from gppylib import gplog
from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib.system import configurationInterface, fileSystemInterface, osInterface
from gppylib import pgconf
from gppylib import userinput
from gppylib.testold.testUtils import testOutput
from gppylib.system.environment import GpMasterEnvironment

import shutil

#                                                                                                                                           
# Constants                                                                                                                                 
#             
gather_dir = "performanceCountersDir"
                                                                                                                            
segment_performance_counters_dir = "gp_performance_counters"

does_performance_table_exist_query = """SELECT 1 FROM performance_counter_schema.gp_performance_counter"""

create_performance_counter_name_space = """ CREATE SCHEMA performance_counter_schema """

create_performance_table_query = """CREATE TABLE performance_counter_schema.gp_performance_counter (
    process_id int
  , seg_id int
  , session_id int
  , command_id int
  , query_string varchar
  , performance_counter_id int
  , query_start_time bigint
  , query_end_time bigint
  , pc_time bigint
  , pc_cpu_ticks bigint
  , measurements int
) distributed by (seg_id);
"""

#
# Globals
#
logger = gplog.get_default_logger()

#-------------------------------------------------------------------------
class GpPerformanceCounterProgram:
    #
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.options = options
        self.gpArray = None
        self.master_temp_dir = None
        self.problems = False

    def setupSQLCopyText(self):     
        allData = os.path.join(self.master_temp_dir, "allData")  
        self.copy_performance_counter_query = """COPY performance_counter_schema.gp_performance_counter 
          (
            process_id
          , seg_id
          , session_id 
          , command_id 
          , query_string 
          , performance_counter_id
          , query_start_time 
          , query_end_time 
          , pc_time 
          , pc_cpu_ticks 
          , measurements 
          )
          FROM '""" + allData + """' WITH CSV delimiter '|' QUOTE AS '"' """

    #
    # Get gparray info.
    #
    def setupGpArray(self):
        masterPort = self.options.masterPort
        if masterPort is None:
            gpEnv = GpMasterEnvironment(self.options.masterDataDirectory, False)
            masterPort = gpEnv.getMasterPort()
        conf = configurationInterface.getConfigurationProvider().initializeProvider(masterPort)
        self.gpArray = conf.loadSystemConfig(useUtilityMode=True)


    #
    # Build the performance counter message.  Performance counter options themselves will NOT be validated by the
    # client. The server will do that when we send the message.
    #
    def buildMessage(self) :

        # note that we don't validate these strings -- if they contain newlines
        # (and so mess up the protocol) then the server will error
        result = ["PerformanceCounterAction"]
        result.append( toNonNoneString(self.options.performanceCounterName))
        result.append( toNonNoneString(self.options.type))
        result.append( toNonNoneString(self.options.sleepTimeSeconds))
        return '\n'.join(result)

    #
    # build a message that will get status of the performance counters
    #
    def buildGetStatusMessage(self) :
        # note that we don't validate this string then the server may error
        result = ["getPerformanceCounterStatus"]
        result.append( toNonNoneString(self.options.performanceCounterName))
        return '\n'.join(result)

    #
    # return True if the segment matches the given role, False otherwise
    #
    def isMatchingRole(self, role, segment):
        segmentRole = segment.getSegmentRole()
        if role == "primary":
            return segmentRole == 'p'
        elif role == "mirror":
            return segmentRole == 'm'
        elif role == "primary_mirror":
            return segmentRole == 'm' or segmentRole == 'p'
        else:
            raise ProgramArgumentValidationException("Invalid role specified: %s" % role)

    #
    # load the segments and filter to the ones we should target
    #
    def loadTargetSegments(self) :

        targetHost = self.options.targetHost
        targetRole = self.options.targetRole
        targetDbId = self.options.targetDbId

        if targetHost is None and targetDbId is None:
            raise ProgramArgumentValidationException(\
                            "neither --host nor --seg_dbid specified.  " \
                            "Exactly one should be specified.")
        if targetHost is not None and targetDbId is not None:
            raise ProgramArgumentValidationException(\
                            "both --host nor --seg_dbid specified.  " \
                            "Exactly one should be specified.")
        if targetHost is not None and targetRole is None:
            raise ProgramArgumentValidationException(\
                            "--role not specified when --host is specified.  " \
                            "Role is required when targeting a host.")
        if targetDbId is not None and targetRole is not None:
            raise ProgramArgumentValidationException(\
                            "--role specified when --seg_dbid is specified.  " \
                            "Role should not be specified when targeting a single dbid.")

        #
        # load from master db
        #
        self.setupGpArray()
        segments = self.gpArray.getDbList()
        
        #
        # prune gpArray according to filter settings
        #
        
        includeMaster = False
        if (targetHost == "ALL"):
            includeMaster = True
        segments = [seg for seg in segments if (seg.isSegmentQE() or includeMaster == True)]
        if targetHost is not None and targetHost != "ALL":
            segments = [seg for seg in segments if seg.getSegmentHostName() == targetHost]

        if targetDbId is not None:
            segments = self.gpArray.getDbList()
            dbId = int(targetDbId)
            segments = [seg for seg in segments if seg.getSegmentDbId() == dbId]

        if targetRole is not None:
            segments = [seg for seg in segments if self.isMatchingRole(targetRole, seg)]

        # only DOWN segments remaining?  Error out
        downSegments = [seg for seg in segments if seg.getSegmentStatus() != 'u']
        if len(downSegments) > 0:
            downSegStr = "\n     Down Segment: "
            raise ExceptionNoStackTraceNeeded(
                "Unable to inject performance counter.  At least one segment is marked as down in the database.%s%s" %
                (downSegStr, downSegStr.join([str(downSeg) for downSeg in downSegments])))

        return segments

    #
    # write string to a temporary file that will be deleted on completion
    #
    def writeToTempFile(self, str):
        inputFile = fileSystemInterface.getFileSystemProvider().createNamedTemporaryFile()
        inputFile.write(str)
        inputFile.flush()
        return inputFile

    #
    # Send the messages.
    #
    def PerformanceCountersSendMessage(self, segments, messageText):

        inputFile = self.writeToTempFile(messageText)
        logger.info("Sending performance counter message on %d segment(s)", len(segments))
        testOutput("Sending performance counter message on %d segment(s)" % len(segments))

        # run the command in serial to each target
        for segment in segments :
            # if there is an error then an exception is raised by command execution
            cmd = gp.SendSegmentTransitionMessage("Performance Counter", inputFile.name, \
                                        segment.getSegmentPort(), base.LOCAL, segment.getSegmentHostName())
            logger.info("Sending performance counter message on %s, message: %s", segment, messageText)
            cmd.run(validateAfter=False)


            # validate ourselves
            if cmd.results.rc != 0:
                raise ExceptionNoStackTraceNeeded("Send performance counter message: %s" % cmd.results.stderr)
            elif self.options.type == "status":
                # server side prints nice success messages on status...so print it
                str = cmd.results.stderr
                if str.startswith("Success: "):
                    str = str.replace("Success: ", "", 1)
                logger.info("%s", str)
        inputFile.close()


    #
    # Gather all the performance counter files from all the segments,
    # and insert the data in the performance counter table.
    #
    def doGather(self):
        
        # Make the directory on the master to hold the data.
        if os.path.exists(self.master_temp_dir) == True:
            logger.warn("As part of the gather process, the gpperformancecounter utility will remove the directory " \
                        + self.master_temp_dir + " and all of its contents.")
            if not userinput.ask_yesno(None, "Are you sure you want to proceed?", 'N'):
                logger.info("User Aborted doGather")
                return
        unix.RemoveFiles.local("remove temp dir", self.master_temp_dir)
        os.mkdir(self.master_temp_dir)
        
        self.setupGpArray()
        
        dbList = self.gpArray.getDbList()
        masterDb = self.gpArray.master
        masterAddress = masterDb.getSegmentAddress()
        
        logger.info("Preparing to gather performance counter information from all segments")
        
        # Copy all the segment's data to the master.
        for db in dbList:
            dbId      = db.getSegmentDbId()
            dbAddress = db.getSegmentAddress()
            dbDataDir = db.getSegmentDataDirectory()
            segTempDir = os.path.join(self.master_temp_dir, str(dbId))
            os.mkdir(segTempDir)
            copyCmd = unix.RemoteCopy( name = "copy seg " + str(dbId) + " to master"
                                , srcDirectory = dbDataDir + "/gp_performance_counters/"
                                , dstHost = masterAddress
                                , dstDirectory = segTempDir
                                , remoteHost = dbAddress
                                , ctxt = base.REMOTE
                                )
            try:
                copyCmd.run(validateAfter=True)
            except Exception, e:
                logger.warn("There was an issue with copying data from the segments to the master: " + str(e))
                self.problems = True
            
        # Consolidate all the segment data into a single file.
        fileListCommand = unix.FilesInDir(name = "list of all files", filePattern = self.master_temp_dir)
        fileListCommand.run(validateAfter=True)
        fileList = fileListCommand.get_result_list()
        totalFile = open(os.path.join(self.master_temp_dir, "allData"), "wb")
        for file in fileList:
            fd = open(file, 'rb')
            shutil.copyfileobj(fd, totalFile)
            fd.close()
        totalFile.close()

        # Load the data into the database.
        try:
            dbUrl = dbconn.DbURL()
            conn = dbconn.connect(dburl=dbUrl)    
            try:
                cur = dbconn.execSQL(conn, create_performance_counter_name_space)
            except Exception, e:
                """ Namespace probably already exists. Just ignore the error """
                conn.commit()
            conn.commit()
            try:
                cur = dbconn.execSQL(conn, does_performance_table_exist_query)
            except:
                conn.commit()
                cur = dbconn.execSQL(conn, create_performance_table_query)
                conn.commit()
            conn.commit()
            dbconn.execSQL(conn, self.copy_performance_counter_query)
            conn.commit()
            conn.close()
            logger.info("Performance counter information gather from all segments complete.")
            logger.info("To avoid gathering duplicate information in the future, ")
            logger.info("gpperformancecounter will now remove all remove all performance counter information on all segments.")
            self.doClean()
        except Exception, e:
            print("Exception: " + str(e))


    #
    # Remove any existing performance data from the segments.
    #        
    def doClean(self):

        remoteHostList = []
        directoryList  = []
        
        self.setupGpArray()        

        dbList = self.gpArray.getDbList()
        masterDb = self.gpArray.master
        masterAddress = masterDb.getSegmentAddress()
        for db in dbList:
            dbAddress = db.getSegmentAddress()
            dbDataDir = db.getSegmentDataDirectory()   
            remoteHostList.append(dbAddress)
            directoryList.append(os.path.join(dbDataDir, segment_performance_counters_dir))     
        
        warnString = "Preparing to remove performance counter information from all segments. \n"
        for i in range(len(dbList)):
            warnString = warnString + "  " + remoteHostList[i] + ":" + directoryList[i] + "\n"
        logger.warn(warnString)
            
        if not userinput.ask_yesno(None, "Are you sure you want to remove performance counter information from all segments?", 'N'):
            logger.info("User Aborted doClean")
            return
        
        for i in range(len(dbList)):
            rmCmd = unix.RemoveFiles( name = "remove segment performance counter files"
                                    , remoteHost = remoteHostList[i]
                                    , directory = directoryList[i]
                                    , ctxt = base.REMOTE
                                    )
            try:
                rmCmd.run(validateAfter=True)
            except Exception, e:
                logger.warn("There was a problem with removing performance counter files: " + str(e))
                self.problems = True
            logger.info("Creating directory " + remoteHostList[i] + ":" + directoryList[i])
            makedirCmd = unix.MakeDirectory( name = "make performance counter directory"
                                           , directory = directoryList[i]
                                           , mode = "777"
                                           , remoteHost = remoteHostList[i]
                                           , ctxt = base.REMOTE
                                           )
            try:
                makedirCmd.run(validateAfter=True) 
            except Exception, e:
                log.warn("There was a problem with re-creating the performance counter directories: " + str(e))
                self.problems = True
        logger.info("Performance counter information removal from all segments complete.") 

    #
    # Run command.
    #
    def run(self):

        if self.options.masterPort is not None and self.options.masterDataDirectory is not None:
            raise ProgramArgumentValidationException("both master port and master data directory options specified;" \
                    " at most one should be specified, or specify none to use MASTER_DATA_DIRECTORY environment variable")

        if isinstance(unix.SYSTEM, unix.LinuxPlatform) == False and isinstance(unix.SYSTEM, unix.DarwinPlatform) == False:
            raise ProgramArgumentValidationException("Performance Counters are only valid on Linux or OSX platforms.")

        self.master_temp_dir = os.path.join(self.options.tempdir, gather_dir)
        self.setupSQLCopyText()

        if self.options.gather == True:
            self.doGather()
        elif self.options.clean == True:
            self.doClean()
        else:
            messageText = self.buildMessage()
            segments = self.loadTargetSegments()

            self.PerformanceCountersSendMessage(segments, messageText)

        if self.problems == True:
            logger.warn("There were issues with executing gpperformancecounters. Please see the log file for details.")
            
        logger.info("DONE")
        return 0 # success -- exit code 0!

    def cleanup(self):
        pass

    #
    # Create the parser for the options.
    #
    @staticmethod
    def createParser():
        description = ("""This utility is for internal-use only. It is used to manage performance counters.""")

        help = ["""
        Return codes:
          0 - Performance Counter message handled
          non-zero: Error or invalid options
        """]

        parser = OptParser(option_class=OptChecker,
                    description='  '.join(description.split()),
                    version='%prog version $Revision: #18 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, False)

        # these options are used to determine the target segments
        addTo = OptionGroup(parser, 'Target Segment Options: ')
        parser.add_option_group(addTo)
        addTo.add_option('-r', '--role', dest="targetRole", type='string', metavar="<role>",
                         help="Role of segments to target: primary, mirror, or primary_mirror")
        addTo.add_option("-s", "--seg_dbid", dest="targetDbId", type="string", metavar="<dbid>",
                         help="The segment  dbid on which performance counter should be set.")
        addTo.add_option("-H", "--host", dest="targetHost", type="string", metavar="<host>",
                         help="The hostname on which performance counter should be set and triggered; pass ALL to target all hosts")

        addTo = OptionGroup(parser, 'Master Connection Options')
        parser.add_option_group(addTo)

        addMasterDirectoryOptionForSingleClusterProgram(addTo)
        addTo.add_option("-p", "--master_port", dest="masterPort",  type="int", default=None,
                         metavar="<masterPort>",
                         help="DEPRECATED, use MASTER_DATA_DIRECTORY environment variable or -d option.  " \
                         "The port number of the master database on localhost, " \
                         "used to fetch the segment configuration.")

        # these options are used to build the message for the segments
        addTo = OptionGroup(parser, 'Performance Counter Options: ')
        parser.add_option_group(addTo)
        addTo.add_option('-y','--type', dest="type", type='string', metavar="<type>",
                         help="Performance counter type: \n" \
                            "activate (begin collecting data for the performance counter), \n" \
                            "deactivate (stop collecting data for the performance counter), \n" \
                            "status (get current status of a performance counter (i.e. active, not active)) \n")
        addTo.add_option("-z", "--sleep_time_s", dest="sleepTimeSeconds", type="int", default="0" ,
                            metavar="<sleepTime>",
                            help="In addition to recording performance counter information, " \
                                "an artificial delay can be introduced each time a performance " \
                                "counter block is started. <sleepTime> is specified in seconds. " \
                                "The default is 0 seconds. " \
				                "Min Max Range is [0, 7200 sec] ")
        addTo.add_option('-f','--performance_counter_name', dest="performanceCounterName", type='string', metavar="<name>",
                         help="A performance counter name: \n" \
                  "performance_counter_LWLockAcquire (the LWLockAcquire function), \n" \
                  "performance_counter_AOtableInput (the appendonly_insert and appendonly_insert_finish functions),\n " \
			      "performance_counter_All (affects all performance counters')") 
        addTo.add_option('--gather', dest='gather', action='store_true',
                         help="All performance counter data will be gathered from each segment, " \
                              "and placed in a table named 'gp_performance_counter' in the 'gp_performance_counter_schema' namespace. " \
                              "The 'gather' option will create the schema/table if it does not exist. All segment performance data " \
                              "will be reset to prevent the gathering of the same performance data multiple times.")
        addTo.add_option('--clean', dest='clean', action='store_true',
                         help='All segment performance data will be reset.')
        addTo.add_option('--tempdir', dest="tempdir", type='string', metavar="<type>", default="/tmp",
                         help="Temporary directory used by gpperformancecounter. The default is /tmp. \n")
        return parser

    #
    # Create the program
    #
    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified")
        return GpPerformanceCounterProgram(options)
