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
# todo MIRRORING is it okay to leave the temporary directory sitting around (as gpexpand seems to do?)
# todo MIRRORING need to compress arguments sent to segments?  I think command lines can be pretty long, but should test it
# todo MIRRORING make sure we get good errors out when trying to build mirror data directories
#
# todo MIRRORING: make sure created directories have the right permissions?
#
# todo MIRRORING if from config file, we should NOT take -F and instead should get full/incremental commands from the file?
# todo MIRRORING I may have changed the semantics for 'spareDataDirectory'.  review old semantics
#
# todo MIRRORING what about chk_pool_dir_available from gprecoverseg?  Is that needed herE?
# todo MIRRORING include check for passive standby master
#
# todo: catch string-to-int and other errors when parsing file
#
# todo MIRRORING test cases where input file says to recover a file in-place (perhaps with a change to ports)
# todo MIRRORING (bugfix) escaping of directory names in unix.LocalCopy
# todo MIRRORING when failing over to another host we may need to change port numbers as well!
#
# todO MIRRORING open question: okay to pause fault prober while prompt to user is up?
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
class GpRecoverSegmentProgram:

    #
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.__options = options
        self.__pool = None

    def outputToFile(self, mirrorBuilder, fileName):
        lines = []
        for mirror in mirrorBuilder.getMirrorsToBuild():
            str = ""
            seg = segToRecover.getFailedSegment()
            str += ('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

            seg = segToRecover.getFailoverSegment()
            if seg is not None:
                str += ' '
                str += ('%s:%d:%d:%s' % (seg.getSegmentHostName(),
                    seg.getSegmentPort(), seg.getSegmentReplicationPort(), seg.getSegmentDataDirectory()))

            lines.append(str)
        writeLinesToFile(fileName, lines)

    def getRecoveryActionsFromConfigFile(self, gpArray):
        """
        getRecoveryActionsFromConfigFile

        returns a GpMirrorListToBuild object
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
            segs.append( GpMirrorToBuild(failedSegments[i], peersForFailedSegments[i], failoverSegments[i], \
                                self.__options.forceFullResynchronization))
        return GpMirrorListToBuild(segs, self.__pool, self.__options.quiet, self.__options.parallelDegree)

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

        returns a GpMirrorListToBuild object
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

            segs.append( GpMirrorToBuild(failedSegment, liveSegment, failoverSegment, forceFull ))

        return GpMirrorListToBuild(segs, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def getRecoveryActionsBasedOnOptions(self, gpArray):
        if self.__options.recoveryConfigFile is not None:
            return self.getRecoveryActionsFromConfigFile(gpArray)
        else:
            return self.getRecoveryActionsFromConfiguration(gpArray)

    def displayRecovery(self, mirrorBuilder):
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
        total = len(mirrorBuilder.getMirrorsToBuild())
        for toRecover in mirrorBuilder.getMirrorsToBuild():
            logger.info('---------------------------------------------------------')
            logger.info('Recovery %d of %d' % (i+1, total))
            logger.info('---------------------------------------------------------')

            syncMode = "Full" if toRecover.isFullSynchronization() else "Incremental"
            logger.info("Synchronization mode                  = %s" % syncMode)

            programIoUtils.displaySegmentInfo("Failed", toRecover.getFailedSegment())
            programIoUtils.displaySegmentInfo("Recovery", toRecover.getLiveSegment())

            if toRecover.getFailoverSegment() is not None:
                programIoUtils.displaySegmentInfo("Failover", toRecover.getFailoverSegment())
            else:
                logger.info("Recover the failed segment in-place")

            i = i + 1

        logger.info('---------------------------------------------------------')

    def run(self):
        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

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

        faultProber = faultProberInterface.getFaultProber().initializeProber(gpEnv.getMasterPort())
        try:
            faultProber.pauseFaultProber()
            confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
            gpArray = confProvider.loadSystemConfig()

            # check that we actually have mirrors
            if gpArray.getFaultStrategy() != gparray.FAULT_STRATEGY_FILE_REPLICATION:
                raise ExceptionNoStackTraceNeeded( \
                        'GPDB Mirroring replication is not configured for this Greenplum Database instance.')

            # figure out what needs to be done
            mirrorBuilder = self.getRecoveryActionsBasedOnOptions(gpArray)


            if self.__options.outputSampleConfigFile is not None:
                # just output config file and done
                self.outputToFile(mirrorBuilder, self.__options.outputSampleConfigFile)
                logger.info('Configuration file output to %s successfully.' % self.__options.outputSampleConfigFile)
            elif len(mirrorBuilder.getMirrorsToBuild()) == 0:
                logger.info('No segments to recover')
            else:
                mirrorBuilder.checkForPortAndDirectoryConflicts(gpArray)

                if self.__options.interactive:
                    self.displayRecovery(mirrorBuilder)
                    if not userinput.ask_yesno(None, "\nContinue with segment recovery procedure", 'N'):
                        raise UserAbortedException()

                mirrorBuilder.buildMirrors("recover", gpEnv, gpArray )

        finally:
            if faultProber.isFaultProberPaused():
                faultProber.unpauseFaultProber()

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
        addMasterDirectoryOptionForSingleClusterProgram(addTo)

        addTo = OptionGroup(parser, "Recovery Source Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, type="string",
                            dest="recoveryConfigFile", 
                            metavar="<configFile>",
                            help="Recovery configuration file")
        addTo.add_option("-o", None,
                            dest="outputSampleConfigFile",
                            metavar="<configFile>", type="string",
                            help="Sample configuration file name to output; "
                                "this file can be passed to a subsequent call using -i option")

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
    
        
    
