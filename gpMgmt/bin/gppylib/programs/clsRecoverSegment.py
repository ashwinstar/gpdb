#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

#
# todo MIRRORING: how to prevent fault prober from changing settings underneath the script while it's deciding what to do?
# todo MIRRORING: should we implement rollback functionality here?

#
# THIS IMPORT SHOULD COME FIRST
#
# import mainUtils FIRST to get python version check
from gppylib.mainUtils import *

import os, sys, time

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
from tempfile import NamedTemporaryFile

from lib import gpmlib

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.utils import toNonNoneString
from gppylib import gplog
from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib.interface import configurationInterface as configInterface
from gppylib import pgconf

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

    def pauseFaultProber(self):
        pass

    def unpauseFaultProber(self):
        pass

    def startAll(self, segments, initialMirrorMode):
        pass

    def convertAll(self, segments, mirrorMode):
        pass

    #
    # Run standard recovery:
    #    pause prober
    #
    #    1) find all current mirrors that are marked down
    #    for each one:
    #       a) change them to be in full resynchronization mode
    #       b) bring the process up
    #       c) mark the segment as up in the db
    #       d) convert the mirror and primary over to the proper mode
    #
    # u  npause prober
    def standardRecover(self, confProvider, systemConfig):

        if self.__options.newRecoverHost is not None and \
            self.__options.spareDataDirectory is not None:
            raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING

        downMirrors = [seg for seg in systemConfig.getSegmentList() if \
                                    seg.getSegmentRole() == 'm' and seg.getSegmentStatus() != 'u']

        if len(downMirrors) == 0:
            logger.info("No mirrors are marked down.  There is nothing to do.")
            return

        # todo: move pool management into a superclass?
        numWorkers = min(len(downMirrors), self.__options.parallelDegree)
        self.__pool = base.WorkerPool( numWorkers=numWorkers )

        primariesForDownMirrors = systemConfig.getPeersAndEnsureFoundPeers(downMirrors)

        # update mode and status in database
        for segment in downMirrors:
            seg.setSegmentStatus('u')
            seg.setSegmentMode('r')
        for segment in primariesForDownMirrors:
            seg.setSegmentMode('r') # recovery!
        confProvider.updateSystemConfig( systemConfig )

        #
        # todo: stop the mirrors if they are already up
        #
        # bring the mirrors up in quiescent mode and then convert to mirroring
        #
        self.startAll(self.__pool, downMirrors, "quiescent")
        self.convertAll(self.__pool, downMirrors, "mirror")

        #
        self.convertAll(self.__pool, primariesForDownMirrors, "primary")

        raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING

    def run(self):

        # verify "what to recovery" options: note that specifying NONE is okay as well
        if self.__options.forceRecoverySegmentDataDirAndHost is not None and \
            self.__options.forceRecoverySegmentDbId is not None and \
            self.__options.recoveryConfigFile is not None and \
            self.__options.outputSampleConfigFile is not None:
            raise ProgramArgumentValidationException(\
                    "Only one of -i, -i, -z, and -S may be specified")

        # verify "what to recovery" options  # todo MIRRORING: do we require at least one to be set
        if self.__options.newRecoverHost is not None and \
            self.__options.spareDataDirectory is not None:
            raise ProgramArgumentValidationException(\
                    "Only one of -p and -s may be specified")

        # todo MIRRORING: get master port properly
        # gpmlib.get_master_port(opt['-d'])
        masterPort = 5432
        if gpmlib.chk_on_passive_standby(masterPort):
            gpmlib.fatal('Unable to process requested command')

        # load segments
        confProvider = configInterface.getConfigurationProvider().initializeProvider(masterPort, False)

        self.pauseFaultProber()

        systemConfig = confProvider.loadSystemConfig()
        segments = systemConfig.getSegmentList()

        # check that we actually have mirrors
        if len([seg for seg in segments if seg.getSegmentPreferredRole() == 'm']) == 0 :
            raise ExceptionNoStackTraceNeeded( \
                    'There are no mirrors configured for this Greenplum Datanase instance' \
                    'The Greenplum Database instance will need to be recovered manually')

        #
        # now do it!
        if self.__options.forceRecoverySegmentDataDirAndHost is not None:
            raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING
        elif self.__options.forceRecoverySegmentDbId is not None:
            raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING
        elif self.__options.recoveryConfigFile is not None:
            raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING
        elif self.__options.outputSampleConfigFile is not None:
            raise ExceptionNoStackTraceNeeded("not implemented") # todo MIRRORING
        else:
            self.standardRecover(confProvider, systemConfig)
        # todo MIRRORING: what is: multi_home = gpmlib.chk_multi_home(hosts)
        # todo MIRRORING: do I need gp_fault_action

        self.unpauseFaultProber()
        logger.info("DONE")
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
        parser.set_usage('%prog [--help] [options] ')
        parser.remove_option('-h')

        addTo = parser
        addTo.add_option('-h', '-?', '--help', action='help',
                          help='show this help message and exit')

        addTo = OptionGroup(parser, "Logging Options")
        parser.add_option_group(addTo)
        addTo.add_option('-v','--verbose', action='store_true',
                          help='debug output.')
        addTo.add_option('-q', '--quiet', action='store_true',
                          help='suppress status messages')
        addTo.add_option("-l", None, dest="logfileDirectory", type="string",
                      help="Logfile directory")
        addTo.add_option('-a', dest="interactive" , action='store_false', default=True,
                        help="quiet mode, do not require user input for confirmations")

        addTo = OptionGroup(parser, "Recovery Source Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, dest="recoveryConfigFile", type="string",
                      help="Recovery configuration file")
        addTo.add_option("-o", None, dest="outputSampleConfigFile", type="string",
                      help="Sample configuration file name to output")
        addTo.add_option("-S", None, dest="forceRecoverySegmentDbId", type="int",
                     help="Primary segment dbid to force recovery")
        addTo.add_option("-z", None, dest="forceRecoverySegmentDataDirAndHost", type="string",
                     help="Primary segment data dir and host to force recovery")

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-p", None, dest="newRecoverHost", type="string",
                      help="Spare new host to which to recover segments")
        addTo.add_option("-s", None, dest="spareDataDirectory", type="string",
                      help="Spare data directories on current hosts")

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-B", None, dest="parallelDegree", type="int", default=4,
                      help="Max # of workers to use for recovering segments")

        # todo: parallel degree sanity checks + defaults from gpstart
        # todo MIRRORING: figure out which options are XORed (exactly one of some options can be specified)
        # todo MIRRORING: is -F (force shutdown) required?
        # todo MIRRORING: set it up to show gprecoverseg_help
        # todo MIRRORING: --version
        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified") # todo MIRRORING: print help instead (as gpstart)
        return RecoverSegmentProgram(options)

