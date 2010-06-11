#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal
import datetime

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
from gppylib.utils import toNonNoneString, checkNotNone, readAllLinesFromFile, writeLinesToFile, TableLogger

logger = gplog.get_default_logger()

CATEGORY__SEGMENT_INFO = "Segment Info"
VALUE__HOSTNAME = "Hostname"
VALUE__ADDRESS = "Address"
VALUE__DATADIR = "Datadir"
VALUE__PORT = "Port"

CATEGORY__MIRRORING_INFO = "Mirroring Info"
VALUE__CURRENT_ROLE = "Current role"
VALUE__PREFERRED_ROLE = "Preferred role"
VALUE__MIRROR_STATUS = "Mirror status"

CATEGORY__ERROR_GETTING_SEGMENT_STATUS = "Error Getting Segment Status"
VALUE__ERROR_GETTING_SEGMENT_STATUS = "Error Getting Segment Status"

CATEGORY__CHANGE_TRACKING_INFO = "Change Tracking Info"
VALUE__CHANGE_TRACKING_DATA_SIZE = "Change tracking data size"

CATEGORY__RESYNCHRONIZATION_INFO = "Resynchronization Info"
VALUE__RESYNC_MODE = "Resynchronization mode"
VALUE__RESYNC_DATA_SYNCHRONIZED = "Data synchronized"
VALUE__RESYNC_EST_TOTAL_DATA = "Estimated total data to synchronize"
VALUE__RESYNC_EST_PROGRESS_WITH_MIRROR = "Estimated resync progress with mirror"
VALUE__RESYNC_EST_COMPLETION_TIME = "Estimated resync completion time"

CATEGORY__PROCESS_INFO = "Process Info"
VALUE__POSTMASTER_PID_FILE = "File postmaster.pid"
VALUE__POSTMASTER_PID_VALUE = "PID from postmaster.pid file"
VALUE__LOCK_FILES= "Lock files in /tmp"
VALUE__ACTIVE_PID = "Active PID"

CATEGORY__STATUS = "Status"
VALUE__MASTER_REPORTS_STATUS = "Master reports status as"
VALUE__MIRROR_SEGMENT_STATUS = "Segment status"
VALUE__NONMIRROR_DATABASE_STATUS = "Database status"

class GpStateData:
    """
    Store key-value pairs of unpacked data for each segment in the cluster

    Also provides categories on top of this

    To add new values:
    1) add CATEGORY_* and VALUE* constants as appropriate
    2) update self.__categories and self.__entriesByCategories below
    3) call .addValue from the code that loads the values (search for it down below)

    """
    def __init__(self ):
        self.__segmentData = []
        self.__segmentDbIdToSegmentData = {}
        self.__dbIdIsProbablyDown = {}
        self.__contentsWithUpSegments = {}
        self.__currentSegmentData = None
        self.__categories = [
                    CATEGORY__SEGMENT_INFO,
                    CATEGORY__MIRRORING_INFO,
                    CATEGORY__ERROR_GETTING_SEGMENT_STATUS,
                    CATEGORY__CHANGE_TRACKING_INFO,
                    CATEGORY__RESYNCHRONIZATION_INFO,
                    CATEGORY__PROCESS_INFO,
                    CATEGORY__STATUS]
        self.__entriesByCategory = {}

        self.__entriesByCategory[CATEGORY__SEGMENT_INFO] = \
                [VALUE__HOSTNAME,
                VALUE__ADDRESS,
                VALUE__DATADIR,
                VALUE__PORT]

        self.__entriesByCategory[CATEGORY__MIRRORING_INFO] = \
                [VALUE__CURRENT_ROLE,
                VALUE__PREFERRED_ROLE,
                VALUE__MIRROR_STATUS]

        self.__entriesByCategory[CATEGORY__ERROR_GETTING_SEGMENT_STATUS] = \
                [VALUE__ERROR_GETTING_SEGMENT_STATUS]
        
        self.__entriesByCategory[CATEGORY__CHANGE_TRACKING_INFO] = \
                [VALUE__CHANGE_TRACKING_DATA_SIZE]

        self.__entriesByCategory[CATEGORY__RESYNCHRONIZATION_INFO] = \
                [VALUE__RESYNC_MODE, 
                VALUE__RESYNC_DATA_SYNCHRONIZED,
                VALUE__RESYNC_EST_TOTAL_DATA,
                VALUE__RESYNC_EST_PROGRESS_WITH_MIRROR,
                VALUE__RESYNC_EST_COMPLETION_TIME]

        self.__entriesByCategory[CATEGORY__PROCESS_INFO] = \
                [VALUE__POSTMASTER_PID_FILE,
                 VALUE__POSTMASTER_PID_VALUE,
                 VALUE__LOCK_FILES,
                 VALUE__ACTIVE_PID]

        self.__entriesByCategory[CATEGORY__STATUS] = \
                [VALUE__MASTER_REPORTS_STATUS,
                VALUE__MIRROR_SEGMENT_STATUS,
                VALUE__NONMIRROR_DATABASE_STATUS]

        self.__allValues = {}
        for values in self.__entriesByCategory.values():
            for v in values:
                self.__allValues[v] = True

    def beginSegment(self, segment):
        self.__currentSegmentData = {}
        self.__currentSegmentData["values"] = {}
        self.__currentSegmentData["isWarning"] = {}

        self.__segmentData.append(self.__currentSegmentData)
        self.__segmentDbIdToSegmentData[segment.getSegmentDbId()] = self.__currentSegmentData

    def addValue(self, key, value, isWarning=False):
        self.__currentSegmentData["values"][key] = value
        self.__currentSegmentData["isWarning"][key] = isWarning

        assert key in self.__allValues;

    def isClusterProbablyDown(self, gpArray):
        """
          approximate whether or not the cluster has a problem and need to review
          we could beef this up -- for example, the mirror is only useful
          if we are in resync mode
        """
        for seg in gpArray.getSegDbList():
            if seg.getSegmentContentId() not in self.__contentsWithUpSegments:
                return True
        return False

    def setDatabaseProbablyDown(self, seg, isThisSegmentProbablyDown):
        if isThisSegmentProbablyDown:
            self.__dbIdIsProbablyDown[seg.getSegmentDbId()] = True
        else:
            self.__contentsWithUpSegments[seg.getSegmentContentId()] = True

    def addSegmentToTableLogger(self, tabLog, segment):

        data = self.__segmentDbIdToSegmentData[segment.getSegmentDbId()]
        valuesMap = data["values"]
        isWarningMap = data["isWarning"]

        for cat in self.__categories:
            keys = self.__entriesByCategory[cat]

            hasValue = False
            for k in keys:
                if k in valuesMap:
                    hasValue = True
                    break
            if not hasValue:
                #
                # skip categories for which we have no values!
                #
                continue

            tabLog.info([cat])
            for k in keys:
                if k in valuesMap:
                    val = valuesMap[k]
                    if val is None:
                        val = ""
                    else:
                        val = str(val)
                    tabLog.infoOrWarn(isWarningMap[k], ["   " + k, "= %s" % val])
            
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

    def __showClusterConfig(self, gpEnv, gpArray):
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

            tabLog = TableLogger().setWarnWithArrows(True)
            tabLog.info(["Status", "Data State", "Primary", "Datadir", "Port", "Mirror", "Datadir", "Port"])
            numInChangeTracking = 0
            numMirrorsActingAsPrimaries = 0
            for primary in primarySegments:
                mirror = contentIdToMirror[primary.getSegmentContentId()][0]

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

                dataStatus = gparray.getDataModeLabel(actingPrimary.getSegmentMode())
                line = [status, dataStatus]
                self.__appendSegmentTripletToArray(primary, line)
                self.__appendSegmentTripletToArray(mirror, line)

                tabLog.infoOrWarn(doWarn, line)
            tabLog.outputTable()

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

            tabLog = TableLogger().setWarnWithArrows(True)
            tabLog.info(["Primary", "Datadir", "Port"])
            for seg in [ seg for seg in gpArray.getSegDbList()]:
                tabLog.info(self.__appendSegmentTripletToArray(seg, []))
            tabLog.outputTable()
            logger.info("-------------------------------------------------------------" )

        return 0

    def _showMirrorList(self,gpEnv, gpArray):
        """
        Returns the exitCode
        """
        exitCode = 0
        if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
            tabLog = TableLogger().setWarnWithArrows(True)
            tabLog.info(["Mirror","Datadir", "Port", "Status", "Data Status", ""])

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

                line = self.__appendSegmentTripletToArray(seg, [])
                line.extend([status, dataStatus])

                tabLog.infoOrWarn(doWarn, line)

            logger.info("-------------------------------------------------------------" )
            logger.info("-Current GPDB mirror list and status" )
            logger.info("-Type = %s" % self.__getMirrorType(gpArray) )
            logger.info("-------------------------------------------------------------" )

            tabLog.outputTable()

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

    def __appendStandbySummary(self, hostNameToResults, standby, tabLog):
        """
        Log information about the configured standby and its current status
        """
        if standby is None:
            tabLog.info(["Master standby", "= No master standby configured"])
        else:
            tabLog.info(["Master standby", "= %s" % standby.getSegmentHostName()])

            (standbyStatusFetchWarning, outputFromStandbyCmd) = hostNameToResults[standby.getSegmentHostName()]
            standbyData = outputFromStandbyCmd[standby.getSegmentDbId()] if standbyStatusFetchWarning is None else None

            if standbyStatusFetchWarning is not None:
                tabLog.warn(["Standby master state", "= Status could not be determined: %s" % standbyStatusFetchWarning])

            elif standbyData[gp.SEGMENT_STATUS__HAS_POSTMASTER_PID_FILE] and \
                    standbyData[gp.SEGMENT_STATUS__GET_PID]['pid'] > 0 and \
                    standbyData[gp.SEGMENT_STATUS__GET_PID]['error'] is None:
                tabLog.info(["Standby master state", "= Standby host passive"])
                
            else:
                tabLog.warn(["Standby master state", "= Standby host DOWN"])

    def __showStatusStatistics(self, gpEnv, gpArray):
        """
        Print high-level numeric stats about the cluster

        returns the exit code
        """
        hostNameToResults = self.__fetchAllSegmentData(gpArray)

        logger.info("Greenplum instance status summary")

        # master summary info
        tabLog = TableLogger().setWarnWithArrows(True)

        tabLog.addSeparator()
        tabLog.info(["Master instance", "= Active"])

        self.__appendStandbySummary(hostNameToResults, gpArray.standbyMaster, tabLog)

        arrayType = "Multi-Home" if  gpArray.guessIsMultiHome() else "Standard"
        tabLog.info(["Greenplum array configuration type", "= %s" % arrayType])
        tabLog.addSeparator()

        tabLog.info(["Total segment instance count from metadata", "= %s" % len(gpArray.getSegDbList())])
        tabLog.addSeparator()

        # primary and mirror segment info
        for whichType in ["Primary", "Mirror"]:
            tabLog.info(["%s Segment Status" % whichType])
            tabLog.addSeparator()

            if whichType == "Primary":
                segs = [seg for seg in gpArray.getSegDbList() if seg.isSegmentPrimary(current_role=False)]
            else:
                segs = [seg for seg in gpArray.getSegDbList() if seg.isSegmentMirror(current_role=False)]
                if not segs:
                    tabLog.info(["Mirrors not configured on this array"])
                    tabLog.addSeparator()
                    continue

            numPostmasterPidFilesMissing = 0
            numPostmasterProcessesMissing = 0
            numLockFilesMissing = 0
            numPostmasterPidsMissing = 0
            for seg in segs:
                (statusFetchWarning, outputFromCmd) = hostNameToResults[seg.getSegmentHostName()]
                if statusFetchWarning is not None:
                    # I guess if we can't contact the segment that we can do this?
                    # or should add a new error row instead to account for this?
                    numPostmasterPidFilesMissing += 1
                    numLockFilesMissing += 1
                    numPostmasterProcessesMissing += 1
                    numPostmasterPidsMissing += 1                    
                else:
                    segmentData = outputFromCmd[seg.getSegmentDbId()]
                    if not segmentData[gp.SEGMENT_STATUS__HAS_LOCKFILE]:
                        numLockFilesMissing += 1
                    if not segmentData[gp.SEGMENT_STATUS__HAS_POSTMASTER_PID_FILE]:
                        numPostmasterPidFilesMissing += 1

                    # note: this (which I think matches old behavior fairly closely)
                    #        doesn't seem entirely correct -- we are checking whether netstat is
                    #        there, but not really checking that the process is running on that port?
                    if segmentData[gp.SEGMENT_STATUS__GET_PID] is None or \
                            segmentData[gp.SEGMENT_STATUS__GET_PID]['pid'] == 0:
                        numPostmasterPidsMissing += 1
                        numPostmasterProcessesMissing += 1
                    elif segmentData[gp.SEGMENT_STATUS__GET_PID]['error'] is not None:
                        numPostmasterProcessesMissing += 1

            numSegments = len(segs)
            numValidAtMaster = len([seg for seg in segs if seg.isSegmentUp()])
            numFailuresAtMaster = len([seg for seg in segs if seg.isSegmentDown()])
            numPostmasterPidFilesFound = numSegments - numPostmasterPidFilesMissing
            numLockFilesFound = numSegments - numLockFilesMissing
            numPostmasterPidsFound = numSegments - numPostmasterPidsMissing
            numPostmasterProcessesFound = numSegments - numPostmasterProcessesMissing

            # print stuff
            tabLog.info(["Total %s segments" % whichType.lower(), "= %d" % numSegments])
            tabLog.info(["Total %s segment valid (at master)" % whichType.lower(), "= %d" % numValidAtMaster])
            tabLog.infoOrWarn(numFailuresAtMaster > 0,
                      ["Total %s segment failures (at master)" % whichType.lower(), "= %d" % numFailuresAtMaster])

            tabLog.infoOrWarn(numPostmasterPidFilesMissing > 0,
                      ["Total number of postmaster.pid files missing", "= %d" % numPostmasterPidFilesMissing])
            tabLog.info( ["Total number of postmaster.pid files found", "= %d" % numPostmasterPidFilesFound])

            tabLog.infoOrWarn(numPostmasterPidsMissing > 0,
                      ["Total number of postmaster.pid PIDs missing", "= %d" % numPostmasterPidsMissing])
            tabLog.info( ["Total number of postmaster.pid PIDs found", "= %d" % numPostmasterPidsFound])

            tabLog.infoOrWarn(numLockFilesMissing > 0,
                        ["Total number of /tmp lock files missing", "= %d" % numLockFilesMissing])
            tabLog.info( ["Total number of /tmp lock files found", "= %d" % numLockFilesFound])

            tabLog.infoOrWarn(numPostmasterProcessesMissing > 0,
                        ["Total number postmaster processes missing", "= %d" % numPostmasterProcessesMissing])
            tabLog.info( ["Total number postmaster processes found", "= %d" % numPostmasterProcessesFound])

            if whichType == "Mirror":
                numMirrorsActive = len([seg for seg in segs if seg.isSegmentPrimary(current_role=True)])
                numMirrorsPassive = numSegments - numMirrorsActive
                tabLog.infoOrWarn(numMirrorsActive > 0,
                            ["Total number mirror segments acting as primary segments", "= %d" % numMirrorsActive])
                tabLog.info( ["Total number mirror segments acting as mirror segments", "= %d" % numMirrorsPassive])

            tabLog.addSeparator()
        tabLog.outputTable()

    def __fetchAllSegmentData(self, gpArray):
        """
        returns a dict mapping hostName to the GpGetSgementStatusValues decoded result
        """
        logger.info("Gathering data from segments...")
        segmentsByHost = GpArray.getSegmentsByHostName(gpArray.getDbList())
        segmentData = {}
        dispatchCount = 0
        hostNameToCmd = {}
        for hostName, segments in segmentsByHost.iteritems():
            cmd = gp.GpGetSegmentStatusValues("get segment version status", segments,
                              [gp.SEGMENT_STATUS__GET_VERSION,
                                gp.SEGMENT_STATUS__GET_PID,
                                gp.SEGMENT_STATUS__HAS_LOCKFILE,
                                gp.SEGMENT_STATUS__HAS_POSTMASTER_PID_FILE,
                                gp.SEGMENT_STATUS__GET_MIRROR_STATUS
                                ],
                               verbose=logging_is_verbose(),
                               ctxt=base.REMOTE,
                               remoteHost=segments[0].getSegmentAddress())
            hostNameToCmd[hostName] = cmd
            self.__pool.addCommand(cmd)
            dispatchCount+=1
        self.__pool.wait_and_printdots(dispatchCount, False) # False -- self.__quiet

        hostNameToResults = {}
        for hostName, cmd in hostNameToCmd.iteritems():
            hostNameToResults[hostName] = cmd.decodeResults()
        return hostNameToResults

    def __showStatus(self, gpEnv, gpArray):
        """
        Prints out the current status of the cluster.

        @param gpEnv the GpMasterEnvironment object
        @param gpArray the array to display

        returns the exit code
        """
        hasWarnings = False
        hostNameToResults = self.__fetchAllSegmentData(gpArray)

        #
        # fetch data about master
        #
        master = gpArray.master
        arrayConfigType = "Multi-Home" if gpArray.guessIsMultiHome() else "Standard"

        dbUrl = dbconn.DbURL(port=gpEnv.getMasterPort(), dbname='template1' )
        conn = dbconn.connect(dbUrl, utility=True)
        initDbVersion = dbconn.execSQLForSingletonRow(conn, "select productversion from gp_version_at_initdb limit 1;")[0]
        pgVersion = dbconn.execSQLForSingletonRow(conn, "show server_version;")[0]
        conn.close()

        try:
            # note: this is how old gpstate did this but ... can we do it without requiring a non-utility-mode
            #  connection?  non-utility-mode connections can take a long time to quit out if there
            #  are segment failures and you need to wait for the prober (and this would print
            #  role as "utility" even though it's really a failed-dispatcher.
            conn = dbconn.connect(dbUrl, utility=False)
            qdRole = "dispatch"
        except Exception:
            qdRole = "utility" # unable to connect in non-utility, but we've been able to connect in utility so...         
        #
        # print output about master
        #
        (statusFetchWarning, outputFromMasterCmd) = hostNameToResults[master.getSegmentHostName()]
        masterData = outputFromMasterCmd[master.getSegmentDbId()] if statusFetchWarning is None else None

        logger.info( "----------------------------------------------------" )
        logger.info("-Master Configuration & Status")
        logger.info( "----------------------------------------------------" )
        tabLog = TableLogger().setWarnWithArrows(True)
        tabLog.info(["Master host", "= %s" % master.getSegmentHostName()])
        if statusFetchWarning is None:
            pidData = masterData[gp.SEGMENT_STATUS__GET_PID]
            tabLog.info(["Master postgres process ID", "= %s" % pidData['pid']])
        else:
            tabLog.warn(["Master port", "= Error fetching data: %s" % statusFetchWarning])
        tabLog.info(["Master data directory", "= %s" % master.getSegmentDataDirectory()])
        tabLog.info(["Master port", "= %d" % master.getSegmentPort()])

        tabLog.info(["Master current role", "= %s" % qdRole])
        tabLog.info(["Greenplum array configuration type", "= %s" % arrayConfigType])
        tabLog.info(["Greenplum initsystem version", "= %s" % initDbVersion])

        if statusFetchWarning is None:
            if masterData[gp.SEGMENT_STATUS__GET_VERSION] is None:
                tabLog.warn(["Greenplum current version", "= Unknown"])
            else:
                tabLog.info(["Greenplum current version", "= %s" % masterData[gp.SEGMENT_STATUS__GET_VERSION]])
        else:
            tabLog.warn(["Greenplum current version", "= Error fetching data: %s" % statusFetchWarning])
        tabLog.info(["Postgres version", "= %s" % pgVersion])
            
        tabLog.info(["Greenplum mirroring status", "= %s" % gparray.getFaultStrategyLabel(gpArray.getFaultStrategy())])
        
        self.__appendStandbySummary(hostNameToResults, gpArray.standbyMaster, tabLog)
        tabLog.outputTable()
        hasWarnings = hasWarnings or tabLog.hasWarnings()

        #
        # Output about segments
        #
        logger.info("----------------------------------------------------")
        logger.info("Segment Instance Status Report")

        data = self.__buildGpStateData(gpArray, hostNameToResults)
        tabLog = TableLogger().setWarnWithArrows(True)
        for seg in gpArray.getSegDbList():
            tabLog.addSeparator()
            data.addSegmentToTableLogger(tabLog, seg)
        tabLog.outputTable()
        hasWarnings = hasWarnings or tabLog.hasWarnings()

        if False and data.isClusterProbablyDown(gpArray):  # disabled for now, may enable when adding resync status
            logger.warn("*****************************************************" )
            logger.warn("DATABASE IS PROBABLY UNAVAILABLE" )
            logger.warn("Review Instance Status in log file or screen output for more information" )
            logger.warn("*****************************************************" )

        if hasWarnings:
            logger.warn("*****************************************************" )
            logger.warn("Warnings have been generated during status processing" )
            logger.warn("Check log file or review screen output" )
            logger.warn("*****************************************************" )

        return 1 if hasWarnings else 0

    def __buildGpStateData(self, gpArray, hostNameToResults):
        data = GpStateData()
        primaryByContentId = GpArray.getSegmentsByContentId(\
                                [s for s in gpArray.getSegDbList() if s.isSegmentPrimary(current_role=True)])
        for seg in gpArray.getSegDbList():
            (statusFetchWarning, outputFromCmd) = hostNameToResults[seg.getSegmentHostName()]

            data.beginSegment(seg)
            data.addValue(VALUE__HOSTNAME, seg.getSegmentHostName())
            data.addValue(VALUE__ADDRESS, seg.getSegmentAddress())
            data.addValue(VALUE__DATADIR, seg.getSegmentDataDirectory())
            data.addValue(VALUE__PORT, seg.getSegmentPort())

            if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
                data.addValue(VALUE__CURRENT_ROLE, "Primary" if seg.isSegmentPrimary(current_role=True) else "Mirror")
                data.addValue(VALUE__PREFERRED_ROLE, "Primary" if seg.isSegmentPrimary(current_role=False) else "Mirror")

                if seg.isSegmentPrimary(current_role=True):
                    data.addValue(VALUE__MIRROR_STATUS, gparray.getDataModeLabel(seg.getSegmentMode()))
                else:
                    primary = primaryByContentId[seg.getSegmentContentId()][0]
                    if primary.getSegmentMode() == gparray.MODE_CHANGELOGGING:
                        data.addValue(VALUE__MIRROR_STATUS, "Out of Sync", isWarning=True)
                    else:
                        data.addValue(VALUE__MIRROR_STATUS, gparray.getDataModeLabel(seg.getSegmentMode()))

            if statusFetchWarning is not None:
                segmentData = None
                data.addValue(VALUE__ERROR_GETTING_SEGMENT_STATUS, statusFetchWarning)
            else:
                segmentData = outputFromCmd[seg.getSegmentDbId()]

                #
                # Able to fetch from that segment, proceed
                #

                #
                # mirror info
                #
                if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_FILE_REPLICATION:
                    # print out mirroring state from the segment itself
                    if seg.isSegmentPrimary(current_role=True):

                        if seg.getSegmentMode() == gparray.MODE_RESYNCHRONIZATION or \
                            seg.getSegmentMode() == gparray.MODE_CHANGELOGGING:

                            mirrorData = segmentData[gp.SEGMENT_STATUS__GET_MIRROR_STATUS]
                            if mirrorData is None or mirrorData["changeTrackingBytesUsed"] < 0:
                                # server returns <0 if there was an error calculating size
                                data.addValue(VALUE__CHANGE_TRACKING_DATA_SIZE, "unable to retrieve data size", isWarning=True)
                            else:
                                data.addValue(VALUE__CHANGE_TRACKING_DATA_SIZE, \
                                        self.__abbreviateBytes(mirrorData["changeTrackingBytesUsed"]))

                        if seg.getSegmentMode() == gparray.MODE_RESYNCHRONIZATION:
                            mirrorData = segmentData[gp.SEGMENT_STATUS__GET_MIRROR_STATUS]

                            if mirrorData is None:
                                data.addValue(VALUE__RESYNC_EST_PROGRESS_WITH_MIRROR, "unable to retrieve progress", isWarning=True)
                            else:
                                dataSynchronizedStr = self.__abbreviateBytes( mirrorData["resyncNumCompleted"] * 32L * 1024 )

                                if mirrorData["dataState"] == "InSync":
                                    totalDataToSynchronizeStr = "Synchronization complete; awaiting configuration change"
                                    resyncProgressStr = "100%"
                                    estimatedTimeToCompletionStr = "Synchronization complete; awaiting configuration change"
                                elif mirrorData["estimatedCompletionTimeSecondsSinceEpoch"] == 0:
                                    totalDataToSynchronizeStr = "Not Available"
                                    resyncProgressStr = "Not Available"
                                    estimatedTimeToCompletionStr = "Not Available"
                                else:

                                    if mirrorData["resyncTotalToComplete"] == 0:
                                        resyncProgressStr = "Not Available"
                                    else:
                                        percentComplete = 100 * mirrorData["resyncNumCompleted"] / float(mirrorData["resyncTotalToComplete"])
                                        resyncProgressStr = "%.2f%%" % percentComplete

                                    totalDataToSynchronizeStr = self.__abbreviateBytes( mirrorData["resyncTotalToComplete"] * 32L * 1024 )
                                    estimatedTimeToCompletionStr = datetime.datetime.fromtimestamp(mirrorData["estimatedCompletionTimeSecondsSinceEpoch"])

                                data.addValue(VALUE__RESYNC_MODE, "Full" if mirrorData['isFullResync'] else "Incremental")
                                data.addValue(VALUE__RESYNC_DATA_SYNCHRONIZED, dataSynchronizedStr)
                                data.addValue(VALUE__RESYNC_EST_TOTAL_DATA, totalDataToSynchronizeStr)
                                data.addValue(VALUE__RESYNC_EST_PROGRESS_WITH_MIRROR, resyncProgressStr)
                                data.addValue(VALUE__RESYNC_EST_COMPLETION_TIME, estimatedTimeToCompletionStr)


                #
                # Now PID status
                #
                pidData = segmentData[gp.SEGMENT_STATUS__GET_PID]

                found = segmentData[gp.SEGMENT_STATUS__HAS_POSTMASTER_PID_FILE]
                data.addValue(VALUE__POSTMASTER_PID_FILE, "Found" if found else "Missing", isWarning=not found)

                # PID from postmaster.pid
                data.addValue(VALUE__POSTMASTER_PID_VALUE, pidData["pid"], pidData['pid'] == 0)

                # has lock file
                found = segmentData[gp.SEGMENT_STATUS__HAS_LOCKFILE]
                data.addValue(VALUE__LOCK_FILES, "Found" if found else "Missing", isWarning=not found)

                if pidData['error'] is None:
                    data.addValue(VALUE__ACTIVE_PID, pidData["pid"])
                else:
                    data.addValue(VALUE__ACTIVE_PID, "Missing", False)

            data.addValue(VALUE__MASTER_REPORTS_STATUS, "Up" if seg.isSegmentUp() else "Down", seg.isSegmentDown())

            databaseStatus = None
            databaseStatusIsWarning = False

            if seg.isSegmentDown():
                databaseStatus = "Down in configuration"
                databaseStatusIsWarning = True
            elif segmentData is None:
                databaseStatus = "Unknown -- unable to load segment status"
                databaseStatusIsWarning = True
            elif segmentData[gp.SEGMENT_STATUS__GET_PID]['error'] is not None:
                databaseStatus = "Process error; see Process Info"
                databaseStatusIsWarning = True
            elif segmentData[gp.SEGMENT_STATUS__GET_MIRROR_STATUS] is None:
                databaseStatus = "Unknown -- unable to load segment status"
                databaseStatusIsWarning = True
            else:
                databaseStatus = segmentData[gp.SEGMENT_STATUS__GET_MIRROR_STATUS]["databaseStatus"]
                databaseStatusIsWarning = databaseStatus == "Uninitialized" or databaseStatus == "Down"

            if seg.isSegmentMirror(current_role=True):
                data.addValue(VALUE__MIRROR_SEGMENT_STATUS, databaseStatus, databaseStatusIsWarning)
            else:
                data.addValue(VALUE__NONMIRROR_DATABASE_STATUS, databaseStatus, databaseStatusIsWarning)

            data.setDatabaseProbablyDown(seg, databaseStatusIsWarning)
        return data        

    def __abbreviateBytes(self, numBytes):
        """
        Abbreviate bytes with 3 bytes of precision (so 1.45GB but also 12.3GB), except for numBytes < 1024


        SAMPLE TEST:

        def testAbbreviateBytes(bytes, expected=""):
            # logger.info(" %s abbreviates to %s" % (bytes, self.__abbreviateBytes(bytes)))
            if expected != self.__abbreviateBytes(bytes):
                raise Exception("Invalid abbreviation for %s : %s" % (bytes, self.__abbreviateBytes(bytes)))

        testAbbreviateBytes(0, "0 bytes")
        testAbbreviateBytes(1, "1 byte")
        testAbbreviateBytes(2, "2 bytes")
        testAbbreviateBytes(13, "13 bytes")
        testAbbreviateBytes(656, "656 bytes")
        testAbbreviateBytes(999, "999 bytes")
        testAbbreviateBytes(1000, "1000 bytes")
        testAbbreviateBytes(1001, "1001 bytes")
        testAbbreviateBytes(1024, "1.00 kB")
        testAbbreviateBytes(1301, "1.27 kB")
        testAbbreviateBytes(13501, "13.2 kB")
        testAbbreviateBytes(135401, "132 kB")
        testAbbreviateBytes(1354015, "1.29 MB")
        testAbbreviateBytes(13544015, "12.9 MB")
        testAbbreviateBytes(135440154, "129 MB")
        testAbbreviateBytes(1354401574, "1.26 GB")
        testAbbreviateBytes(13544015776, "12.6 GB")
        testAbbreviateBytes(135440157769, "126 GB")
        testAbbreviateBytes(1354401577609, "1.23 TB")
        testAbbreviateBytes(13544015776094, "12.3 TB")
        testAbbreviateBytes(135440157760944, "123 TB")
        testAbbreviateBytes(1754401577609464, "1.56 PB")
        testAbbreviateBytes(17544015776094646, "15.6 PB")
        testAbbreviateBytes(175440157760946475, "156 PB")
        testAbbreviateBytes(175440157760945555564, "155822 PB")


        """
        abbreviations = [
            (1024L*1024*1024*1024*1024, "PB"),
            (1024L*1024*1024*1024, "TB"),
            (1024L*1024*1024, "GB"),
            (1024L*1024, "MB"),
            (1024L, "kB"),
            (1, "bytes")]

        if numBytes == 1:
            return "1 byte"
        for factor, suffix in abbreviations:
            if numBytes >= factor:
                break

        precision = 3
        precisionForDisplay = precision - len('%d' % int(numBytes/factor))
        if precisionForDisplay < 0 or numBytes < 1024:
            precisionForDisplay = 0

        return '%.*f %s' % (precisionForDisplay, float(numBytes) / factor, suffix)

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

            tabLog = TableLogger().setWarnWithArrows(True)
            tabLog.info(["Down Segment", "Datadir", "Port"])
            for seg in downSegments:
                tabLog.info(self.__appendSegmentTripletToArray(seg, []))
            tabLog.outputTable()

        logger.info( "----------------------------------------------------------")

        return exitCode

    def __showPortInfo(self, gpEnv, gpArray):

        logger.info("-Master segment instance  %s  port = %d" % (gpEnv.getMasterDataDir(), gpEnv.getMasterPort()))
        logger.info("-Segment instance port assignments")
        logger.info("----------------------------------")

        tabLog = TableLogger().setWarnWithArrows(True)
        tabLog.info([ "Host", "Datadir", "Port"])
        for seg in gpArray.getSegDbList():
            tabLog.info(self.__appendSegmentTripletToArray(seg, []))
        tabLog.outputTable()

    def __showStandbyMasterInformation(self, gpEnv, gpArray):

        standby = gpArray.standbyMaster

        #
        # print standby configuration/status
        #
        if standby is None:
            logger.info("Standby master instance not configured")
        else:
            cmd = gp.GpGetSegmentStatusValues("get standby segment version status", [standby],
                               [gp.SEGMENT_STATUS__GET_PID], verbose=logging_is_verbose(), ctxt=base.REMOTE,
                               remoteHost=standby.getSegmentAddress())
            cmd.run()

            # fetch standby pid
            (standbyPidFetchWarning, outputFromCmd) = cmd.decodeResults()
            if standbyPidFetchWarning is None:
                pidData = outputFromCmd[standby.getSegmentDbId()][gp.SEGMENT_STATUS__GET_PID]
            else:
                pidData = {}
                pidData['pid'] = 0
                pidData['error'] = None

            # Print output!
            logger.info("Standby master details" )
            logger.info("----------------------" )
            tabLog = TableLogger().setWarnWithArrows(True)
            tabLog.info(["Standby address", "= %s" % standby.getSegmentAddress()])
            tabLog.info(["Standby data directory", "= %s" % standby.getSegmentDataDirectory()])
            tabLog.info(["Standby port", "= %s" % standby.getSegmentPort()])
            if standbyPidFetchWarning is not None:
                tabLog.warn(["Standby PID", "= %s" % standbyPidFetchWarning ])
                tabLog.warn(["Standby status", "= Status could not be determined"])
            elif pidData['pid'] == 0:
                tabLog.warn(["Standby PID", "= 0"])
                tabLog.warn(["Standby status", "= Standby process not running"])
            else:
                if pidData['error'] is not None:
                    #
                    # we got a pid value but had some kind of error -- so possibly the PID
                    #   is not actually active on its port.  Print the error
                    #
                    tabLog.warn(["Standby PID", "= %s" % pidData['pid'], "%s" % pidData['error']])
                    tabLog.warn(["Standby status", "= Status could not be determined" ])
                else:
                    tabLog.info(["Standby PID", "= %s" % pidData['pid']])
                    tabLog.info(["Standby status", "= Standby host passive" ])
            tabLog.outputTable()

        #
        # now print gp_master_mirroring table
        #

        logger.info("-------------------------------------------------------------" )
        logger.info("-gp_master_mirroring table" )
        logger.info("-------------------------------------------------------------" )

        dbUrl = dbconn.DbURL(port=gpEnv.getMasterPort(), dbname='template1')
        conn = dbconn.connect(dbUrl, utility=True)
        sql = "SELECT summary_state, detail_state, log_time, error_message FROM gp_master_mirroring"
        row = dbconn.execSQLForSingletonRow(conn, sql)
        row = ["" if val is None else val for val in row] # transform None to empty string
        conn.close()

        logger.info("-Summary state: %s" % row[0])
        logger.info("-Detail state: %s" % row[1])
        logger.info("-Log time: %s" % row[2])
        if row[3] != "":
            logger.info("-Error message: %s" % row[3])
        logger.info("-------------------------------------------------------------" )

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
                    version = outputFromCmd[seg.getSegmentDbId()][gp.SEGMENT_STATUS__GET_VERSION]
                    if version is not None:
                        dbIdToVersion[seg.getSegmentDbId()] = version
                        uniqueVersions[version] = True
            else:
                logger.warn(warning)

        # print the list of all segments and warnings about trouble
        tabLog = TableLogger().setWarnWithArrows(True)
        tabLog.info(["Host","Datadir", "Port", "Version", ""])
        for seg in segmentsAndMaster:
            line = self.__appendSegmentTripletToArray(seg, [])
            version = dbIdToVersion.get(seg.getSegmentDbId())
            if version is None:
                line.append("unable to retrieve version")
                tabLog.warn(line)
            else:
                line.append(version)
                tabLog.info(line)
        tabLog.outputTable()

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
                 (1 if self.__options.showStatus else 0) + \
                 (1 if self.__options.showStatusStatistics else 0) + \
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
            exitCode = self._showMirrorList(gpEnv, gpArray)
        elif self.__options.showClusterConfig:
            exitCode = self.__showClusterConfig(gpEnv, gpArray)
        elif self.__options.showQuickStatus:
            exitCode = self.__showQuickStatus(gpEnv, gpArray)
        elif self.__options.showStatusStatistics:
            exitCode = self.__showStatusStatistics(gpEnv, gpArray)
        elif self.__options.showStatus:
            exitCode = self.__showStatus(gpEnv, gpArray)
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
        addTo.add_option("-s", None, default=False, action="store_true",
                            dest="showStatus",
                            metavar="<showStatus>",
                            help="Show status")
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
        addTo.add_option("-b", None, default=False, action="store_true",
                         dest="showStatusStatistics",
                         metavar="<showStatusStatistics>",
                         help="Show status statistics")

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
    
        
    
