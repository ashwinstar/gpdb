import os, pickle

from gppylib.mainUtils import *

from gppylib.utils import checkNotNone
from gppylib import gparray, gplog
from gppylib.gplog import *
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import base
from gppylib.gparray import GpArray
from gppylib import gphostcache
from gppylib.test.testUtils import *
from gppylib.operations import startSegments

logger = get_default_logger()

#
# todo MIRRORING pg_hba.conf updates
# todo MIRRORING search for the following and see if we apply this fix: logger.debug('Sleeping for 1 second to allow sshd to catch up for host %s' % hostname)
#

class GpMirrorToBuild:

    def __init__(self, failedSegment, liveSegment, failoverSegment, forceFullSynchronization):
        checkNotNone("liveSegment", liveSegment)
        checkNotNone("forceFullSynchronization", forceFullSynchronization)

        if failedSegment is None and failoverSegment is None:
            raise Exception( "No mirror passed to GpMirrorToBuild")

        if not liveSegment.isSegmentPrimary(True):
            raise ExceptionNoStackTraceNeeded("Segment to recover from for content %s is not a primary" % liveSegment.getSegmentContentId())
        if not liveSegment.isSegmentQE():
            raise ExceptionNoStackTraceNeeded("Segment to recover from for content %s is not a correct segment " \
                    "(it is a master or standby master)" % liveSegment.getSegmentContentId())
        if not liveSegment.isSegmentUp():
            raise ExceptionNoStackTraceNeeded("Primary segment is not up for content %s" % liveSegment.getSegmentContentId())

        if failedSegment is not None:
            if failedSegment.getSegmentContentId() != liveSegment.getSegmentContentId():
                raise ExceptionNoStackTraceNeeded("The primary is not of the same content as the failed mirror.  Primary content %d, " \
                        "mirror content %d" % (liveSegment.getSegmentContentId(), failedSegment.getSegmentContentId()))
            if failedSegment.getSegmentDbId() == liveSegment.getSegmentDbId():
                raise ExceptionNoStackTraceNeeded("For content %d, the dbid values are the same.  " \
                                    "A segment may not be recovered from itself" % liveSegment.getSegmentDbId())

        if failoverSegment is not None:
            if failoverSegment.getSegmentContentId() != liveSegment.getSegmentContentId():
                raise ExceptionNoStackTraceNeeded("The primary is not of the same content as the mirror.  Primary content %d, " \
                        "mirror content %d" % (liveSegment.getSegmentContentId(), failoverSegment.getSegmentContentId()))
            if failoverSegment.getSegmentDbId() == liveSegment.getSegmentDbId():
                raise ExceptionNoStackTraceNeeded("For content %d, the dbid values are the same.  " \
                                    "A segment may not be built from itself" % liveSegment.getSegmentDbId())

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
        returns the segment that failed. This can be None, for example when adding mirrors
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

        # if we are failing over to a new segment location then we must fully resync
        if self.__failoverSegment is not None:
            return True

        return False


class GpMirrorListToBuild:
    def __init__(self, toBuild, pool, quiet, parallelDegree):
        self.__mirrorsToBuild = toBuild
        self.__pool = pool
        self.__quiet = quiet
        self.__parallelDegree = parallelDegree
        pass

    def getMirrorsToBuild(self):
        """
        Returns a newly allocated list
        """
        return [m for m in self.__mirrorsToBuild]

    def buildMirrors(self, actionName, gpEnv, gpArray):
        """
        Build the mirrors.

        gpArray must have already been altered to have updated directories -- that is, the failoverSegments
            from the mirrorsToBuild must be present in gpArray.

        """
        assert faultProberInterface.getFaultProber().isFaultProberPaused()
        testOutput("building %s segment(s)" % len(self.__mirrorsToBuild))

        if len(self.__mirrorsToBuild) == 0:
            logger.info("No segments to " + actionName)
            return

        self.checkForPortAndDirectoryConflicts(gpArray)

        logger.info("%s segment(s) to %s" % (len(self.__mirrorsToBuild), actionName))

        # make sure the target directories are up-to-date
        #  by cleaning them, if needed, and then copying a basic directory there
        #  the postgresql.conf in that basic directory will need updating
        cleanupDirectives = []
        copyDirectives = []
        for toRecover in self.__mirrorsToBuild:
            if toRecover.isFullSynchronization():
                if toRecover.getFailedSegment() is not None:
                    cleanupDirectives.append(GpCleanupSegmentDirectoryDirective(toRecover.getFailedSegment()))

                if toRecover.getFailoverSegment() is not None:
                    targetSegment = toRecover.getFailoverSegment()
                else: targetSegment = toRecover.getFailedSegment()

                d = GpCopySegmentDirectoryDirective(toRecover.getLiveSegment(), targetSegment)
                copyDirectives.append(d)
        self.__cleanUpSegmentDirectories(cleanupDirectives)
        self.__copySegmentDirectories(gpEnv, copyDirectives)

        # update and save metadata
        for toRecover in self.__mirrorsToBuild:

            if toRecover.getFailoverSegment() is None:
                # we are recovering the lost segment in place
                seg = toRecover.getFailedSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(gparray.MODE_RESYNCHRONIZATION)
            else:
                seg = toRecover.getFailedSegment()
                # no need to update the failed segment's information -- it is
                #   being overwritten in the configuration with the failover segment
                for gpArraySegment in gpArray.getDbList():
                    if gpArraySegment is seg:
                        raise Exception("failed segment should not be in the new configuration if failing over to new segment")

                seg = toRecover.getFailoverSegment()
                seg.setSegmentStatus(gparray.STATUS_UP)
                seg.setSegmentMode(gparray.MODE_RESYNCHRONIZATION)

            seg = toRecover.getLiveSegment()
            seg.setSegmentMode(gparray.MODE_RESYNCHRONIZATION)

        configInterface.getConfigurationProvider().updateSystemConfig(gpArray)

        # stop/start new mirrors
        mirrorsToStart = []
        primariesToConvert = []
        convertPrimaryUsingFullResync = []
        for toRecover in self.__mirrorsToBuild:
            seg = toRecover.getFailoverSegment()
            if seg is None:
                seg = toRecover.getFailedSegment() # we are recovering in place
            mirrorsToStart.append(seg)
            primariesToConvert.append(toRecover.getLiveSegment())
            convertPrimaryUsingFullResync.append(toRecover.isFullSynchronization())


        self.__startAll(gpEnv, gpArray, mirrorsToStart)
        self.__convertAllPrimaries(gpEnv, gpArray, primariesToConvert, convertPrimaryUsingFullResync)

    def checkForPortAndDirectoryConflicts(self, gpArray):
        """
        Check gpArray for internal consistency -- no duplicate ports or directories on the same host, for example

        A detected problem causes an Exception to be raised
        """

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

    def __runWaitAndCheckWorkerPoolForErrorsAndClear(self, cmds, actionVerb):
        for cmd in cmds:
            self.__pool.addCommand(cmd)

        self.__pool.wait_and_printdots(len(cmds), self.__quiet)
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
                                            batchSize=self.__parallelDegree,
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

    def __createStartSegmentsOp(self, gpEnv):
        return startSegments.StartSegmentsOperation(self.__pool, self.__quiet,
                gpEnv.getLocaleArray(), gpEnv.getGpVersion(),
                gpEnv.getGpHome(), gpEnv.getMasterDataDir()
                )

    def __startAll(self, gpEnv, gpArray, segments):
        segmentStartResult = self.__createStartSegmentsOp(gpEnv).startSegments(gpArray, segments, startSegments.START_AS_PRIMARY_OR_MIRROR)

        for i in range(len(segmentStartResult.getFailedSegments())):
            failedSeg = segmentStartResult.getFailedSegments()[i]
            failureReason = segmentStartResult.getFailureReasons()[i]
            logger.warn("Failed to start segment.  The fault prober will shortly mark it as down. Segment: %s: REASON: %s" % (failedSeg, failureReason))
        pass

    def __convertAllPrimaries(self, gpEnv, gpArray, segments, convertUsingFullResync):
        segmentStartResult = self.__createStartSegmentsOp(gpEnv).transitionSegments(gpArray, segments, convertUsingFullResync, startSegments.MIRROR_MODE_PRIMARY)
        for i in range(len(segmentStartResult.getFailedSegments())):
            failedSeg = segmentStartResult.getFailedSegments()[i]
            failureReason = segmentStartResult.getFailureReasons()[i]
            logger.warn("Failed to inform primary segment of updated mirroring state.  Segment: %s: REASON: %s" % (failedSeg, failureReason))



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
