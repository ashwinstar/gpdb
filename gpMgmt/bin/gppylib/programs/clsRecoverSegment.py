#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

#
# todo MIRRORING: should we implement rollback functionality here?
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
from lib import gpmlib

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

logger = gplog.get_default_logger()

class GpRecoveryActions:

    def __init__(self, segmentsToRecover):
        self.__segmentsToRecover = segmentsToRecover
        pass

    def getSegmentsToRecover(self):
        return self.__segmentsToRecover

class GpSegmentToRecover:

    def __init__(self, failedSegment, liveSegment, failoverSegment):
        checkNotNone("failedSegment", failedSegment)
        checkNotNone("liveSegment", liveSegment)
        assert liveSegment.isSegmentUp()
        assert liveSegment.isSegmentPrimary()
        assert liveSegment.isSegmentQE()
        assert failedSegment.getSegmentContentId() == liveSegment.getSegmentContentId()
        assert failedSegment.getSegmentDbId() != liveSegment.getSegmentDbId()

        """
        __failedSegment is the segment that failed and should be recovered
        """
        self.__failedSegment = failedSegment

        """
        __liveSegment is the segment from which the recovery will take place
        """
        self.__liveSegment = liveSegment

        """
        __failoverSegment is the target segment to which we will copy the data, or None
            if we will recover in place
        """
        self.__failoverSegment = failoverSegment

    def getFailedSegment(self):
        return self.__failedSegment

    def getLiveSegment(self):
        return self.__liveSegment

    def getFailoverSegment(self):
        return self.__failoverSegment

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
        self.__options = options
        self.__pool = None

    def pauseFaultProber(self):
        pass

    def unpauseFaultProber(self):
        pass

    def startAll(self, segments, initialMirrorMode):
        pass

    def convertAll(self, segments, mirrorMode):
        pass

    def outputToFile(self, actions, fileName):
        with open(fileName, 'w') as f:
            for segToRecover in actions.getSegmentsToRecover:
                seg = segToRecover.getFailedSegment()
                f.write('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

                seg = segToRecover.getFailoverSegment()
                if seg is not None:
                    f.write(' ')
                    f.write('%s:%d:%s' % (seg.getSegmentHostName(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

                f.write('\n')

    def copySegmentDirectories(self, directives):
        if len(directives) == 0:
            return
            
        raise("not implemented") # todo MIRRORING (group by src or dest host, do transfers)
    
    def recover(self, systemConfig, actions):
        # todo MIRRORING: assert that we have paused the fault prober

        # make sure the target directories are up-to-date
        copyDirectives = []
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.getFailoverSegment() is not None:
                copyDirectives.append(GpCopyDirective(toRecover.getLiveSegment(), toRecover.getFailoverSegment(), False))
        self.copySegmentDirectories(copyDirectives)

        # update and save metadata
        for toRecover in actions.getSegmentsToRecover():
            if toRecover.getFailoverSegment() is None:
                # we are recovering the lost segment in place
                seg = toRecover.getFailedSegment()
                seg.setSegmentStatus(STATUS_UP)
                seg.setSegmentMode(MODE_RESYNCHRONIZATION)

                # todo: MIRRORING: mark for full resync
            else:
                seg = toRecover.getFailedSegment()
                # todo: mark seg for delete

                seg = toRecover.getFailoverSegment()
                seg.setSegmentStatus(STATUS_UP)
                seg.setSegmentMode(MODE_RESYNCHRONIZATION)

                raise Exception("not implemented") # todo MIRRORING

            seg = toRecover.getLiveSegment()
            seg.setSegmentMode(MODE_RESYNCHRONIZATION)

        configInterface.getConfigurationProvider().updateSystemConfig(systemConfig)

        # todo: mirroring STOP the old mirror in case they are running?

        # stop/start new mirrors
        mirrorsToStart = []
        primariesToConvert = []
        for toRecover in actions.getSegmentsToRecover():
            seg = toRecover.getFailoverSegment()
            if seg is None:
                seg = toRecover.getFailedSegment() # we are recovering in place
            mirrorsToStart.append(seg)
            primariesToConvert.append(toRecover.getLiveSegment())

        self.startAll(mirrorsToStart, "quiescent")
        self.convertAll(mirrorsToStart, "mirror")
        self.convertAll(primariesToConvert, "primary")

    def getRecoveryActionsFromConfigFile(self, systemConfig):
        """
        getRecoveryActionsFromConfigFile

        returns a GpRecoveryActions object
        """
        raise Exception("not implemented")

    def findAndValidatePeersForFailedSegments(self, systemConfig, failedSegments):
        peersForFailedSegments = [ dbIdToPeerMap.get(seg.getSegmentDbId()) for seg in failedSegments]
        for i in range(len(failedSegments)):
            peer = peersForFailedSegments[i]
            if peer is None:
                raise Exception("No peer found for dbid %s" % failedSegments[i].getSegmentDbId())
            elif peer.isSegmentDown():
                raise Exception("Both segments for content %s are down; Try restarting Greenplum DB and running %s again." %
                        (peer.getSegmentContentId(), getProgramName()))
        return peersForFailedSegments

    def getRecoveryActionsFromConfiguration(self, systemConfig):
        """
        getRecoveryActionsFromConfiguration

        returns a GpRecoveryActions object
        """
        dbIdToPeerMap = systemConfig.getDbIdToPeerMap()
        segments = systemConfig.getSegDbList()

        failedSegments = [ seg for seg in segments if seg.isSegmentDown() ]
        peersForFailedSegments = self.findAndValidatePeersForFailedSegments(systemConfig, failedSegments)

        segs = []
        for i in range(len(failedSegments)):
            segs.append( GpSegmentToRecover(failedSegments[i], peersForFailedSegments[i], failedSegments[i].copy()))

        # todo: use newRecoverHost or spareDataDirectory here to update failedSegments
        #       and pick new ports, etc.
        return GpRecoveryActions(segs)

    def getRecoveryActionsBasedOnOptions(self, systemConfig):
        if self.__options.forceRecoverySegmentDataDirAndHost is not None:
            raise Exception("Not implemented") # todo MIRORING: do we need this?
        elif self.__options.forceRecoverySegmentDbId is not None:
            raise Exception("Not implemented") # todo MIRORING: do we need this?
        elif self.__options.recoveryConfigFile is not None:
            return self.getRecoveryActionsFromConfigFile(systemConfig)
        else:
            return self.getRecoveryActionsFromConfiguration(systemConfig)
            
    #
    # todo MIRRORING: what is: multi_home = gpmlib.chk_multi_home(hosts)
    # todo MIRRORING: do I need gp_fault_action
    # todo MIRRORING: get master port properly (from MASTER_DATA_DIRECTORY)
    #
    def run(self):

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

        # gpmlib.get_master_port(opt['-d'])
        masterPort = 5432
        if gpmlib.chk_on_passive_standby(masterPort): # todo: what is this doing?  Proper error message
            raise Exception('Unable to process requested command')

        # done basic checks, now load config
        self.pauseFaultProber()
        confProvider = configInterface.getConfigurationProvider().initializeProvider(masterPort, False)
        systemConfig = confProvider.loadSystemConfig()

        # check that we actually have mirrors
        if len([seg for seg in segments if seg.getSegmentPreferredRole() == 'm']) == 0 :
            raise ExceptionNoStackTraceNeeded( \
                    'There are no mirrors configured for this Greenplum Datanase instance' \
                    'The Greenplum Database instance will need to be recovered manually')

        # figure out what needs to be done
        actions = self.getRecoveryActionsBasedOnOptions(systemConfig)

        # prompt user, if desired
        if self.__options.interactive:
            # todo: prompt user
            pass

        if self.__options.outputSampleConfigFile is not None:
            self.outputToFile(actions, self.__options.outputSampleConfigFile)
        else:
            self.recover(systemConfig, actions)

        self.unpauseFaultProber()
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

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-B", None, dest="parallelDegree", type="int", default=4,
                         metavar="<parallelDegree>",
                      help="Max # of workers to use for recovering segments")

        # todo: parallel degree sanity checks + defaults from gpstart
        # todo MIRRORING: is -F (force shutdown) required?
        # todo MIRRORING: set it up to show gprecoverseg_help

        # note (MIRRORING): I removed some options from old gprecoverseg, including:
        #        --version
        #
        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified") # todo MIRRORING: print help instead (as gpstart)
        return GpRecoverSegmentProgram(options)




