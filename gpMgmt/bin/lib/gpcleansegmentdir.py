#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
#
# THIS IMPORT MUST COME FIRST
# import mainUtils FIRST to get python version check
#
from gppylib.mainUtils import *

# todo MIRRORING: catch and log errors more nicely
# todo MIRRORING: make a main that can be used for remote-level scripts (that will return pickled output?)

from gppylib.gplog import *
from gppylib.commands import unix
import pickle, base64
from gppylib.gpparseopts import OptChecker,OptParser,OptParser

logger = get_default_logger()

class GpCleanSegmentDirectoryProgram:

    def __init__(self, options):
        self.__options = options

    def run(self):
        if self.__options.pickledArguments is None:
            raise ProgramArgumentValidationException("-a argument is missing")
        
        segments = pickle.loads(base64.urlsafe_b64decode(self.__options.pickledArguments))
        # todo: verify that the segment does not have a running postmaster???

        results = []
        for segment in segments:
            dir = segment.getSegmentDataDirectory()
            logger.info("Cleaning %s" % dir)

            #
            # todo MIRRORING: should we just remove dir + "/*"?  Can we somehow validate that
            #                 this is a correct mirroring directory???  Do some sanity check?
            #
            subDirs = ["PG_VERSION", "base", "global", "gpstart*", "pg_*", "postgres*", "postmaster*"]
            for subDir in subDirs:
                unix.RemoveFiles('clean segment', dir + "/" + subDir).run( validateAfter=True )

    def cleanup(self):
        pass

    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("""
        Clean segment directories.
        """)

        help = ["""
          To be used internally only.
        """]

        parser = OptParser(option_class=OptChecker,
                    description=' '.join(description.split()),
                    version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = OptionGroup(parser, "Clean Segment Options")
        parser.add_option_group(addTo)
        addTo.add_option('-p', None, dest="pickledArguments",
                         type='string', default=None, metavar="<pickledArguments>",
                       help="The arguments passed from the original script")

        parser.set_defaults()
        return parser


    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            # todo: print help (as in gpstart) instead of this message
            raise ProgramArgumentValidationException("too many arguments: only options may be specified")
        return GpCleanSegmentDirectoryProgram(options)

#-------------------------------------------------------------------------
if __name__ == '__main__':
    simple_main( GpCleanSegmentDirectoryProgram.createParser, GpCleanSegmentDirectoryProgram.createProgram)
