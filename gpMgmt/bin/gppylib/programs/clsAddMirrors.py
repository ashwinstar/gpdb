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
from gppylib.utils import toNonNoneString, checkNotNone, readAllLinesFromFile

logger = gplog.get_default_logger()


#
# todo: calculate replication ports for both sides as well!
# todo: update fault strategy in database
# todo: understand 'multihome' check from old code
# todo: understand goal of the spread/group mirror code
# todo: hba update code from old addmirrors code


class GpAddMirrorsProgram:
    #
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.__options = options
        self.__pool = None

    def __getMirrorsToBuildFromConfigFile(self, gpArray):
        toBuild = []

        return GpMirrorListToBuild(toBuild, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def __generateMirrorsToBuild(self, gpArray):
        toBuild = []

        return GpMirrorListToBuild(toBuild, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def getMirrorsToBuildBasedOnOptions(self, gpArray):
        """
        returns a GpMirrorListToBuild object
        """

        if self.__options.configFile is not None:
            return self.__getMirrorsToBuildFromConfigFile(gpArray)
        else:
            return self.__generateMirrorsToBuild(gpArray)

    def displayAddMirrors(self, mirrorBuilder):
        # todo: display as gpaddmirrors does?

        logger.info('Greenplum addmirrors parameters')
        logger.info('---------------------------------------------------------')

        # todo: print info about options selected
        count = 0
        i = 0
        total = len(mirrorBuilder.getMirrorsToBuild())
        for toRecover in mirrorBuilder.getMirrorsToBuild():
            logger.info('---------------------------------------------------------')
            logger.info('Mirror %d of %d' % (i+1, total))
            logger.info('---------------------------------------------------------')
            programIoUtils.displaySegmentInfo("Primary", toRecover.getLiveSegment())
            programIoUtils.displaySegmentInfo("Mirror", toRecover.getFailoverSegment())
            i = i + 1

        logger.info('---------------------------------------------------------')

    def checkMirrorOffset(self, gpArray):
        """
        return an array of the ports to use to begin mirror port, replication port, and mirror replication port

        todo: unit test this function.
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

    def outputToFile(self, mirrorBuilder, file):
        raise Exception("Not implemented")

    def run(self):
        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

        faultProberInterface.getFaultProber().pauseFaultProber()
        confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
        gpArray = confProvider.loadSystemConfig()

        # check that we actually have mirrors
        if gpArray.getFaultStrategy() != gparray.FAULT_STRATEGY_NONE:
            raise ExceptionNoStackTraceNeeded( \
                    "GPDB mirroring cannot be added.  The cluster is configured with %s" % \
                    gparray.getFaultStrategyLabel(gpArray.getFaultStrategy()))

        # figure out what needs to be done
        mirrorBuilder = self.getMirrorsToBuildBasedOnOptions(gpArray)
        mirrorBuilder.checkForPortAndDirectoryConflicts(gpArray)


        if self.__options.outputSampleConfigFile is not None:
            # just output config file and done
            self.outputToFile(mirrorBuilder, self.__options.outputSampleConfigFile)
            logger.info('Configuration file output to %s successfully.' % self.__options.outputSampleConfigFile)
        else:
            if self.__options.interactive:
                self.displayAddMirrors(mirrorBuilder)
                if not userinput.ask_yesno(None, "\nContinue with add mirrors procedure", 'N'):
                    raise UserAbortedException()

            mirrorBuilder.buildMirrors("addmirrors", gpEnv, gpArray )

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

        """
          -o        : file to output configuration
          -i        : mirror configuration file
          -s        : spread mirror mode
          -p        : mirror offset, if not supplied, script will calculate a suitable
                      port offset for the mirror segment instances, range 7000 - 61000
                      current offset value is 1000
          -m        : mirror data directory config file

        """
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
                            help="Mirror port offset.  The mirror port offset will be used multiple times to derive 3 sets of ports [default: %default]")

        # todo: will need replication port offset as well
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


