#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal

from gppylib import gparray, gplog, pgconf, userinput, utils
from gppylib.commands import base, gp, pg, unix
from gppylib.db import catalog, dbconn
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.operations.startSegments import *
from gppylib.operations.buildMirrorSegments import *
from gppylib.programs import programIoUtils
from gppylib.system import configurationInterface as configInterface
from gppylib.system.environment import GpMasterEnvironment
from gppylib.test.testUtils import *
from gppylib.utils import toNonNoneString, checkNotNone, readAllLinesFromFile, writeLinesToFile

logger = gplog.get_default_logger()

#-------------------------------------------------------------------------
class GpSystemStateProgram:

    #
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.__options = options
        self.__pool = None

    def __appendSegmentTripletToArray(self, segment, line):
        """
        returns line

        @param the line to which to append the triplet of address/datadir/port
        """
        line.append(segment.getSegmentAddress())
        line.append(segment.getSegmentDataDirectory())
        line.append(str(segment.getSegmentPort()))
        return line

    def __displayClusterConfig(self, gpEnv, gpArray, mirrorType):
        """
        Returns the exitCode

        """
        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            logger.info("-------------------------------------------------------------" )
            logger.info("-Current GPDB mirror list and status" )
            logger.info("-Type = %s" % mirrorType )
            logger.info("-------------------------------------------------------------" )

            primarySegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentPrimary(False) ]
            mirrorSegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentMirror(False) ]
            contentIdToMirror = GpArray.getSegmentsByContentId(mirrorSegments)

            lines = []
            lines.append(["Status", "Data State", "Primary", "Datadir", "Port", "Mirror", "Datadir", "Port"])
            for primary in primarySegments:
                mirror = contentIdToMirror[primary.getSegmentContentId()][0]

                status = ""
                if primary.isSegmentMirror(True):
                    actingPrimary = mirror
                    actingMirror = primary

                    actMirrorStatus = "Available" if actingMirror.isSegmentUp() else "Failed"
                    status = "Mirror Active, Primary %s" % (actMirrorStatus)
                else:
                    actingPrimary = primary
                    actingMirror = mirror

                    actMirrorStatus = "Available" if actingMirror.isSegmentUp() else "Failed"
                    status = "Primary Active, Mirror %s" % (actMirrorStatus)

                dataStatus = gparray.getDataModeLabel(actingPrimary.getSegmentMode())

                line = [status, dataStatus]
                self.__appendSegmentTripletToArray(primary, line)
                self.__appendSegmentTripletToArray(mirror, line)
                lines.append(line)
            utils.logLinesToTable(lines, {})
        else:
            logger.info("-------------------------------------------------------------" )
            logger.info("-Primary list [%s not implemented]" % \
                        gparray.getFaultStrategyLabel(gparray.FAULT_STRATEGY_FILE_REPLICATION))
            logger.info("-------------------------------------------------------------" )

            lines = []
            lines.append(["Primary", "Datadir", "Port"])
            for seg in [ seg for seg in gpArray.getSegDbList()]:
                lines.append(self.__appendSegmentTripletToArray(seg, []))
            utils.logLinesToTable(lines, {})

        return 0

    def __displayMirrorList(self,gpEnv, gpArray, mirrorType):
        """
        Returns the exitCode
        """
        exitCode = 0
        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            lines = []
            lines.append(["Mirror","Datadir", "Port", "Status", ""])
            warningLineNumbers = {}

            # based off the bash version of -m "mirror list" option,
            #    the mirror list prints information about defined mirrors only
            mirrorSegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentMirror(False) ]
            for seg in mirrorSegments:
                doWarn = False
                if seg.isSegmentPrimary(True):
                    if seg.isSegmentModeSynchronized():
                        status = "Active, Synchronized"
                    elif seg.isSegmentModeInResynchronization():
                        status = "Active, Resynchronizing"
                    else:
                        status = "Active, In Change Tracking"

                    doWarn = True
                    exitCode = 1
                elif seg.isSegmentUp():
                    if seg.isSegmentModeSynchronized():
                        status = "Passive, Synchronized"
                    elif seg.isSegmentModeInResynchronization():
                        status = "Passive, Resynchronizing"
                    else:
                        status = "Passive" # this should not happen
                else:
                    status="Failed"
                    doWarn = True

                extra = "<<<<<<<<<<" if doWarn else ""
                warningLineNumbers[len(lines)] = doWarn
                line = self.__appendSegmentTripletToArray(seg, [])
                line.append(status)
                line.append(extra)
                lines.append(line)

            logger.info("-------------------------------------------------------------" )
            logger.info("-Current GPDB mirror list and status" )
            logger.info("-Type = %s" % mirrorType )
            logger.info("-------------------------------------------------------------" )

            utils.logLinesToTable(lines, warningLineNumbers)

        else:
            logger.warn("-------------------------------------------------------------" )
            logger.warn( "%s not used" % gparray.getFaultStrategyLabel(gparray.FAULT_STRATEGY_FILE_REPLICATION))
            logger.warn("-------------------------------------------------------------" )

        return exitCode

    def __showQuickStatus(self, gpEnv, gpArray, mirrorType):

        exitCode = 0

        logger.info("-Quick Greenplum database status from Master instance only")
        logger.info( "----------------------------------------------------------")

        segments = [seg for seg in gpArray.getDbList() if seg.isSegmentQE()]
        upSegments = [seg for seg in segments if seg.isSegmentUp()]
        downSegments = [seg for seg in segments if seg.isSegmentDown()]

        logger.info("# of up segments, from configuration table     = %s" % (len(upSegments)))
        if len(downSegments) > 0:
            exitCode = 1

            logger.info("# of down segments, from configuration table   = %s" % (len(downSegments)))

            lines = []
            lines.append(["Down Segment", "Datadir", "Port"])
            for seg in downSegments:
                lines.append(self.__appendSegmentTripletToArray(seg, []))
            utils.logLinesToTable(lines, {}) # use {} for warningLineNumbers, this script doesn't warn right now

        logger.info( "----------------------------------------------------------")

        return exitCode

    def __showVersionInfo(self, gpEnv, gpArray):

        exitCode = 0

        logger.info("Loading version information")
        segmentsAndMaster = [seg for seg in gpArray.getDbList()]
        upSegmentsAndMaster = [seg for seg in segmentsAndMaster if seg.isSegmentUp()]

        # fetch from hosts
        segmentsByHost = GpArray.getSegmentsByHostName(upSegmentsAndMaster)
        dispatchCount = 0
        for hostName, segments in segmentsByHost.iteritems():
            cmd = gp.GpGetSegStatusUsingTransition("get segment version status", segments, "getVersion",
                               verbose=logging_is_verbose(),
                               ctxt=base.REMOTE,
                               remoteHost=segments[0].getSegmentAddress())
            self.__pool.addCommand(cmd)
            dispatchCount+=1

        self.__pool.wait_and_printdots(dispatchCount, False) # False -- self.__quiet

        # group output
        dbIdToVersion = {}
        uniqueVersions = {}
        for cmd in self.__pool.getCompletedItems():
            if cmd.get_results().rc == 0:
                outputFromCmd = None
                for line in cmd.get_results().stdout.split('\n'):
                    if line.startswith("STATUS_RESULTS:"):
                        toDecode = line[len("STATUS_RESULTS:"):]
                        outputFromCmd = pickle.loads(base64.urlsafe_b64decode(toDecode))
                        break
                if outputFromCmd is None:
                    logger.warn("No status output provided from host %s" % cmd.remoteHost)
                else:
                    segments = cmd.dblist
                    for segIndex, seg in enumerate(segments):
                        version = outputFromCmd[segIndex]
                        if version is not None:
                            dbIdToVersion[segments[segIndex].getSegmentDbId()] = version
                            uniqueVersions[version] = True
            else:
                logger.warn("Error getting status from host %s" % cmd.remoteHost)

        # print the list of all segments and warnings about trouble
        warningLineNumbers = {}
        lines = []
        lines.append(["Host","Datadir", "Port", "Version", ""])
        for seg in segmentsAndMaster:
            line = self.__appendSegmentTripletToArray(seg, [])
            version = dbIdToVersion.get(seg.getSegmentDbId())
            doWarn = False
            if version is None:
                warningLineNumbers[len(lines)] = True
                line.append("unable to retrieve version")
                line.append("<<<<<<<<<<")
            else:
                line.append(version)
                line.append("")
            lines.append(line)
        utils.logLinesToTable(lines, warningLineNumbers)

        if len(uniqueVersions) > 1:
            logger.warn("Versions for some segments do not match.  Review table above for details.")

        hadFailures = len(dbIdToVersion) != len(segmentsAndMaster)
        if hadFailures:
            logger.warn("Unable to retrieve version data from all segments.  Review table above for details.")

        if len(uniqueVersions) == 1 and not hadFailures:
            # if we got data from all segments then we are confident they are all the same version
            logger.info("All segments are running the same software version")

        self.__pool.empty_completed_items()

        return exitCode

    def run(self):

        # check that only one option is set
        numSet = (1 if self.__options.showMirrorList else 0) + \
                 (1 if self.__options.showClusterConfig else 0) + \
                 (1 if self.__options.showQuickStatus else 0) + \
                 (1 if self.__options.showVersionInfo else 0)
        if numSet > 1:
            raise ProgramArgumentValidationException("Too many output options specified")

        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)

        # load config
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
        gpArray = confProvider.loadSystemConfig()

        # do it!
        if self.__options.showMirrorList:
            exitCode = self.__displayMirrorList(gpEnv, gpArray, self.__options.mirrorType)
        elif self.__options.showClusterConfig:
            exitCode = self.__displayClusterConfig(gpEnv, gpArray, self.__options.mirrorType)
        elif self.__options.showQuickStatus:
            exitCode = self.__showQuickStatus(gpEnv, gpArray, self.__options.mirrorType)
        elif self.__options.showVersionInfo:
            exitCode = self.__showVersionInfo(gpEnv, gpArray )
        else:
            raise ProgramArgumentValidationException( "No output options specified")

        return exitCode

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("Display system state")
        help = [""]

        parser = OptParser(option_class=OptChecker,
                    description=' '.join(description.split()),
                    version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = OptionGroup(parser, "Connection Options")
        parser.add_option_group(addTo)
        addMasterDirectoryOptionForSingleClusterProgram(addTo)

        addTo = OptionGroup(parser, "Output Options")
        parser.add_option_group(addTo)
        addTo.add_option('-m', None, default=False, action='store_true',
                            dest="showMirrorList",
                            metavar="<showMirrorList>",
                            help="Show mirror list from configuration")
        addTo.add_option('-c', None, default=False, action='store_true',
                            dest="showClusterConfig",
                            metavar="<showClusterConfig>",
                            help="Show cluster configuration")
        addTo.add_option("-Q", None, default=False, action="store_true",
                            dest="showQuickStatus",
                            metavar="<showQuickStatus>",
                            help="Show quick status")
        addTo.add_option("-i", None, default=False, action="store_true",
                            dest="showVersionInfo",
                            metavar="<showVersionInfo>",
                            help="Show version information")

        addTo = OptionGroup(parser, "Other Options")
        parser.add_option_group(addTo)
        addTo.add_option("-M", None, default="", # this should be removed when bash is all gone
                            dest="mirrorType",
                            metavar="<mirrorType>",
                            help="mirrorType, as calculated from $MIR_TYPE")
        addTo.add_option("-B", None, type="int", default=16,
                            dest="parallelDegree",
                            metavar="<parallelDegree>",
                            help="Max # of workers to use querying segments for status.  [default: %default]")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified", True)
        return GpSystemStateProgram(options)
    
        
    
