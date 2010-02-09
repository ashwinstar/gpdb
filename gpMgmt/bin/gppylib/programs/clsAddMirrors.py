#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal

from gppylib import gparray, gplog, pgconf, userinput
from gppylib.commands import base, gp, pg, unix
from gppylib.db import catalog, dbconn
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.operations.startSegments import *
from gppylib.operations.buildMirrorSegments import *
from gppylib.programs import programIoUtils
from gppylib.system import configurationInterface as configInterface
from gppylib.system.environment import GpMasterEnvironment
from gppylib.test.testUtils import *
from gppylib.utils import toNonNoneString, checkNotNone, writeLinesToFile, readAllLinesFromFile
from lib import gpmlib

logger = gplog.get_default_logger()

# todo: unit testing for GpMirrorBuildCalculator and others
# todo: understand 'multihome' check from old code
# todo: hba update code from old addmirrors code
# todo: remove gpmlib run use (can we replace that echo with an os.path function?)
# todo MIRRORING: discuss to see if grouping by hostname is okay.  Did 3.3 have host+address columns?
# todo MIRRORING: I think the previous code would also do primary + offset to get mirror port, but this
#                 didn't seem implemented correctly for spread mirroring so I changed it (primary+offset is
#                 not always correct when doing spread mirroring)

class GpMirrorBuildCalculator:
    """
    Create mirror segment (GpDB) objects for an existing array, using different strategies.

    This class should be used by constructing and then calling either getSpreadMirrors, getGroupMirrors, or call
       addMirror multiple times

    The class uses internal state for tracking so cannot be reused after calling getSpreadMirrors or getGroupMirrors
    """

    def __init__(self, gpArray, mirrorPortOffset, mirrorDataDirs):
        self.__gpArray = gpArray
        self.__primaries = [seg for seg in gpArray.getDbList() if seg.isSegmentPrimary(False)]
        self.__primariesByHost = GpArray.getSegmentsByHostName(self.__primaries)
        self.__nextDbId = max([seg.getSegmentDbId() for seg in gpArray.getDbList()]) + 1

        def sortByPort(left, right):
            return cmp(left.getSegmentPort(), right.getSegmentPort())

        self.__mirrorsAddedByHost = {} # map hostname to the # of mirrors that have been added to that host
        self.__primaryPortBaseByHost = {} # map hostname to the lowest port number in-use by a primary on that host
        for hostName, segments in self.__primariesByHost.iteritems():
            self.__primaryPortBaseByHost[hostName] = min([seg.getSegmentPort() for seg in segments])
            self.__mirrorsAddedByHost[hostName] = 0
            segments.sort(sortByPort)

        self.__mirrorPortOffset = mirrorPortOffset
        self.__mirrorDataDirs = mirrorDataDirs

    def addMirror(self, resultOut, primary, targetHost, address, port, mirrorDataDir, replicationPort,
            primarySegmentReplicationPort):

        mirrorIndexOnTargetHost = self.__mirrorsAddedByHost[targetHost]
        assert mirrorIndexOnTargetHost is not None

        mirror = gparray.GpDB(
                content = primary.getSegmentContentId(),
                preferred_role = gparray.ROLE_MIRROR,
                dbid = self.__nextDbId,
                role = gparray.ROLE_MIRROR,
                mode = gparray.MODE_RESYNCHRONIZATION,
                status = gparray.STATUS_UP,
                hostname = targetHost,
                address = address,
                port = port,
                datadir = mirrorDataDir,
                replicationPort = replicationPort)

        self.__gpArray.addSegmentDb(mirror)

        primary.setSegmentReplicationPort(primarySegmentReplicationPort)
        primary.setSegmentMode(gparray.MODE_RESYNCHRONIZATION)

        resultOut.append(GpMirrorToBuild(None, primary, mirror, True))

        self.__mirrorsAddedByHost[targetHost] = mirrorIndexOnTargetHost + 1
        self.__nextDbId += 1

    def __addMirrorForTargetHost(self, resultOut, primary, targetHost ):
        assert self.__nextDbId != -1

        primariesOnTargetHost = self.__primariesByHost[targetHost]
        assert primariesOnTargetHost is not None

        mirrorIndexOnTargetHost = self.__mirrorsAddedByHost[targetHost]
        assert mirrorIndexOnTargetHost is not None

        # find basePort for target host
        basePort = self.__primaryPortBaseByHost[targetHost]
        assert basePort is not None
        basePort += mirrorIndexOnTargetHost

        port = basePort + self.__mirrorPortOffset
        replicationPort = basePort + self.__mirrorPortOffset * 2
        primarySegmentReplicationPort = basePort + self.__mirrorPortOffset * 3

        if mirrorIndexOnTargetHost >= len(self.__mirrorDataDirs):
            raise Exception("More mirrors targeted to host %s than there are mirror data directories" % targetHost )

        mirrorDataDir = self.__mirrorDataDirs[mirrorIndexOnTargetHost]

        # we want to spread out use of addresses on a single host so go through primary addresses
        #            for the target host to get the mirror address
        address = primariesOnTargetHost[mirrorIndexOnTargetHost % len(primariesOnTargetHost)].getSegmentAddress()

        self.addMirror( resultOut, primary, targetHost, address, port, mirrorDataDir, replicationPort,
                primarySegmentReplicationPort)

    def getGroupMirrors(self):
        """
         Side-effect: self.__gpArray is updated to contain the returned segments AND to change the replication port values of
                      the primaries as needed
        """

        hosts = self.__primariesByHost.keys()
        hosts.sort()

        result = []
        for hostIndex in range(len(hosts)):
            primaryHostName = hosts[hostIndex]
            primariesThisHost = self.__primariesByHost[primaryHostName]
            targetHost = hosts[(hostIndex + 1) % len(hosts)]

            # for the primary host, build mirrors on the target host
            for i in range(len(primariesThisHost)):
                self.__addMirrorForTargetHost( result, primariesThisHost[i], targetHost)

        return result

    def getSpreadMirrors(self):
        """
         Side-effect: self.__gpArray is updated to contain the returned segments AND to change the replication port values of
                      the primaries as needed
        """

        hosts = self.__primariesByHost.keys()
        hosts.sort()

        result = []
        for hostIndex in range(len(hosts)):
            primaryHostName = hosts[hostIndex]
            primariesThisHost = self.__primariesByHost[primaryHostName]

            for i in range(len(primariesThisHost)):
                # put the  mirror on host + i + 1
                targetHost = hosts[(hostIndex + i + 1) % len(hosts)]
                self.__addMirrorForTargetHost( result, primariesThisHost[i], targetHost)
        return result

class GpAddMirrorsProgram:
    """
    The implementation of gpaddmirrors
    
    """
    def __init__(self, options):
        """
        Constructor:

        @param options the options as returned by the options parser

        """
        self.__options = options
        self.__pool = None

    def __getMirrorsToBuildFromConfigFile(self, gpArray):

        NUM_EXPECTED_VALUES_PER_LINE = 7

        toBuild = []

        primaries = [seg for seg in gpArray.getDbList() if seg.isSegmentPrimary(False)]
        segsByContentId = GpArray.getSegmentsByContentId(primaries)

        calc = GpMirrorBuildCalculator(gpArray, 1000, [])
        for line in [s.strip() for s in readAllLinesFromFile(self.__options.mirrorConfigFile) if len(s.strip()) > 0]:
            param = line.split('=')
            if len(param) != 2:
                raise Exception('Mirror config format error for line %s' % line)
            mirror = param[1].split(':')
            if len(mirror) != NUM_EXPECTED_VALUES_PER_LINE:
                raise Exception('Mirror config format error for line %s (wrong number of colon-separated values.  '
                        'Found %d but expected %d)' % (line, len(mirror), NUM_EXPECTED_VALUES_PER_LINE))

            contentId = int(mirror[0])
            hostName = mirror[1]
            address = mirror[2]
            port = int(mirror[3])
            replicationPort = int(mirror[4])
            primarySegmentReplicationPort = int(mirror[5])
            dataDir = mirror[6]

            primary = segsByContentId[contentId]
            if primary is None:
                raise Exception("Invalid content %d specified in input file" % contentId)
            primary = primary[0]

            calc.addMirror(toBuild, primary, hostName, address, port, dataDir, replicationPort, \
                primarySegmentReplicationPort )

        return GpMirrorListToBuild(toBuild, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def outputToFile(self, mirrorBuilder, file):
        lines = []
        i = 0
        for toBuild in mirrorBuilder.getMirrorsToBuild():
            mirror = toBuild.getFailoverSegment()
            primary = toBuild.getLiveSegment()
            lines.append('mirror%d=%d:%s:%s:%d:%d:%d:%s' % \
                    (i, \
                    mirror.getSegmentContentId(), \
                    mirror.getSegmentHostName(), \
                    mirror.getSegmentAddress(), \
                    mirror.getSegmentPort(), \
                    mirror.getSegmentReplicationPort(), \
                    primary.getSegmentReplicationPort(), \
                    mirror.getSegmentDataDirectory()))
            i += 1
        writeLinesToFile(self.__options.outputSampleConfigFile, lines)

    def __getDataDirectoriesForMirrors(self, maxPrimariesPerHost):
        result = []
        if self.__options.mirrorDataDirConfigFile is not None:
            lines = readAllLinesFromFile(self.__options.mirrorDataDirConfigFile)
            result = [s.strip() for s in lines if len(s.strip()) > 0]
            if maxPrimariesPerHost != len(result):
                sys.exit('Number of data directories must equal %d but %d were read from %s' % \
                    (maxPrimariesPerHost, len(result), self.__options.mirrorDataDirConfigFile))
        else:
            while len(result) < maxPrimariesPerHost:
                print 'Enter mirror segment data directory location %d of %d >' % (len(result)+1, maxPrimariesPerHost)
                line = sys.stdin.readline()
                line = line.strip()
                if len(line) > 0:
                    result.append(line)

        dirs = []
        for d in result:
            (ok, out) = gpmlib.run('echo %s' % d)
            dirs.append(out[0].strip())
        return dirs

    def __generateMirrorsToBuild(self, gpArray):
        toBuild = []

        maxPrimariesPerHost = 0
        segments = [seg for seg in gpArray.getDbList() if seg.isSegmentPrimary(False)]
        for hostName, hostSegments in GpArray.getSegmentsByHostName(segments).iteritems():
            if len(hostSegments) > maxPrimariesPerHost:
                maxPrimariesPerHost = len(hostSegments)

        dataDirs = self.__getDataDirectoriesForMirrors(maxPrimariesPerHost)
        calc = GpMirrorBuildCalculator(gpArray, self.__options.mirrorOffset, dataDirs)
        if self.__options.spreadMirroring:
            toBuild = calc.getSpreadMirrors()
        else:
            toBuild = calc.getGroupMirrors()

        return GpMirrorListToBuild(toBuild, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def __getMirrorsToBuildBasedOnOptions(self, gpArray):
        """
        returns a GpMirrorListToBuild object
        """

        if self.__options.mirrorConfigFile is not None:
            return self.__getMirrorsToBuildFromConfigFile(gpArray)
        else:
            return self.__generateMirrorsToBuild(gpArray)

    def __displayAddMirrors(self, gpEnv, mirrorBuilder):
        logger.info('Greenplum Add Mirrors Parameters')
        logger.info('---------------------------------------------------------')
        logger.info('Greenplum master data directory          = %s' % gpEnv.getMasterDataDir())
        logger.info('Greenplum master port                    = %d' % gpEnv.getMasterPort())
        logger.info('Parallel batch limit                     = %d' % self.__options.parallelDegree )

        i = 0
        total = len(mirrorBuilder.getMirrorsToBuild())
        for toRecover in mirrorBuilder.getMirrorsToBuild():
            logger.info('---------------------------------------------------------')
            logger.info('Mirror %d of %d' % (i+1, total))
            logger.info('---------------------------------------------------------')
            programIoUtils.displaySegmentInfo("Primary", toRecover.getLiveSegment())
            programIoUtils.displaySegmentInfo("Mirror", toRecover.getFailoverSegment())

            i += 1

        logger.info('---------------------------------------------------------')

    def checkMirrorOffset(self, gpArray):
        """
        return an array of the ports to use to begin mirror port, replication port, and mirror replication port
        """

        maxAllowedPort = 61000
        minAllowedPort = 7000

        minPort = max([seg.getSegmentPort() for seg in gpArray.getDbList()])
        maxPort = max([seg.getSegmentPort() for seg in gpArray.getDbList()])

        if self.__options.mirrorOffset < 0:
            minPort = minPort + 3 * self.__options.mirrorOffset
            maxPort = maxPort + self.__options.mirrorOffset
        else:
            minPort = minPort + self.__options.mirrorOffset
            maxPort = maxPort + 3 * self.__options.mirrorOffset

        if maxPort > maxAllowedPort or minPort < minAllowedPort:
            raise ProgramArgumentValidationException( \
                'Value of port offset supplied via -p option produces ports outside of the valid range' \
                'Mirror port base range must be between %d and %d' % (minAllowedPort, maxAllowedPort))

    def run(self):
        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException(
                    "Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

        faultProberInterface.getFaultProber().pauseFaultProber()
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
        gpArray = confProvider.loadSystemConfig()

        # check that we actually have mirrors
        if gpArray.getFaultStrategy() != gparray.FAULT_STRATEGY_NONE:
            raise ExceptionNoStackTraceNeeded( \
                    "GPDB physical mirroring cannot be added.  The cluster is already configured with %s." % \
                    gparray.getFaultStrategyLabel(gpArray.getFaultStrategy()))

        # figure out what needs to be done
        mirrorBuilder = self.__getMirrorsToBuildBasedOnOptions(gpArray)
        mirrorBuilder.checkForPortAndDirectoryConflicts(gpArray)

        if self.__options.outputSampleConfigFile is not None:
        # just output config file and done
            self.outputToFile(mirrorBuilder, self.__options.outputSampleConfigFile)
            logger.info('Configuration file output to %s successfully.' % self.__options.outputSampleConfigFile)
        else:
            if self.__options.interactive:
                self.__displayAddMirrors(gpEnv, mirrorBuilder)
                if not userinput.ask_yesno(None, "\nContinue with add mirrors procedure", 'N'):
                    raise UserAbortedException()

            gpArray.setFaultStrategy(gparray.FAULT_STRATEGY_FILE_REPLICATION)
            mirrorBuilder.buildMirrors("add", gpEnv, gpArray )

        faultProberInterface.getFaultProber().unpauseFaultProber()
        return 0 # success -- exit code 0!

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

        #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("Add mirrors to a system")
        help = [""]

        parser = OptParser(option_class=OptChecker,
                           description=' '.join(description.split()),
                           version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = OptionGroup(parser, "Connection Options")
        parser.add_option_group(addTo)
        addMasterDirectoryOptionForSingleClusterProgram(addTo)

        addTo = OptionGroup(parser, "Mirroring Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, type="string",
                         dest="mirrorConfigFile",
                         metavar="<configFile>",
                         help="Mirroring configuration file")

        addTo.add_option("-o", None,
                         dest="outputSampleConfigFile",
                         metavar="<configFile>", type="string",
                         help="Sample configuration file name to output; "
                         "this file can be passed to a subsequent call using -i option")
        addTo.add_option("-m", None, type="string",
                         dest="mirrorDataDirConfigFile",
                         metavar="<dataDirConfigFile>",
                         help="Mirroring data directory configuration file")

        addTo.add_option('-s', default=False, action='store_false',
                         dest="spreadMirroring" ,
                         help="use spread mirroring for placing mirrors on hosts")

        addTo.add_option("-p", None, type="int", default=1000,
                         dest="mirrorOffset",
                         metavar="<mirrorOffset>",
                         help="Mirror port offset.  The mirror port offset will be used multiple times "
                         "to derive three sets of ports [default: %default]")

        addTo.add_option("-B", None, type="int", default=16,
                         dest="parallelDegree",
                         metavar="<parallelDegree>",
                         help="Max # of workers to use for building recovery segments.  [default: %default]")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException("too many arguments: only options may be specified", True)
        return GpAddMirrorsProgram(options)


