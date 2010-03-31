import os, pickle

from gppylib.mainUtils import *

from gppylib.utils import checkNotNone, appendNewEntriesToHbaFile
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
# note: it's a little quirky that caller must set up failed/failover so that failover is in gparray but
#                                 failed is not (if both set)...change that, or at least protect against problems
#
# todo update hba.conf in existing segments as well
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
        #  the postgresql.conf in that basic directory will need updating (to change the port)
        toStopDirectives = []
        cleanupDirectives = []
        copyDirectives = []
        for toRecover in self.__mirrorsToBuild:

            if toRecover.getFailedSegment() is not None:
                # will stop the failed segment.  Note that we do this even if we are recovering to a different location!
                toStopDirectives.append(GpStopSegmentDirectoryDirective(toRecover.getFailedSegment()))

            if toRecover.isFullSynchronization():
                if toRecover.getFailedSegment() is not None and \
                    toRecover.getFailoverSegment() is None:
                    #
                    # We are recovering a failed segment in-place
                    #
                    cleanupDirectives.append(GpCleanupSegmentDirectoryDirective(toRecover.getFailedSegment()))

                if toRecover.getFailoverSegment() is not None:
                    targetSegment = toRecover.getFailoverSegment()
                else: targetSegment = toRecover.getFailedSegment()

                d = GpCopySegmentDirectoryDirective(toRecover.getLiveSegment(), targetSegment)
                copyDirectives.append(d)

        self.__ensureStopped(gpEnv, toStopDirectives)
        self.__cleanUpSegmentDirectories(cleanupDirectives)
        self.__copySegmentDirectories(gpEnv, gpArray, copyDirectives)

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

        # should use mainUtils.getProgramName but I can't make it work!
        programName = os.path.split(sys.argv[0])[-1]
        configInterface.getConfigurationProvider().updateSystemConfig(gpArray, "%s: segment resync" % programName )

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

    def __runWaitAndCheckWorkerPoolForErrorsAndClear(self, cmds, actionVerb, suppressErrorCheck=False):
        for cmd in cmds:
            self.__pool.addCommand(cmd)

        self.__pool.wait_and_printdots(len(cmds), self.__quiet)
        if not suppressErrorCheck:
            self.__pool.check_results()
        self.__pool.empty_completed_items()

    def __copyFiles(self, srcDir, destDir, fileNames):
        for name in fileNames:
            cmd = gp.LocalCopy("copy file for segment", srcDir + "/" + name, destDir + "/" + name)
            cmd.run(validateAfter=True)

    def __createEmptyDirectories( self, dir, newDirectoryNames ):
        for name in newDirectoryNames:
            subDir = os.path.join(dir, name)
            unix.MakeDirectory("create blank directory for segment", subDir).run(validateAfter=True)
            unix.Chmod.local('set permissions on blank dir', subDir, '0700')

    def __buildTarFileForTransfer(self, gpEnv, masterSegment, sampleSegment, newSegments):
        """
        Returns the file for the tarfile that should be transferred and used
         for building the blank segment

        """
        masterDir = gpEnv.getMasterDataDir()

        # note that this tempdir will be left around on the system (this is what other scripts do currently)
        tempDir = gp.createTempDirectoryName(gpEnv.getMasterDataDir(), "gpbuildingsegment")
        unix.MakeDirectory("create temp directory for segment", tempDir ).run(validateAfter=True)

        schemaDir = tempDir + "/schema"
        unix.MakeDirectory("create temp schema directory for segment", schemaDir ).run(validateAfter=True)
        unix.Chmod.local('set permissions on schema dir', schemaDir, '0700') # set perms so postgres can start

        #
        # Copy remote files from the sample segment to the master
        #
        logger.info('Copying postgresql.conf from existing segment into template')
        for toCopyFromRemote in ["postgresql.conf", "pg_hba.conf"]:
            cmd = gp.RemoteCopy('copying %s from a segment' % toCopyFromRemote,
                               sampleSegment.getSegmentDataDirectory() + '/' + toCopyFromRemote,
                               masterSegment.getSegmentHostName(), schemaDir, ctxt=base.REMOTE,
                               remoteHost=sampleSegment.getSegmentAddress())
            cmd.run(validateAfter=True)
        appendNewEntriesToHbaFile( schemaDir + "/pg_hba.conf", newSegments)

        #
        # Use the master's version of other files, and build
        #
        self.__createEmptyDirectories( schemaDir, [
                # this list (with some small differences) also occurs in initdb.c
                "global",
                "pg_log",
                "pg_xlog",
                "pg_clog",
                "pg_changetracking",
                "pg_subtrans",
                "pg_twophase",
                "pg_multixact",
                "pg_multixact/members",
                "pg_multixact/offsets",
                "pg_distributedxidmap",
                "pg_distributedlog",
                "pg_utilitymodedtmredo",
                "base",
                "pg_tblspc",
                "pg_stat_tmp"
                ] )
        self.__copyFiles(masterDir, schemaDir, ["PG_VERSION", "pg_ident.conf"])


        #
        # Build final tar
        #
        tarFile = tempDir + "/schema.tar"
        cmd = gp.CreateTar('gpbuildingmirrorsegment tar segment template', schemaDir, tarFile)
        cmd.run(validateAfter=True)

        return (tempDir, tarFile)

    def __copySegmentDirectories(self, gpEnv, gpArray, directives):
        """
        directives should be composed of GpCopySegmentDirectoryDirective values
        """
        if len(directives) == 0:
            return

        srcSegments = [d.getSrcSegment() for d in directives]
        destSegments = [d.getDestSegment() for d in directives]
        destSegmentByHost = GpArray.getSegmentsByHostName(destSegments)
        newSegmentInfo = gp.buildExpansionSegmentInfo(destSegments)

        (tempDir, blankTarFile) = self.__buildTarFileForTransfer(gpEnv, gpArray.master, srcSegments[0], destSegments)

        cmds = []
        for hostName, segments in destSegmentByHost.iteritems():
            #
            # NOTE: copy set of files from the master itself to each target segment
            #
            cmds.append( gp.RemoteCopy("copy segment tar", blankTarFile, hostName, '.') )

        self.__runWaitAndCheckWorkerPoolForErrorsAndClear(cmds, "building and transferring basic segment directory")

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

    def __ensureStopped(self, gpEnv, directives):
        """

        @param directives a list of the GpStopSegmentDirectoryDirective values indicating which segments to stop

        """
        if len(directives) == 0:
            return

        logger.info("Ensuring %d failed segment(s) are stopped" % (len(directives)))
        segments = [d.getSegment() for d in directives]
        segmentByHost = GpArray.getSegmentsByHostName(segments)

        cmds = []
        for hostName, segments in segmentByHost.iteritems():
            cmd=gp.GpSegStopCmd("remote segment stop on host '%s'" % hostName,
                                gpEnv.getGpHome(), gpEnv.getGpVersion(),
                                mode='fast', dbs=segments, verbose=logging_is_verbose(),
                                ctxt=base.REMOTE, remoteHost=hostName)

            cmds.append( cmd)

        # we suppress checking for the error.  This is because gpsegstop will actually error
        #  in many cases where the stop is actually done (that is, for example, the segment is
        #  running but slow to shutdown so gpsegstop errors after whacking it with a kill)
        #
        # Perhaps we should make it so that it so that is checks if the seg is running and only attempt stop
        #  if it's running?  In that case, we could propagate the error
        #
        self.__runWaitAndCheckWorkerPoolForErrorsAndClear(cmds, "stopping segments", suppressErrorCheck=True)

    def __cleanUpSegmentDirectories(self, directives):
        if len(directives) == 0:
            return

        logger.info("Cleaning files from %d segment(s)" % (len(directives)))
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

class GpStopSegmentDirectoryDirective:
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
