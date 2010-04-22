#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved. 
#
#
# THIS IMPORT MUST COME FIRST
# import mainUtils FIRST to get python version check
#
from gppylib.mainUtils import *
import os, sys

import pickle, base64, figleaf

from optparse import Option, OptionGroup, OptionParser, OptionValueError

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib import gplog, gparray, pgconf
from gppylib.commands import base, gp, pg, unix
from gppylib.db import catalog, dbconn
from gppylib.gpcoverage import GpCoverage

logger = gplog.get_default_logger()

class GpSegStatusUsingTransition:
    """
    Program to fetch status from the postmaster by passing a message to SendFilerepTransitionStatusMessage
    """

    def __init__(self, options):
        self.__options = options
        self.__pool = None

    def run(self):

        output = []
        segments = map(gparray.GpDB.initFromString, self.__options.dirList)
        for seg in segments:
            cmd = gp.SendFilerepTransitionStatusMessage("Check Status", self.__options.statusQueryMessage,
                                                    seg.getSegmentDataDirectory(),
                                                    seg.getSegmentPort())

            cmd.run()
            res = cmd.get_results()
            if res.rc != 0:
                logger.warn("Error getting data stdout:\"%s\"  stderr:\"%s\"" % \
                            (res.stdout.replace("\n", " "), res.stderr.replace("\n", " ")))
                output.append(None)
            else:
                logger.info("Result: stdout:\"%s\"  stderr:\"%s\"" % \
                            (res.stdout.replace("\n", " "), res.stderr.replace("\n", " ")))

                line = res.stderr
                if line.startswith("Success:"):
                    line = line[len("Success:"):]
                output.append(line)

        status = '\nSTATUS_RESULTS:' + base64.urlsafe_b64encode(pickle.dumps(output))
        logger.info(status)

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

    @staticmethod
    def createParser():

        description = ("Gets status from segments on a single host using a transition message.  Internal-use only.")
        help = [""]

        parser = OptParser(option_class=OptChecker,
                           description=' '.join(description.split()),
                           version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = parser
        addTo.add_option("-s", None, type="string",
                         dest="statusQueryMessage",
                         metavar="<statusQueryMessage>",
                         help="Status Query Message")
        addTo.add_option("-D", "--dblist", type="string", action="append",
                         dest="dirList",
                         metavar="<dirList>",
                         help="Directory List")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified", True)
        return GpSegStatusUsingTransition(options)

#-------------------------------------------------------------------------
if __name__ == '__main__':
    coverage = GpCoverage()
    coverage.start()
    
    try:
        simple_main( GpSegStatusUsingTransition.createParser, GpSegStatusUsingTransition.createProgram)
    finally:
        coverage.stop()
        coverage.generate_report()