#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# todo MIRRORING verify failover strategy is 'mirrored'
# todo MIRRORING better error message when cluster is not running
# todo MIRRORING STOP the old mirror in case they are running? (when failover to a new location)
# todo MIRRORING checks for being able to build target directory
# todo MIRRORING what is: multi_home = gpmlib.chk_multi_home(hosts) (from other gprecoverseg)
# todo MIRRORING do I need gp_fault_action?
# todo MIRRORING parallel degree sanity checks + defaults from gpstart
# todo MIRRORING is -F (force shutdown) required?
# todo MIRRORING set it up to show gprecoverseg_help
# todo MIRRORING implement pause/unpause of fault prober properly
# todo MIRRORING mark for full resync on the server side if needed
# todo MIRRORING rename ConfigExpansionSeg?
# todo MIRRORING update hba.conf on all originals if we changed any hosts: (call update_original_segments from gpexpand)
# todo MIRRORING is it okay to leave the temporary directory sitting around (as gpexpand does?)
# todo MIRRORING escaping of directory names in unix.LocalCopy
# todo MIRRORING need to compress arguments sent to segments?  I think command lines can be pretty long, but should test it
#
# todo MIRRORING we should copy postgresql.conf from some other statement rather than the master (see what gpexpand does) since settings can be different on segments
# todo MIRRORING: make sure created directories have the right permissions?
#
# todo code reorg and consolidation with gpexpand?
#
# note (MIRRORING): I removed some options from old gprecoverseg, including:
#        --version
#        todo MIRRORING fill out full list of removed options
#
# todo MIRRORING: more general way to send lots of commands to segments? (pass a list of segments, list of args, and set of class names to run on client?)
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
from gppylib.system.environment import GpMasterEnvironment

import gppylib.test.testUtils
import gppylib.commands.gp

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
        __failedSegment is the segment that failed and will always be non-none
        """
        self.__failedSegment = failedSegment

        """
        __liveSegment is the primary segment from which the recovery will take place
        """
        self.__liveSegment = liveSegment

        """
        __failoverSegment is the target segment to which we will copy the data, or None
            if we will recover in place.  Note that __failoverSegment should refer to the same dbid
            as __failedSegment, but should have updated path + file information.
        """
        self.__failoverSegment = failoverSegment

        """
        __forceFullSynchronization is true if full resynchronization should be FORCED -- that is, the
           existing segment will be cleared and all objects will be transferred by the file resynchronization
           process on the server
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

class GpCleanupSegmentDirectoryDirective:
    def __init__(self, segment):
        checkNotNone("segment", segment)
        self.__segment = segment

    def getSegment(self):
        return self.__segment

class GpCopySegmentDirectoryDirective:
    def __init__(self, source, dest ):
        """
        """
        checkNotNone("source", source)
        checkNotNone("dest", dest)

        self.__source = source
        self.__dest = dest

    def getSrcSegment(self):
        return self.__source

    def getDestSegment(self):
        return self.__dest

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

    def __createStartSegmentsOp(self, gpEnv):
        return StartSegmentsOperation(self.__pool, self.__options.quiet,
                gpEnv.getLocaleArray(), gpEnv.getGpVersion(),
                gpEnv.getGpHome(), gpEnv.getMasterDataDir()
                )

    def __startAll(self, gpEnv, gpArray, segments):
        segmentStartResult = self.__createStartSegmentsOp(gpEnv).startSegments(gpArray, segments, START_AS_PRIMARY_OR_MIRROR)

        for i in range(len(segmentStartResult.getFailedSegments())):
            failedSeg = segmentStartResult.getFailedSegments()[i]
            failureReason = segmentStartResult.getFailureReasons()[i]
            logger.warn("Failed to start segment.  The fault prober will shortly mark it as down. Segment: %s: REASON: %s" % (failedSeg, failureReason))
        pass

    def __convertAllPrimaries(self, gpEnv, gpArray, segments):
        segmentStartResult = self.__createStartSegmentsOp(gpEnv).transitionSegments(gpArray, segments, MIRROR_MODE_PRIMARY)
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

    def __runWaitAndCheckWorkerPoolForErrorsAndClear(self, cmds, actionVerb):
        for cmd in cmds:
            self.__pool.addCommand(cmd)

        self.__pool.wait_and_printdots(len(cmds), self.__options.quiet)
        self.__pool.check_results()
        self.__pool.empty_completed_items()

    def __copyFiles(self, srcDir, destDir, fileNames):
        for name in fileNames:
            cmd = gp.LocalCopy("copy file for segment", srcDir + "/" + name, destDir + "/" + name)
            cmd.run(validateAfter=True)

    def __createEmptyDirectories( self, dir, newDirectoryNames ):
        for name in newDirectoryNames:
            cmd = unix.MakeDirectory("create blank directory for segment", dir + "/" + name)
            cmd.run(validateAfter=True)

    def __buildTarFileForTransfer(self, gpEnv):
        """

        Returns the file for the tarfile that should be transferred and used
         for building the blank segment

        todo: MIRRORING does this need to be built on each primary because pg_control must be copied from the primary
              segment?  Or is it okay to start it up with a useless pg_control -- file replication code should
              overwrite it.
        """
        masterDir = gpEnv.getMasterDataDir()

        tempDir = gp.createTempDirectoryName(gpEnv.getMasterDataDir(), "gpbuildingmirrorsegment")
        unix.MakeDirectory("create temp directory for segment", tempDir ).run(validateAfter=True)

        schemaDir = tempDir + "/schema"
        unix.MakeDirectory("create temp schema directory for segment", schemaDir ).run(validateAfter=True)

        # todo MIRRORING build proper hba.conf on master before we make the mirrors
        # note that we could transfer the final pg_hba.conf here and then
        self.__createEmptyDirectories( schemaDir, ["base", "global", "pg_log"])
        self.__copyFiles(masterDir, schemaDir, ["PG_VERSION", "postgresql.conf", "pg_hba.conf", "global/pg_control"])

        tarFile = tempDir + "/schema.tar"

        cmd = gp.CreateTar('gpbuildingmirrorsegment tar segment template', schemaDir, tarFile)
        cmd.run(validateAfter=True)

        return (tempDir, tarFile)

    def __copySegmentDirectories(self, gpEnv, directives):
        """
        directives should be composed of GpCopySegmentDirectoryDirective values
        """
        if len(directives) == 0:
            return

        srcSegments = [d.getSrcSegment() for d in directives]
        srcSegmentByHost = GpArray.getSegmentsByHostName(srcSegments)

        (tempDir, blankTarFile) = self.__buildTarFileForTransfer(gpEnv)

        cmds = []
        for hostName, segments in srcSegmentByHost.iteritems():
            #
            # NOTE: copy set of files from the master itself to each target segment
            #
            cmds.append( gp.RemoteCopy("copy segment tar", blankTarFile, hostName, '.') )

        self.__runWaitAndCheckWorkerPoolForErrorsAndClear(cmds, "building and transferring basic segment directory")

        destSegments = [d.getDestSegment() for d in directives]
        destSegmentByHost = GpArray.getSegmentsByHostName(srcSegments)
        newSegmentInfo = gp.buildExpansionSegmentInfo(destSegments)
        cmds = []
        for hostName, segments in destSegmentByHost.iteritems():

            segmentInfo = newSegmentInfo[hostName]
            checkNotNone("segmentInfo for %s" % hostName, segmentInfo)

            #
            # todo MIRRORING: set batchSize according to argument?
            #
            cmds.append(gp.ConfigExpansionSeg('gpexpand configure new segments',
                                            segmentInfo,
                                            tarFile=blankTarFile,
                                            newSegments=True,
                                            verbose=gplog.logging_is_verbose(),
                                            batchSize=16,
                                            ctxt=gp.REMOTE, 
                                            remoteHost=hostName))
        self.__runWaitAndCheckWorkerPoolForErrorsAndClear(cmds, "unpacking basic segment directory")

        # todo: update hba conf on every existing segment as well


    def __cleanUpSegmentDirectories(self, directives):
        if len(directives) == 0:
            return

        segments = [d.getSegment() for d in directives]
        segmentByHost = GpArray.getSegmentsByHostName(segments)

        cmds = []
        for hostName, segments in segmentByHost.iteritems():
            cmds.append( gp.GpCleanSegmentDirectories("clean segment directories on %s" % hostName, \
                    segments, gp.REMOTE, hostName))

        self.__runWaitAndCheckWorkerPoolForErrorsAndClear(cmds, "cleaning existing directories")
    
    def recover(self, gpEnv, gpArray, actions):
        assert self.__faultProber.isFaultProberPaused()
        testOutput("recovering %s segment(s)" % len(actions.getSegmentsToRecover()))

        if len(actions.getSegmentsToRecover()) == 0:
            logger.info("No segments to recover")
            return

        logger.info("%s segment(s) to recover" % len(actions.getSegmentsToRecover()))

        # make sure the target directories are up-to-date
        #  by cleaning them, if needed, and then copying a basic directory there
        #  the postgresql.conf in that basic directory will need updating
        cleanupDirectives = []
        copyDirectives = []
        for toRec in actions.getSegmentsToRecover():
            if toRec.isFullSynchronization():
                cleanupDirectives.append(GpCleanupSegmentDirectoryDirective(toRec.getFailedSegment()))

                if toRec.getFailoverSegment() is not None:
                    targetSegment = toRec.getFailoverSegment()
                else: targetSegment = toRec.getFailedSegment()

                d = GpCopySegmentDirectoryDirective(toRec.getLiveSegment(), targetSegment)
                copyDirectives.append(d)
        self.__cleanUpSegmentDirectories(cleanupDirectives)
        self.__copySegmentDirectories(gpEnv, copyDirectives)

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
        self.__convertAllPrimaries(gpEnv, gpArray, primariesToConvert)

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
            segs.append( GpSegmentToRecover(failedSegments[i], peersForFailedSegments[i], None, \
                                self.__options.forceFullResynchronization))

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

        self.__pool = base.WorkerPool(numWorkers=16) # todo MIRRORING configure number of workers
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

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
            self.recover(gpEnv, gpArray, actions)

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
        addTo.add_option('-d', '--master_data_directory', type='string',
                            dest="masterDataDirectory",
                            metavar="<master data directory>",
                            help="Optional. The master host data directory. If not specified, the value set"\
                                "for $MASTER_DATA_DIRECTORY will be used.")

        addTo = OptionGroup(parser, "Recovery Source Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, type="string",
                            dest="recoveryConfigFile", 
                            metavar="<configFile>",
                            help="Recovery configuration file")
        addTo.add_option("-o", None, dest="outputSampleConfigFile",
                            metavar="<configFile>", type="string",
                            help="Sample configuration file name to output")
        addTo.add_option("-S", None, type="int",
                            dest="forceRecoverySegmentDbId",
                            metavar="<dbid>",
                            help="Primary segment dbid to force recovery")
        addTo.add_option("-z", None, type="string",
                            dest="forceRecoverySegmentDataDirAndHost",
                            metavar="<dataDirAndHost>",
                            help="Primary segment data dir and host to force recovery")

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-p", None, type="string",
                            dest="newRecoverHost",
                            metavar="<targetHost>",
                            help="Spare new host to which to recover segments")
        addTo.add_option("-s", None, type="string",
                            dest="spareDataDirectory", 
                            metavar="<targetDirectory>",
                            help="Spare data directories on current hosts")

        addTo = OptionGroup(parser, "Recovery Options")
        parser.add_option_group(addTo)
        addTo.add_option('-F', None, default=False, action='store_true',
                            dest="forceFullResynchronization",
                            metavar="<forceFullResynchronization>",
                            help="Force full segment resynchronization")
        addTo.add_option("-B", None,  type="int", default=4,
                            dest="parallelDegree",
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
        
    
