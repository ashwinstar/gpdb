#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# todo MIRRORING verify failover strategy is 'mirrored'
# todo MIRRORING better error message when cluster is not running
# todo MIRRORING STOP the old mirror in case they are running? (when failover to a new location)
# todo MIRRORING checks for being able to build target directory
# todo MIRRORING what is: multi_home = gpmlib.chk_multi_home(hosts) (from other gprecoverseg)
# todo MIRRORING do I need gp_fault_action
# todo MIRRORING parallel degree sanity checks + defaults from gpstart
# todo MIRRORING is -F (force shutdown) required?
# todo MIRRORING set it up to show gprecoverseg_help
# todo MIRRORING implement pause/unpause of fault prober properly
# todo MIRRORING mark for full resync (not just rsync) in some cases?
#
# note (MIRRORING): I removed some options from old gprecoverseg, including:
#        --version
#        todo: fill out full list
#
# todo MIRRORING: make the script do stuff on remote host (as is done with gpexpand).
# todo MIRRORING: general way to send lots of commands to segments?
#
# THIS IMPORT SHOULD COME FIRST
#
# import mainUtils FIRST to get python version check
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal

from gppylib.test.testUtils import testOutput
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.utils import toNonNoneString, checkNotNone
from gppylib import gplog
from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib.system import configurationInterface as configInterface
from gppylib import pgconf
from gppylib.operations.startSegments import *

import gppylib.test.testUtils

logger = gplog.get_default_logger()

class GpRecoveryActions:

    def __init__(self, segmentsToRecover):
        self.__segmentsToRecover = segmentsToRecover
        pass

    def getSegmentsToRecover(self):
        return self.__segmentsToRecover

class GpSegmentToRecover:

    def __init__(self, failedSegment, liveSegment, failoverSegment, forceFullSynchronization):
        checkNotNone("failedSegment", failedSegment)
        checkNotNone("liveSegment", liveSegment)
        checkNotNone("forceFullSynchronization", forceFullSynchronization)
        assert liveSegment.isSegmentUp()
        assert liveSegment.isSegmentPrimary(True)
        assert liveSegment.isSegmentQE()
        assert failedSegment.getSegmentContentId() == liveSegment.getSegmentContentId()
        assert failedSegment.getSegmentDbId() != liveSegment.getSegmentDbId()

        """
        __failedSegment is the segment that failed
        """
        self.__failedSegment = failedSegment

        """
        __liveSegment is the segment from which the recovery will take place
        """
        self.__liveSegment = liveSegment

        """
        __failoverSegment is the target segment to which we will copy the data, or None
            if we will recover in place.  Note that __failoverSegment should refer to the same dbid
            as __failedSegment, but should have updated path + file information.
        """
        self.__failoverSegment = failoverSegment

        """
        __forceFullSynchronization is true if full resynchronization should be FORCED
        """
        self.__forceFullSynchronization = forceFullSynchronization

    def getFailedSegment(self):
        return self.__failedSegment

    def getLiveSegment(self):
        return self.__liveSegment

    def getFailoverSegment(self):
        return self.__failoverSegment

    def isFullSynchronization(self):
        if self.__forceFullSynchronization:
            return True

        # if we are failing over to a new segment location then we must fully resync
        if self.__failoverSegment is not None:
            return True
        return False

class GpFaultProberInterface:

    def __init__(self):
        self.__isPaused = False

    def pauseFaultProber(self):
        assert not self.__isPaused
        self.__isPaused = True

    def unpauseFaultProber(self):
        assert self.__isPaused
        self.__isPaused = False

    def isFaultProberPaused(self):
        return self.__isPaused

class GpMasterEnvironment:

    def __init__(self, masterDataDir):

        if masterDataDir is None:
            self.__masterDataDir = gp.get_masterdatadir()
        else: self.__masterDataDir = masterDataDir

        logger.debug("Obtaining master's port from master data directory")
        pgconf_dict = pgconf.readfile(self.__masterDataDir + "/postgresql.conf")
        self.__masterPort = pgconf_dict.int('port')
        logger.debug("Read from postgresql.conf port=%s" % self.__masterPort)
        self.__masterMaxConnections = pgconf_dict.int('max_connections')
        logger.debug("Read from postgresql.conf max_connections=%s" % self.__masterMaxConnections)

        self.__gpHome = gp.get_gphome()
        self.__gpVersion = gp.GpVersion.local('local GP software version check',self.__gpHome)
        logger.info("local Greenplum Version: '%s'" % self.__gpVersion)

        # read collation settings from master
        dbUrl = dbconn.DbURL(port=self.__masterPort, dbname='template1')
        conn = dbconn.connect(dbUrl, utility=True)
        (self.__lcCollate, self.__lcMonetary, self.__lcNumeric) = catalog.getCollationSettings(conn)
        conn.close()

    def getGpHome(self): return self.__gpHome
    def getGpVersion(self): return self.__gpVersion
    def getLcCollate(self): return self.__lcCollate
    def getLcMonetary(self): return self.__lcMonetary
    def getLcNumeric(self): return self.__lcNumeric
    def getLocaleArray(self): return [self.__lcCollate, self.__lcMonetary, self.__lcNumeric]
    def getMasterDataDir(self): return self.__gpHome
    def getMasterMaxConnections(self) : return self.__masterMaxConnections
    def getMasterPort(self) : return self.__masterPort

class GpCleanupSegmentDirectoryDirective:
    def __init__(self, segment):
        checkNotNone("segment", segment)
        self.__segment = segment

    def getSegment(self):
        return self.__segment

class GpCopySegmentDirectoryDirective:
    def __init__(self, source, dest, includeSchema ):
        """
        includeSchema should only be True if copying offline
        """
        checkNotNone("source", source)
        checkNotNone("dest", dest)
        checkNotNone("includeSchema", includeSchema)

        self.__source = source
        self.__dest = dest
        self.__includeSchema = includeSchema

    def getSource(self):
        return self.__source

    def getDest(self):
        return self.__dest

    def getIncludeSchema(self):
        return self.__includeSchema


#-------------------------------------------------------------------------
class GpRecoverSegmentProgram:

#
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.__faultProber = GpFaultProberInterface()
        self.__options = options
        self.__pool = None

    def __createStartSegmentsOp(self):
        return StartSegmentsOperation(self.__pool, self.__options.quiet,
                gpEnv.getLocaleArray(), gpEnv.getGpVersion(),
                gpEnv.getGpHome(), gpEnv.getMasterDataDir()
                )

    def __startAll(self, gpEnv, gpArray, segments):
        segmentStartResult = self.__createStartSegmentsOp.startSegments(gpArray, segments, START_AS_PRIMARY_OR_MIRROR)

        for i in range(len(segmentStartResult.getFailedSegments())):
            failedSeg = segmentStartResult.getFailedSegments()[i]
            failureReason = segmentStartResult.getFailureReasons()[i]
            logger.warn("Failed to start segment.  The fault prober will shortly mark it as down again. Segment: %s: REASON: %s" % (failedSeg, failureReason))
        pass

    def __convertAllPrimaries(self, segments):
        segmentStartResult = self.__createStartSegmentsOp.transitionSegments(gpArray, segments, MIRROR_MODE_PRIMARY)
        for i in range(len(segmentStartResult.getFailedSegments())):
            failedSeg = segmentStartResult.getFailedSegments()[i]
            failureReason = segmentStartResult.getFailureReasons()[i]
            logger.warn("Failed to inform primary segment of updated mirroring state.  Segment: %s: REASON: %s" % (failedSeg, failureReason))

    def outputToFile(self, actions, fileName):
        with open(fileName, 'w') as file:
            for segToRecover in actions.getSegmentsToRecover:
                seg = segToRecover.getFailedSegment()
                file.write('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

                seg = segToRecover.getFailoverSegment()
                if seg is not None:
                    file.write(' ')
                    file.write('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

                file.write('\n')

    def __waitAndCheckWorkerPoolForErrorsAndClear(self, dispatchCount, actionVerb):

        self.__pool.wait_and_printdots(dispatchCount, self.__options.quiet)

        cmds=self.pool.getCompletedItems()
        for cmd in cmds:
            if cmd.get_results().rc == 0:
                cmdout = cmd.get_results().stdout
                lines=cmdout.split('\n')
            else:
                raise Exception("error in %s" % actionVerb)

        self.pool.empty_completed_items()

    def copySegmentDirectories(self, directives):
        """
        directives should be composed of GpCopySegmentDirectoryDirective values
        """
        if len(directives) == 0:
            return

        srcSegments = [d.getSrcSegment() for d in directives]
        srcSegmentByHost = GpArray.getSegmentsByHostName(srcSegments)

        dispatchCount = 0
        for hostName, segments in srcSegmentByHost.iteritems():
            #
            # todo MIRRORING call a remote script that will tar up the segment directory and transfer to the
            #       target host (see gpexpand code)
            #  (segcopy.py).
            # NOTE: it's possible that we can just copy from the master itself
            #
            cmd = dispatchCount
            dispatchCount += 1
            self.__pool.addCommand(cmd)

        self.__waitAndCheckWorkerPoolForErrorsAndClear(dispatchCount, "building and transferring basic segment directory")

        destSegments = [d.getDestSegment() for d in directives]
        destSegmentByHost = GpArray.getSegmentsByHostName(srcSegments)
        dispatchCount = 0
        for hostName, segments in destSegmentByHost.iteritems():
            #
            # todo MIRRORING call a remote script that will untar the transferred tar (see gpconfexpseg) and update ports
            #
            cmd = dispatchCount
            dispatchCount += 1
            self.__pool.addCommand(cmd)
        self.__waitAndCheckWorkerPoolForErrorsAndClear(dispatchCount, "unpacking basic segment directory")


    def cleanupSegmentDirectories(self, directives):
        if len(directives) == 0:
            return

        segments = [d.getSegment() for d in directives]
        segmentByHost = GpArray.getSegmentsByHostName(srcSegments)

        dispatchCount = 0
        for hostName, segments in segmentByHost.iteritems():
            #
            # todo MIRRORING call a remote script that will clean up the segment directories
            #
            cmd = dispatchCount
            dispatchCount += 1
            self.__pool.addCommand(cmd)

        self.__waitAndCheckWorkerPoolForErrorsAndClear(dispatchCount, "cleaning existing directories")
    
    def recover(self, gpArray, actions):
        assert self.__faultProber.isFaultProberPaused()
        testOutput("recovering %s segment(s)" % len(actions.getSegmentsToRecover()))

        logger.info("%s segment(s) to recover" % len(actions.getSegmentsToRecover()))

        # make sure the target directories are up-to-date
        cleanupDirectives = []
        copyDirectives = []
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.isFullSynchronization() is not None:
                cleanupDirectives.append(GpCleanupSegmentDirectoryDirective(toRecover.getFailedSegment()))
                copyDirectives.append(GpCopySegmentDirectoryDirective(toRecover.getLiveSegment(), toRecover.getFailoverSegment(), False))
        self.cleanupSegmentDirectories(cleanupDirectives)
        self.copySegmentDirectories(copyDirectives)

        # update and save metadata
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.getFailoverSegment() is None:
                # we are recovering the lost segment in place
                seg = toRecover.getFailedSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(gparray.MODE_RESYNCHRONIZING)
            else:
                seg = toRecover.getFailedSegment()
                # no need to update the failed segment's information -- it is
                #   being overwritten in the configuration with the failover segment
                for gpArraySegment in gpArray.getDbList():
                    if gpArraySegment is seg:
                        raise Exception("failed segment should not be in the new configuration if failing over to new segment")

                seg = toRecover.getFailoverSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(gparray.MODE_RESYNCHRONIZING)

            seg = toRecover.getLiveSegment()
            seg.setSegmentMode(gparray.MODE_RESYNCHRONIZING)

        configInterface.getConfigurationProvider().updateSystemConfig(gpArray)

        # stop/start new mirrors
        mirrorsToStart = []
        primariesToConvert = []
        for toRecover in actions.getSegmentsToRecover():
            seg = toRecover.getFailoverSegment()
            if seg is None:
                seg = toRecover.getFailedSegment() # we are recovering in place
            mirrorsToStart.append(seg)
            primariesToConvert.append(toRecover.getLiveSegment())

        self.__startAll(gpEnv, gpArray, mirrorsToStart)
        self.__convertAllPrimaries(primariesToConvert)

    def getRecoveryActionsFromConfigFile(self, gpArray):
        """
        getRecoveryActionsFromConfigFile

        returns a GpRecoveryActions object
        """
        raise Exception("not implemented")

    def findAndValidatePeersForFailedSegments(self, gpArray, failedSegments):
        dbIdToPeerMap = gpArray.getDbIdToPeerMap()
        peersForFailedSegments = [ dbIdToPeerMap.get(seg.getSegmentDbId()) for seg in failedSegments]
        for i in range(len(failedSegments)):
            peer = peersForFailedSegments[i]
            if peer is None:
                raise Exception("No peer found for dbid %s" % failedSegments[i].getSegmentDbId())
            elif peer.isSegmentDown():
                raise Exception("Both segments for content %s are down; Try restarting Greenplum DB and running %s again." %
                        (peer.getSegmentContentId(), getProgramName()))
        return peersForFailedSegments

    def getRecoveryActionsFromConfiguration(self, gpArray):
        """
        getRecoveryActionsFromConfiguration

        returns a GpRecoveryActions object
        """
        segments = gpArray.getSegDbList()

        failedSegments = [ seg for seg in segments if seg.isSegmentDown() ]
        peersForFailedSegments = self.findAndValidatePeersForFailedSegments(gpArray, failedSegments)

        segs = []
        for i in range(len(failedSegments)):
            segs.append( GpSegmentToRecover(failedSegments[i], peersForFailedSegments[i], None))

        # todo: use newRecoverHost or spareDataDirectory here to update failedSegments
        #       and pick new ports, etc.
        return GpRecoveryActions(segs)

    def getRecoveryActionsBasedOnOptions(self, gpArray):
        if self.__options.forceRecoverySegmentDataDirAndHost is not None:
            raise Exception("Not implemented") # todo MIRRORING: do we need to implement this?
        elif self.__options.forceRecoverySegmentDbId is not None:
            raise Exception("Not implemented") # todo MIRRORING: do we need to implement this?
        elif self.__options.recoveryConfigFile is not None:
            return self.getRecoveryActionsFromConfigFile(gpArray)
        else:
            return self.getRecoveryActionsFromConfiguration(gpArray)
            
    def run(self):

        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory)

        # verify "what to recovery" options: note that specifying NONE is okay as well
        if self.__options.forceRecoverySegmentDataDirAndHost is not None and \
            self.__options.forceRecoverySegmentDbId is not None and \
            self.__options.recoveryConfigFile is not None:
            raise ProgramArgumentValidationException(\
                    "Only one of -i, -z, and -S may be specified")

        # verify "where to recover" options
        if self.__options.newRecoverHost is not None and \
            self.__options.spareDataDirectory is not None:
            raise ProgramArgumentValidationException(\
                    "Only one of -p and -s may be specified")

        # if gpmlib.chk_on_passive_standby(masterPort): # todo: what is this doing?  Proper error message
         #    raise Exception('Unable to process requested command')

        # done basic checks, now load config
        self.__faultProber.pauseFaultProber()
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
        gpArray = confProvider.loadSystemConfig()

        # check that we actually have mirrors
        if len([seg for seg in gpArray.getSegDbList() if seg.getSegmentPreferredRole() == 'm']) == 0 :
            raise ExceptionNoStackTraceNeeded( \
                    'There are no mirrors configured for this Greenplum Datanase instance' \
                    'The Greenplum Database instance will need to be recovered manually')

        # figure out what needs to be done
        actions = self.getRecoveryActionsBasedOnOptions(gpArray)

        # prompt user, if desired
        if self.__options.interactive:
            # todo: prompt user
            pass

        if self.__options.outputSampleConfigFile is not None:
            self.outputToFile(actions, self.__options.outputSampleConfigFile)
        else:
            self.recover(gpArray, actions)

        self.__faultProber.unpauseFaultProber()
        return 0 # success -- exit code 0!

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("""
        Recover a failed segment
        """)

        help = ["""
        """]

        parser = OptParser(option_class=OptChecker,
                    description=' '.join(description.split()),
                    version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = OptionGroup(parser, "Connection Options")
        parser.add_option_group(addTo)
        addTo.add_option('-d', '--master_data_directory', dest="masterDataDirectory",
                         type='string', default=None, metavar="<master data directory>",
                       help="Optional. The master host data directory. If not specified, the value set"\
                            "for $MASTER_DATA_DIRECTORY will be used.")

        addTo = OptionGroup(parser, "Recovery Source Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, dest="recoveryConfigFile", type="string",
                         metavar="<configFile>",
                      help="Recovery configuration file")
        addTo.add_option("-o", None, dest="outputSampleConfigFile",
                         metavar="<configFile>", type="string",
                      help="Sample configuration file name to output")
        addTo.add_option("-S", None, dest="forceRecoverySegmentDbId",
                         metavar="<dbid>", type="int",
                     help="Primary segment dbid to force recovery")
        addTo.add_option("-z", None, dest="forceRecoverySegmentDataDirAndHost",
                         metavar="<dataDirAndHost>", type="string",
                     help="Primary segment data dir and host to force recovery")

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-p", None, dest="newRecoverHost", type="string",
                         metavar="<targetHost>",
                      help="Spare new host to which to recover segments")
        addTo.add_option("-s", None, dest="spareDataDirectory", type="string",
                         metavar="<targetDirectory>",
                      help="Spare data directories on current hosts")

        addTo = OptionGroup(parser, "Recovery Options")
        parser.add_option_group(addTo)
        addTo.add_option('-F', None, action='store_true', default=False,
                         metavar="<forceFullResynchronization>",
                      help="Force full segment resynchronization")
        addTo.add_option("-B", None, dest="parallelDegree", type="int", default=4,
                         metavar="<parallelDegree>",
                      help="Max # of workers to use for recovering segments")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified") # todo MIRRORING: print help instead (as gpstart)
        return GpRecoverSegmentProgram(options)




