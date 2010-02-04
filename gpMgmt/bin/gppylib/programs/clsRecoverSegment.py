#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# todo MIRRORING get address of the new host (not just hostName) using gphostcache.py
#
# todo MIRRORING better error message when cluster is not running
# todo MIRRORING STOP the old mirror in case they are running? (when failover to a new location)
# todo MIRRORING checks for being able to build target directory
# todo MIRRORING what is: multi_home = gpmlib.chk_multi_home(hosts) (from other gprecoverseg)
# todo MIRRORING implement pause/unpause of fault prober properly
# todo MIRRORING update hba.conf on all originals if we changed any hosts: (call update_original_segments from gpexpand)
# todo MIRRORING is it okay to leave the temporary directory sitting around (as gpexpand seems to do?)
# todo MIRRORING need to compress arguments sent to segments?  I think command lines can be pretty long, but should test it
#
# todo MIRRORING we should copy postgresql.conf from some other segment rather than the master (see what gpexpand does) since settings can be different on segments
# todo MIRRORING: make sure created directories have the right permissions?
#
# todo MIRRORING why do we take recovery commands from a file?  Is that really needed?
# todo MIRRORING if from config file, we should NOT take -F and instead should get full/incremental commands from the file?
# todo MIRRORING I may have changed the semantics for 'spareDataDirectory'.  review old semantics
#
# todo MIRRORING what about chk_pool_dir_available from gprecoverseg?  Is that needed herE?
#
# todo code reorg and consolidation with gpexpand?
#
# todo: catch string-to-int and other errors when parsing file
#
# todo MIRRORING test cases where input file says to recover a file in-place (perhaps with a change to ports)
# todo MIRRORING (bugfix) escaping of directory names in unix.LocalCopy
# todo MIRRORING it's a little quirky that caller must set up failed/failover so that failover is in gparray but failed is not (if both set)...change that, or at least protect against problems
#
#
#
# Options removed when 3.4 gprecoverseg was implemented:
#        --version
#       -S "Primary segment dbid to force recovery": I think this is done now by bringing the primary down, waiting for
#           failover, and then doing recover full
#       -z "Primary segment data dir and host to force recovery" see removed -S option for comment
#       -f        : force Greenplum Database instance shutdown and restart
#       -F (HAS BEEN CHANGED) -- used to mean "force recovery" and now means "full recovery)
# And a change to the -i input file: it now takes replicationPort in list of args (for failover target)
#
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal

from gppylib import gparray, gplog, pgconf, userinput
from gppylib.db import catalog, dbconn
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.commands import base, gp, pg, unix
from gppylib.operations.startSegments import *
from gppylib.system import configurationInterface as configInterface
from gppylib.system.environment import GpMasterEnvironment
from gppylib.test.testUtils import *
from gppylib.utils import toNonNoneString, checkNotNone, readAllLinesFromFile

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
        if not liveSegment.isSegmentPrimary(True):
            raise ExceptionNoStackTraceNeeded("Segment to recover from for content %s is not a primary" % liveSegment.getSegmentContentId())
        if not liveSegment.isSegmentQE():
            raise ExceptionNoStackTraceNeeded("Segment to recover from for content %s is not a correct segment " \
                    "(it is a master or standby master)" % liveSegment.getSegmentContentId())
        if not liveSegment.isSegmentUp():
            raise ExceptionNoStackTraceNeeded("Primary segment is not up for content %s" % liveSegment.getSegmentContentId())
        if failedSegment.getSegmentContentId() != liveSegment.getSegmentContentId():
            raise ExceptionNoStackTraceNeeded("The primary is not of the same content as the mirror.  Primary content %d, " \
                    "mirror content %d" % (liveSegment.getSegmentContentId(), failedSegment.getSegmentContentId()))
        if failedSegment.getSegmentDbId() == liveSegment.getSegmentDbId():
            raise ExceptionNoStackTraceNeeded("For content %d, the dbid values are the same.  " \
                                "A segment may not be recovered from itself" % liveSegment.getSegmentDbId())
        if failedSegment is not None and failoverSegment is not None:
            # for now, we require the code to have produced this -- even when moving the segment to another
            #  location, we preserve the directory
            assert failedSegment.getSegmentDbId() == failoverSegment.getSegmentDbId()

        self.__failedSegment = failedSegment
        self.__liveSegment = liveSegment
        self.__failoverSegment = failoverSegment

        """
        __forceFullSynchronization is true if full resynchronization should be FORCED -- that is, the
           existing segment will be cleared and all objects will be transferred by the file resynchronization
           process on the server
        """
        self.__forceFullSynchronization = forceFullSynchronization

    def getFailedSegment(self):
        """
        returns the segment that failed. This will always be non-None
        """
        return self.__failedSegment

    def getLiveSegment(self):
        """
        returns the primary segment from which the recovery will take place.  Will always be non-None
        """
        return self.__liveSegment

    def getFailoverSegment(self):
        """
        returns the target segment to which we will copy the data, or None
            if we will recover in place.  Note that __failoverSegment should refer to the same dbid
            as __failedSegment, but should have updated path + file information.
        """
        return self.__failoverSegment

    def isFullSynchronization(self):
        """
        Returns whether or not this segment to recover needs to recover using full resynchronization
        """

        if self.__forceFullSynchronization:
            return True

        # if the segment we're copying from is marked full resync, we can't downgrade that to incremental!
        if self.__liveSegment.getSegmentMode() == gparray.MODE_FULL_RESYNCHRONIZATION:
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
            for segToRecover in actions.getSegmentsToRecover():
                seg = segToRecover.getFailedSegment()
                file.write('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

                seg = segToRecover.getFailoverSegment()
                if seg is not None:
                    file.write(' ')
                    file.write('%s:%d:%d:%s' % (seg.getSegmentHostName(),
                        seg.getSegmentPort(), seg.getSegmentReplicationPort(), seg.getSegmentDataDirectory()))

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
        destSegmentByHost = GpArray.getSegmentsByHostName(destSegments)
        newSegmentInfo = gp.buildExpansionSegmentInfo(destSegments)
        cmds = []
        for hostName, segments in destSegmentByHost.iteritems():
            segmentInfo = newSegmentInfo[hostName]
            checkNotNone("segmentInfo for %s" % hostName, segmentInfo)

            cmds.append(gp.ConfigExpansionSeg('gpexpand configure new segments',
                                            segmentInfo,
                                            tarFile=blankTarFile,
                                            newSegments=True,
                                            verbose=gplog.logging_is_verbose(),
                                            batchSize=self.__options.parallelDegree,
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

    def __checkForPortAndDirectoryConflicts(self, gpArray):
        for hostName, segmentArr in gpArray.getSegmentsByHostName(gpArray.getDbList()).iteritems():
            usedPorts = {}
            usedDataDirectories = {}
            for segment in segmentArr:

                # check for port conflict
                replicationPort = segment.getSegmentReplicationPort()
                port = segment.getSegmentPort()
                dbid = segment.getSegmentDbId()
                if port in usedPorts:
                    raise Exception("A port for segment with dbid %s conflicts with a port for segment dbid %s" \
                            % (dbid, usedPorts.get(port)))
                if replicationPort in usedPorts:
                    raise Exception("A port for segment with dbid %s conflicts with a port for segment dbid %s" \
                            % (dbid, usedPorts.get(replicationPort)))

                if port == replicationPort:
                    raise Exception("Segment with dbid %s has equal port and replication port" % dbid)
                usedPorts[port] = dbid
                usedPorts[replicationPort] = dbid

                # check for directory conflict
                if segment.getSegmentDataDirectory() in usedDataDirectories:
                    raise Exception("The directory for segment with dbid %s conflicts with the data directory for segment dbid %s"\
                            % (dbid, usedDataDirectories.get(port)))
                usedDataDirectories[segment.getSegmentDataDirectory()] = dbid

    
    def recover(self, gpEnv, gpArray, actions):
        assert self.__faultProber.isFaultProberPaused()
        testOutput("recovering %s segment(s)" % len(actions.getSegmentsToRecover()))

        if len(actions.getSegmentsToRecover()) == 0:
            logger.info("No segments to recover")
            return

        self.__checkForPortAndDirectoryConflicts(gpArray)

        logger.info("%s segment(s) to recover" % len(actions.getSegmentsToRecover()))

        # make sure the target directories are up-to-date
        #  by cleaning them, if needed, and then copying a basic directory there
        #  the postgresql.conf in that basic directory will need updating
        cleanupDirectives = []
        copyDirectives = []
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.isFullSynchronization():
                cleanupDirectives.append(GpCleanupSegmentDirectoryDirective(toRecover.getFailedSegment()))

                if toRecover.getFailoverSegment() is not None:
                    targetSegment = toRecover.getFailoverSegment()
                else: targetSegment = toRecover.getFailedSegment()

                d = GpCopySegmentDirectoryDirective(toRecover.getLiveSegment(), targetSegment)
                copyDirectives.append(d)
        self.__cleanUpSegmentDirectories(cleanupDirectives)
        self.__copySegmentDirectories(gpEnv, copyDirectives)

        # update and save metadata
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.isFullSynchronization():
                resyncMode = gparray.MODE_FULL_RESYNCHRONIZATION
            else:
                resyncMode = gparray.MODE_INCREMENTAL_RESYNCHRONIZATION

            if toRecover.getFailoverSegment() is None:
                # we are recovering the lost segment in place
                seg = toRecover.getFailedSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(resyncMode)
            else:
                seg = toRecover.getFailedSegment()
                # no need to update the failed segment's information -- it is
                #   being overwritten in the configuration with the failover segment
                for gpArraySegment in gpArray.getDbList():
                    if gpArraySegment is seg:
                        raise Exception("failed segment should not be in the new configuration if failing over to new segment")

                seg = toRecover.getFailoverSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(resyncMode)

            seg = toRecover.getLiveSegment()
            seg.setSegmentMode(resyncMode)

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

        failedSegments = []
        failoverSegments = []
        for line in readAllLinesFromFile(self.__options.recoveryConfigFile):
            line = line.strip()
            logger.info("Checking %s " % line)
            if line == '':
                continue
            qeArray = line.split()
            qeArrayLen = len(qeArray)
            if qeArrayLen < 1 or  qeArrayLen > 2:
                raise Exception('config file format error. %s' % line)

            arr = [s.strip() for s in qeArray[0].split(':')]
            if len(arr) != 3:
                raise Exception('config file format error. %s' % line)

            # find the failed segment
            (failedHostName, failedPort, failedDataDirectory) = arr
            failedSegment = None
            for segment in gpArray.getDbList():
                if segment.getSegmentHostName() == failedHostName and \
                            str(segment.getSegmentPort()) == failedPort and \
                            segment.getSegmentDataDirectory() == failedDataDirectory:

                    if failedSegment is not None:
                        raise Exception("Segment found twice: %s" % line)
                    failedSegment = segment

            if failedSegment is None:
                raise Exception("Segment not found: %s" % line )

            failoverSegment = None
            if qeArrayLen == 2:
                """
                When the length is two, the caller is going to tell us to where we need to failover, so
                  build a failover segment
                """
                arr = [s.strip() for s in qeArray[1].split(':')]
                if len(arr) != 4:
                    raise Exception('Config file format error. %s' % line)

                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()

                # now update values in failover segment
                failoverSegment.setSegmentHostName( arr[0] )
                failoverSegment.setSegmentPort( int(arr[1]) )
                failoverSegment.setSegmentReplicationPort( int(arr[2]) )
                failoverSegment.setSegmentDataDirectory( arr[3] )

            # this must come AFTER the if check above because failedSegment can be adjusted to
            #   point to a different object
            failedSegments.append(failedSegment)
            failoverSegments.append(failoverSegment)

        peersForFailedSegments = self.findAndValidatePeersForFailedSegments(gpArray, failedSegments)

        segs = []
        for i in range(len(failedSegments)):
            segs.append( GpSegmentToRecover(failedSegments[i], peersForFailedSegments[i], failoverSegments[i], \
                                self.__options.forceFullResynchronization))
        return GpRecoveryActions(segs)

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

        forceFull = self.__options.forceFullResynchronization
        segs = []
        for i in range(len(failedSegments)):

            failoverSegment = None
            failedSegment = failedSegments[i]
            liveSegment = peersForFailedSegments[i]

            if self.__options.newRecoverHost is not None:
                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()
                failoverSegment.setSegmentHostName( self.__options.newRecoverHost )

            if self.__options.spareDataDirectory is not None:
                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()
                failoverSegment.setSegmentDataDirectory( self.__options.spareDataDirectory )

            segs.append( GpSegmentToRecover(failedSegment, liveSegment, failoverSegment, forceFull ))

        return GpRecoveryActions(segs)

    def getRecoveryActionsBasedOnOptions(self, gpArray):
        if self.__options.recoveryConfigFile is not None:
            return self.getRecoveryActionsFromConfigFile(gpArray)
        else:
            return self.getRecoveryActionsFromConfiguration(gpArray)

    def displaySegmentInfo(self, instanceType, seg):
        paddingLen = 11 - len(instanceType)
        padding = ""
        for i in range(paddingLen):
            padding = padding + " "

        logger.info('%s instance host             %s= %s' % (instanceType, padding, seg.getSegmentHostName()))
        logger.info('%s instance directory        %s= %s' % (instanceType, padding, seg.getSegmentDataDirectory()))
        logger.info('%s instance port             %s= %d' % (instanceType, padding, seg.getSegmentPort()))
        logger.info('%s instance replication port %s= %d' % (instanceType, padding, seg.getSegmentReplicationPort()))

    def displayRecovery(self, actions):
        logger.info('Greenplum instance recovery parameters')
        logger.info('---------------------------------------------------------')

        if self.__options.recoveryConfigFile:
            logger.info('Recovery from configuration -i option supplied')
        elif self.__options.newRecoverHost is not None:
            logger.info('Recovery type             = Pool Host')
            logger.info('Pool host for recovery    = %s' % self.__options.newRecoverHost)
            type_text = 'Pool    '
        elif self.__options.spareDataDirectory is not None:
            logger.info('Recovery type             = Pool Directory')
            logger.info('Mirror pool directory     = %s' % self.__options.spareDataDirectory)
            type_text = 'Pool dir'
        else:
            logger.info('Recovery type             = Standard')
            type_text = 'Failed  '

        count = 0
        # logger.info('Recovery parallel batch value = %d' % opt['-B'])
        i = 0
        total = len(actions.getSegmentsToRecover())
        for toRecover in actions.getSegmentsToRecover():
            logger.info('---------------------------------------------------------')
            logger.info('Recovery %d of %d' % (i+1, total))
            logger.info('---------------------------------------------------------')

            syncMode = "Full" if toRecover.isFullSynchronization() else "Incremental"
            logger.info("Synchronization mode                  = %s" % syncMode)

            self.displaySegmentInfo("Failed", toRecover.getFailedSegment())
            self.displaySegmentInfo("Recovery", toRecover.getLiveSegment())

            if toRecover.getFailoverSegment() is not None:
                self.displaySegmentInfo("Failover", toRecover.getFailoverSegment())
            else:
                logger.info("Recover the failed segment in-place")

            i = i + 1

        logger.info('---------------------------------------------------------')

    def run(self):

        self.__pool = base.WorkerPool(numWorkers=16) # todo MIRRORING configure number of workers
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        # verify "where to recover" options
        optionCnt = 0
        if self.__options.newRecoverHost is not None:
            optionCnt += 1
        if self.__options.spareDataDirectory is not None:
            optionCnt += 1
        if self.__options.recoveryConfigFile is not None:
            optionCnt += 1
        if optionCnt > 1:
            raise ProgramArgumentValidationException(\
                    "Only one of -i, -p, -s may be specified")

        # if gpmlib.chk_on_passive_standby(masterPort): # todo: what is this doing?  Proper error message
         #    raise Exception('Unable to process requested command')

        # done basic checks, now load config
        self.__faultProber.pauseFaultProber()
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
        gpArray = confProvider.loadSystemConfig()

        # check that we actually have mirrors
        if gpArray.getFaultStrategy() != gparray.FAULT_STRATEGY_FILE_REPLICATION:
            raise ExceptionNoStackTraceNeeded( \
                    'GPDB Mirroring replication is not configured for this Greenplum Database instance.')

        # figure out what needs to be done
        actions = self.getRecoveryActionsBasedOnOptions(gpArray)


        if self.__options.outputSampleConfigFile is not None:
            # just output config file and done
            self.outputToFile(actions, self.__options.outputSampleConfigFile)
            logger.info('Configuration file output to %s successfully.' % self.__options.outputSampleConfigFile)
        else:

            self.__checkForPortAndDirectoryConflicts(gpArray)

            # going to do real work...so check with user
            if self.__options.interactive:
                # display parameter setting
                self.displayRecovery(actions)
                msg = "\nContinue with segment recovery procedure"
                if not userinput.ask_yesno(None,msg,'N'):
                    raise UserAbortedException()

            self.recover(gpEnv, gpArray, actions)

        self.__faultProber.unpauseFaultProber()
        return 0 # success -- exit code 0!

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("Recover a failed segment")
        help = [""]

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
        addTo.add_option("-B", None, type="int", default=16,
                            dest="parallelDegree",
                            metavar="<parallelDegree>",
                            help="Max # of workers to use for building recovery segments.  [default: %default]")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified", True)
        return GpRecoverSegmentProgram(options)
        
    
