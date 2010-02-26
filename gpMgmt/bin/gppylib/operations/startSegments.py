import os, pickle

from gppylib.utils import checkNotNone
from gppylib.gplog import *
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import base
from gppylib.gparray import GpArray
from gppylib import gphostcache
from gppylib.commands.gp import SEGMENT_TIMEOUT_DEFAULT

logger = get_default_logger()

START_AS_PRIMARY_OR_MIRROR = "primaryOrMirror"
START_AS_MIRRORLESS = "asMirrorless"

MIRROR_MODE_MIRROR = "mirror"
MIRROR_MODE_MIRRORLESS = "mirrorless"
MIRROR_MODE_PRIMARY = "primary"
MIRROR_MODE_QUIESCENT = "quiescent"

#
# Note that a failed segments may still be up -- it could be that the conversion to mirror/primary mode failed
#
class StartSegmentsResult:
    def __init__(self):
        self.__successfulSegments = []
        self.__failedSegments = []
        self.__failureReasons = []
        pass

    def getSuccessfulSegments(self):
        return [x for x in self.__successfulSegments]

    def getFailedSegments(self):
        """
        These failed segments will be in parallel with the results of getFailureReasons
        """
        return [x for x in self.__failedSegments ]

    def getFailureReasons(self):
        """
        These failure reasons will be in parallel with the results of getFailedSegments
        """
        return [x for x in self.__failureReasons ]

    def addSuccess(self, segment):
        self.__successfulSegments.append(segment)

    def addFailure(self, segment, reason):
        self.__failedSegments.append(segment)
        self.__failureReasons.append(reason)

    def clearSuccessfulSegments(self):
        self.__successfulSegments = []

class StartSegmentsOperation:
    """
       This operation, to be run from the master, will start the segments up
            and, if necessary, convert them to the proper mode

       Note that this can be used to start only a subset of the segments 

    """

    def __init__(self, workerPool, quiet, localeData, gpVersion, 
                 gpHome, masterDataDirectory, timeout=SEGMENT_TIMEOUT_DEFAULT):
        checkNotNone("workerPool", workerPool)
        self.__workerPool = workerPool
        self.__quiet = quiet
        self.__localeData = localeData
        self.__gpVersion = gpVersion
        self.__gpHome = gpHome
        self.__masterDataDirectory = masterDataDirectory
        self.__timeout = timeout

    def startSegments(self, gpArray, segments, startMethod):
        """

        gpArray: the system to which the segments to start belong
        segments: the list of segments to be started
        startMethod: should be one of the START_AS_* constant values

        """
        totalToAttempt = len(segments)
        result = StartSegmentsResult()
        numContentsInCluster = gpArray.getNumSegmentContents()

        # check on pingability
        self.hostcache = gphostcache.GpHostCache(gpArray, self.__workerPool)
        failedPings=self.hostcache.ping_hosts(self.__workerPool)
        failedPingDbIds = {}
        for segment in failedPings:
            hostName = segment.getSegmentHostName()
            logger.warning("Skipping startup of segdb on %s directory %s Ping Failed <<<<<<" % \
                                        (hostName, segment.getSegmentDataDirectory()))
            result.addFailure(segment, 'Failed to Ping on host: %s' % hostName)
            failedPingDbIds[segment.getSegmentDbId()] = True

        self.hostcache.log_contents()

        segments = [seg for seg in segments if failedPingDbIds.get(seg.getSegmentDbId()) is None]

        # now do the start!
        if startMethod == START_AS_PRIMARY_OR_MIRROR:
            self.__runStartCommand(segments, MIRROR_MODE_QUIESCENT , numContentsInCluster, result)

            mirrorDbs = [seg for seg in result.getSuccessfulSegments() if seg.isSegmentMirror(True)]
            primaryDbs = [seg for seg in result.getSuccessfulSegments() if seg.isSegmentPrimary(True)]

            result.clearSuccessfulSegments()
            self.__sendPrimaryMirrorTransition(MIRROR_MODE_MIRROR, mirrorDbs, None, gpArray, result)
            self.__sendPrimaryMirrorTransition(MIRROR_MODE_PRIMARY, primaryDbs, None, gpArray, result)

        elif startMethod == START_AS_MIRRORLESS:
        # bring them up in mirrorless mode
            self.__runStartCommand(segments, MIRROR_MODE_MIRRORLESS, numContentsInCluster, result)
        else:
            raise Exception("Invalid startMethod %s" % startMethod)

        # done!
        assert totalToAttempt == len(result.getFailedSegments()) + len(result.getSuccessfulSegments())
        return result

    def transitionSegments(self, gpArray, segments, convertUsingFullResync, mirrorMode):
        """
        gpArray: the system to which the segments to start belong
        segments: the list of segments to be transitioned
        convertUsingFullResync: in parallel with segments, gives true/false for whether fullResync flag should be passed
        mirrorMode: should be one of the MIRROR_MODE_* constant values

        """

        result = StartSegmentsResult()
        self.__sendPrimaryMirrorTransition(mirrorMode, segments, convertUsingFullResync, gpArray, result)
        
        assert len(segments) == len(result.getFailedSegments()) + len(result.getSuccessfulSegments())
        return result

    def __runStartCommand(self, segments, mirroringMode, numContentsInCluster, resultOut):
        """
        Returns a StartSegmentsResult object
        """
        logger.info("Commencing parallel segment instance startup, please wait...")
        dispatchCount=0

        # launch the start
        for hostName, segments in GpArray.getSegmentsByHostName(segments).iteritems():
            logger.debug("Dispatching command to start segments on host: %s, " \
                            "with %s contents in cluster" % (hostName, numContentsInCluster))
            cmd = gp.GpSegStartCmd("remote segment starts on host '%s'" %
            hostName, self.__gpHome, segments, self.__localeData,
                                   self.__gpVersion,
                                   mirroringMode,
                                   numContentsInCluster,
                                   self.__timeout,
                                   verbose=logging_is_verbose(),
                                   ctxt=base.REMOTE,
                                   remoteHost=hostName)
            self.__workerPool.addCommand(cmd)
            dispatchCount+=1

        self.__workerPool.wait_and_printdots(dispatchCount, self.__quiet)

        # process results
        self.__processStartOrConvertCommands(resultOut)
        self.__workerPool.empty_completed_items()

    def __processStartOrConvertCommands(self, resultOut):
        logger.info("Process results...")
        for cmd in self.__workerPool.getCompletedItems():
            if cmd.get_results().rc == 0 or cmd.get_results().rc == 1:
            # error code 0 mean all good, 1 means it ran but at least one thing failed
                cmdout = cmd.get_results().stdout
                lines=cmdout.split('\n')

                for line in lines:
                    if line.startswith("STATUS"):
                        fields=line.split('--')
                        dir = fields[1].split(':')[1]
                        started = fields[2].split(':')[1]

                        # The funny join and splits are because Reason could have colons or -- in the text itself
                        reasonStr = "--".join(fields[3:])
                        reasonArr = reasonStr.split(':')
                        reasonArr = reasonArr[1:]
                        reasonStr = ":".join(reasonArr)

                        if started.lower() == 'false':
                            success=False
                        else:
                            success=True

                        for segment in cmd.dblist:
                            if segment.getSegmentDataDirectory() == dir:
                                if success:
                                    resultOut.addSuccess(segment)
                                else:
                                    resultOut.addFailure(segment, reasonStr)
            else:
                for segment in cmd.dblist:
                    resultOut.addFailure(segment, cmd.get_results())

    def __sendPrimaryMirrorTransition(self, targetMode, segments, convertUsingFullResync, gpArray, resultOut):
        """
            @param segments the segments to convert
            @param convertUsingFullResync in parallel with segments, may be None, gives true/false for whether fullResync
                                          flag should be passed to the transition
        """

        if len(segments) == 0:
            logger.debug("%s conversion of zero segments...skipping" % targetMode)
            return

        logger.info("Commencing parallel %s conversion of %s segments, please wait..." % (targetMode, len(segments)))

        dbIdToFullResync = {}
        if convertUsingFullResync is not None:
            for i in range(len(segments)):
                if convertUsingFullResync[i]:
                    dbIdToFullResync[segments[i].getSegmentDbId()] = True

        dbIdToPeerMap = gpArray.getDbIdToPeerMap()

        ###############################################
        # for each host, create + transfer the transition arguments file
        dispatchCount=0

        segmentsByHostName = GpArray.getSegmentsByHostName(segments)
        for hostName, segments in segmentsByHostName.iteritems():
            assert len(segments) > 0

            # this data is pickled just below and loaded in gpsegtoprimaryormirror.py (look for pickle.load)
            hostData = {}
            hostData["targetMode"] = targetMode
            hostData["dbsByPort"] = {}
            for db in segments:
                peer = dbIdToPeerMap[db.getSegmentDbId()]
                dbData = {}
                dbData["dbid"] = db.dbid
                dbData["mode"] = db.mode
                dbData["hostName"] = db.hostname
                dbData["hostPort"] = db.replicationPort
                dbData["peerName"] = peer.hostname
                dbData["peerPort"] = peer.replicationPort
                hostData["dbsByPort"][db.port] = dbData
                dbData["fullResyncFlag"] = db.dbid in dbIdToFullResync

            remoteConversionFile = os.path.join(segments[0].getSegmentDataDirectory(), "gpstart_mirror_args")
            localConversionFile = os.path.join(self.__masterDataDirectory, "gpstart_mirror_args_%s" % dispatchCount)

            logger.debug("Creating local conversion arguments file: %s " % (localConversionFile))
            f = open(localConversionFile, "w")
            pickle.dump(hostData, f)
            f.close()

            logger.debug("Distributing conversion arguments to host: %s " % (hostName))
            cmd=unix.RemoteCopy("copy conversion arguments", localConversionFile, hostName, remoteConversionFile)
            self.__workerPool.addCommand(cmd)
            dispatchCount+=1

        self.__workerPool.wait_and_printdots(dispatchCount,self.__quiet)

        # remove segmentsByHostName entries where we failed, and include failure messages!
        for cmd in self.__workerPool.getCompletedItems():
            if cmd.get_results().rc != 0:
                for segment in segments:
                    resultOut.addFailure(segment, "Failed to copy transition arguments to host %s : %s" % \
                        (cmd.dstHost, cmd.get_results().stderr)) # todo toString on get_results can print stderr, stdout, ret code
                del segmentsByHostName[cmd.dstHost]
        self.__workerPool.empty_completed_items()

        # clean up the local conversion files
        for i in range(0,dispatchCount):
            localConversionFile = os.path.join(self.__masterDataDirectory, "gpstart_mirror_args_%s" % i)
            logger.debug("Deleting local conversion arguments file: %s " % (localConversionFile))
            os.remove(localConversionFile)

        # request the mirror mode change
        dispatchCount=0
        for hostName, segments in segmentsByHostName.iteritems():
            logger.debug("Dispatching command to convert segments on host: %s " % (hostName))
            remoteConversionFile = os.path.join(segments[0].getSegmentDataDirectory(), "gpstart_mirror_args")
            cmd=gp.GpSegChangeMirrorModeCmd("remote segment mirror mode conversion on host '%s'" % hostName,
                                            self.__gpHome, self.__localeData, self.__gpVersion,
                                            segments, targetMode, remoteConversionFile, verbose=logging_is_verbose(),
                                            ctxt=base.REMOTE,
                                            remoteHost=hostName)
            self.__workerPool.addCommand(cmd)
            dispatchCount+=1
        self.__workerPool.wait_and_printdots(dispatchCount,self.__quiet)

        # process results
        self.__processStartOrConvertCommands(resultOut)
        self.__workerPool.empty_completed_items()



