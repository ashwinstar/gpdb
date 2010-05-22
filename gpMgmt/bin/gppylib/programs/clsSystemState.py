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

    def __getMirrorType(self, gpArray):

        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            if gpArray.guessIsMultiHome():
                if gpArray.guessIsSpreadMirror():
                    return "Spread [Multi-home]"
                else:
                    return "Group [Multi-home]"
            else:
                if gpArray.guessIsSpreadMirror():
                    return "Spread [Single-home]"
                else:
                    return "Group [Single-home]"
        else:
            return "No Mirror"

    def __displayClusterConfig(self, gpEnv, gpArray):
        """
        Returns the exitCode

        """
        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            logger.info("-------------------------------------------------------------" )
            logger.info("-Current GPDB mirror list and status" )
            logger.info("-Type = %s" % self.__getMirrorType(gpArray) )
            logger.info("-------------------------------------------------------------" )

            primarySegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentPrimary(False) ]
            mirrorSegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentMirror(False) ]
            contentIdToMirror = GpArray.getSegmentsByContentId(mirrorSegments)

            lines = []
            lines.append(["Status", "Data State", "Primary", "Datadir", "Port", "Mirror", "Datadir", "Port"])
            numInChangeTracking = 0
            numMirrorsActingAsPrimaries = 0
            for primary in primarySegments:
                mirror = contentIdToMirror[primary.getSegmentContentId()][0]

                warningLineNumbers = {}
                doWarn = False
                status = ""
                if primary.isSegmentMirror(True):
                    actingPrimary = mirror
                    actingMirror = primary

                    actMirrorStatus = "Available" if actingMirror.isSegmentUp() else "Failed"
                    status = "Mirror Active, Primary %s" % (actMirrorStatus)

                    doWarn = True
                    numMirrorsActingAsPrimaries += 1
                else:
                    actingPrimary = primary
                    actingMirror = mirror

                    actMirrorStatus = "Available" if actingMirror.isSegmentUp() else "Failed"
                    status = "Primary Active, Mirror %s" % (actMirrorStatus)

                if actingPrimary.isSegmentModeInChangeLogging():
                    doWarn = True
                    numInChangeTracking += 1

                extra = "<<<<<<<<<<" if doWarn else ""
                warningLineNumbers[len(lines)] = doWarn

                dataStatus = gparray.getDataModeLabel(actingPrimary.getSegmentMode())
                line = [status, dataStatus]
                self.__appendSegmentTripletToArray(primary, line)
                self.__appendSegmentTripletToArray(mirror, line)
                line.append(extra)
                lines.append(line)
            utils.logLinesToTable(lines, warningLineNumbers)

            logger.info("-------------------------------------------------------------" )
            if numMirrorsActingAsPrimaries > 0:
                logger.warn( "%s segment(s) configured as mirror(s) are acting as primaries" % numMirrorsActingAsPrimaries )
            if numInChangeTracking > 0:
                logger.warn( "%s segment(s) are in change tracking" % numInChangeTracking)

        else:
            logger.info("-------------------------------------------------------------" )
            logger.info("-Primary list [%s not used]" % \
                        gparray.getFaultStrategyLabel(gparray.FAULT_STRATEGY_FILE_REPLICATION))
            logger.info("-------------------------------------------------------------" )

            lines = []
            lines.append(["Primary", "Datadir", "Port"])
            for seg in [ seg for seg in gpArray.getSegDbList()]:
                lines.append(self.__appendSegmentTripletToArray(seg, []))
            utils.logLinesToTable(lines, {})
            logger.info("-------------------------------------------------------------" )

        return 0

    def __displayMirrorList(self,gpEnv, gpArray):
        """
        Returns the exitCode
        """
        exitCode = 0
        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            lines = []
            lines.append(["Mirror","Datadir", "Port", "Status", "Data Status", ""])
            warningLineNumbers = {}

            # based off the bash version of -m "mirror list" option,
            #    the mirror list prints information about defined mirrors only
            mirrorSegments = [ seg for seg in gpArray.getSegDbList() if seg.isSegmentMirror(False) ]
            numMirrorsActingAsPrimaries = 0
            numFailedMirrors = 0
            numChangeTrackingMirrors = 0
            for seg in mirrorSegments:
                doWarn = False
                status = ""
                dataStatus = gparray.getDataModeLabel(seg.getSegmentMode())
                if seg.isSegmentPrimary(True):
                    status = "Acting as Primary"

                    if seg.isSegmentModeInChangeLogging():
                        numChangeTrackingMirrors += 1

                    numMirrorsActingAsPrimaries += 1
                    doWarn = True
                elif seg.isSegmentUp():
                    status = "Passive"
                else:
                    status = "Failed"
                    dataStatus = ""
                    doWarn = True
                    numFailedMirrors += 1

                if doWarn:
                    exitCode = 1

                extra = "<<<<<<<<<<" if doWarn else ""
                warningLineNumbers[len(lines)] = doWarn
                line = self.__appendSegmentTripletToArray(seg, [])
                line.append(status)
                line.append(dataStatus)
                line.append(extra)
                lines.append(line)

            logger.info("-------------------------------------------------------------" )
            logger.info("-Current GPDB mirror list and status" )
            logger.info("-Type = %s" % self.__getMirrorType(gpArray) )
            logger.info("-------------------------------------------------------------" )

            utils.logLinesToTable(lines, warningLineNumbers)

            logger.info("-------------------------------------------------------------" )
            if numMirrorsActingAsPrimaries > 0:
                logger.warn( "%s segment(s) configured as mirror(s) are acting as primaries" % numMirrorsActingAsPrimaries )
            if numFailedMirrors > 0:
                logger.warn( "%s segment(s) configured as mirror(s) have failed" % numFailedMirrors )
            if numChangeTrackingMirrors > 0:
                logger.warn( "%s mirror segment(s) acting as primaries are in change tracking" % numChangeTrackingMirrors)

        else:
            logger.warn("-------------------------------------------------------------" )
            logger.warn( "%s not used" % gparray.getFaultStrategyLabel(gparray.FAULT_STRATEGY_FILE_REPLICATION))
            logger.warn("-------------------------------------------------------------" )

        return exitCode

    def __showQuickStatus(self, gpEnv, gpArray):

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

    def __showPortInfo(self, gpEnv, gpArray):

        logger.info("-Master segment instance  %s  port = %d" % (gpEnv.getMasterDataDir(), gpEnv.getMasterPort()))
        logger.info("-Segment instance port assignments")
        logger.info("----------------------------------")

        lines = []
        lines.append([ "Host", "Datadir", "Port"])
        for seg in gpArray.getSegDbList():
            lines.append(self.__appendSegmentTripletToArray(seg, []))
        utils.logLinesToTable(lines, {}) # use {} for warningLineNumbers, this script doesn't warn right now

    def __showStandbyMasterInformation(self, gpEnv, gpArray):
        definedMasters = [seg for seg in gpArray.getDbList() if seg.isSegmentMaster(current_role=False)]
        definedStandbys = [seg for seg in gpArray.getDbList() if seg.isSegmentStandby(current_role=False)]

        #
        # print standby configuration/status
        #
        if not definedStandbys:
            logger.info("Standby master instance not configured")
        else:
            definedStandby = definedStandbys[0]

            cmd = gp.GpGetSegmentStatusValues("get standby segment version status", [definedStandby],
                               [gp.SEGMENT_STATUS__GET_PID], verbose=logging_is_verbose(), ctxt=base.REMOTE,
                               remoteHost=definedStandby.getSegmentAddress())
            cmd.run()

            # fetch standby pid
            (standbyPidFetchWarning, outputFromCmd) = cmd.decodeResults()
            if standbyPidFetchWarning is None:
                pidData = outputFromCmd[gp.SEGMENT_STATUS__GET_PID][definedStandby.getSegmentDbId()]
            else:
                pidData = {}
                pidData['pid'] = 0
                pidData['error'] = None

            if definedStandby.isSegmentMaster(current_role=True):
                standbyStatus = "STANDBY HOST ACTIVE INSTANCE MASTER"
            else:
                standbyStatus = "Standby host passive"

            # Print output!
            warningLineNumbers = {}
            logger.info("Standby master details" )
            logger.info("----------------------" )
            lines = []
            lines.append(["Standby address", "= %s" % definedStandby.getSegmentAddress()])
            lines.append(["Standby data directory", "= %s" % definedStandby.getSegmentDataDirectory()])
            lines.append(["Standby port", "= %s" % definedStandby.getSegmentPort()])
            if standbyPidFetchWarning is not None:
                warningLineNumbers[len(lines)] = True
                lines.append(["Standby PID", "= %s" % standbyPidFetchWarning, "<<<<<<<<<<" ])

                warningLineNumbers[len(lines)] = True
                lines.append(["Standby status", "= Status could not be determined", "<<<<<<<<<<" ])
            elif pidData['pid'] == 0:
                warningLineNumbers[len(lines)] = True
                lines.append(["Standby PID", "= 0", "<<<<<<<<<<" ])

                warningLineNumbers[len(lines)] = True
                lines.append(["Standby status", "= Standby process not running", "<<<<<<<<<<" ])
            else:
                if pidData['error'] is not None:
                    #
                    # we got a pid value but had some kind of error -- so possibly the PID
                    #   is not actually active on its port.  Print the error
                    #
                    warningLineNumbers[len(lines)] = True
                    lines.append(["Standby PID", "= %s" % pidData['pid'], "<<<<<<<<<< %s" % pidData['error']])
                else:
                    lines.append(["Standby PID", "= %s" % pidData['pid']])

                lines.append(["Standby status", "= %s" % standbyStatus ])
            utils.logLinesToTable(lines, warningLineNumbers)

        #
        # now print gp_master_mirroring table
        #
        dbUrl = dbconn.DbURL(port=gpEnv.getMasterPort(), dbname='template1')
        conn = dbconn.connect(dbUrl, utility=True)

        logger.info("-------------------------------------------------------------" )
        logger.info("-gp_master_mirroring table" )
        logger.info("-------------------------------------------------------------" )

        isFirst = True # isFirst is used just for making sure it's a singleton
        sql = "SELECT summary_state, detail_state, log_time, error_message FROM gp_master_mirroring"
        for row in dbconn.execSQL( conn, sql):
            assert isFirst, "Too many rows returned from expected single-row table 'gp_master_mirroring'"

            row = ["" if val is None else val for val in row] # transform None to empty string

            logger.info("-Summary state: %s" % row[0])
            logger.info("-Detail state: %s" % row[1])
            logger.info("-Log time: %s" % row[2])
            if row[3] != "":
                logger.info("-Error message: %s" % row[3])

            isFirst = False
        assert not isFirst, "No rows returned from expected single-row table 'gp_master_mirroring"
        logger.info("-------------------------------------------------------------" )

        conn.close()
        # done printing gp_master_mirroring table

    def __showVersionInfo(self, gpEnv, gpArray):

        exitCode = 0

        logger.info("Loading version information")
        segmentsAndMaster = [seg for seg in gpArray.getDbList()]
        upSegmentsAndMaster = [seg for seg in segmentsAndMaster if seg.isSegmentUp()]

        # fetch from hosts
        segmentsByHost = GpArray.getSegmentsByHostName(upSegmentsAndMaster)
        dispatchCount = 0
        for hostName, segments in segmentsByHost.iteritems():
            cmd = gp.GpGetSegmentStatusValues("get segment version status", segments,
                               [gp.SEGMENT_STATUS__GET_VERSION],
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
            (warning, outputFromCmd) = cmd.decodeResults()
            if warning is None:
                for seg in cmd.dblist:
                    version = outputFromCmd[gp.SEGMENT_STATUS__GET_VERSION][seg.getSegmentDbId()]
                    if version is not None:
                        version = version.rstrip()
                        dbIdToVersion[seg.getSegmentDbId()] = version
                        uniqueVersions[version] = True
            else:
                logger.warn(warning)

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
                 (1 if self.__options.showPortInformation else 0) + \
                 (1 if self.__options.showStandbyMasterInformation else 0) + \
                 (1 if self.__options.showVersionInfo else 0)
        if numSet > 1:
            raise ProgramArgumentValidationException("Too many output options specified")

        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)

        # load config
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), True)
        gpArray = confProvider.loadSystemConfig()

        # do it!
        if self.__options.showMirrorList:
            exitCode = self.__displayMirrorList(gpEnv, gpArray)
        elif self.__options.showClusterConfig:
            exitCode = self.__displayClusterConfig(gpEnv, gpArray)
        elif self.__options.showQuickStatus:
            exitCode = self.__showQuickStatus(gpEnv, gpArray)
        elif self.__options.showVersionInfo:
            exitCode = self.__showVersionInfo(gpEnv, gpArray)
        elif self.__options.showStandbyMasterInformation:
            exitCode = self.__showStandbyMasterInformation(gpEnv, gpArray)
        elif self.__options.showPortInformation:
            exitCode = self.__showPortInfo(gpEnv, gpArray)
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
        addTo.add_option("-p", None, default=False, action="store_true",
                            dest="showPortInformation",
                            metavar="<showPortInformation>",
                            help="Show port information")
        addTo.add_option("-f", None, default=False, action="store_true",
                            dest="showStandbyMasterInformation",
                            metavar="<showStandbyMasterInformation>",
                            help="Show standby master information")

        addTo = OptionGroup(parser, "Other Options")
        parser.add_option_group(addTo)
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
    
        
    
