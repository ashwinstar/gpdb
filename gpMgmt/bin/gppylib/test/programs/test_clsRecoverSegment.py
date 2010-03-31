#!/usr/bin/env python
#
#
from gppylib.test.testDriver import TestDriver
from gppylib.programs.clsRecoverSegment import GpRecoverSegmentProgram
from gppylib.mainUtils import *
from gppylib.test.testUtils import *
from gppylib.system import fileSystemImplTest, fileSystemInterface
from gppylib.gplog import get_default_logger

programName = sys.argv[0]
parserFn = GpRecoverSegmentProgram.createParser
commandFn = GpRecoverSegmentProgram.createProgram

driver = TestDriver()
logger = get_default_logger()

###############
#
# Now the tests:
#
####################################
#
# Tests involving default recovery
#
driver.initOneHostConfiguration()
test_main( "nothingToDo", [], parserFn, commandFn, None, \
    """recovering 0 segment(s)""")
driver.initTwoSegmentOneFailedMirrorConfiguration()
test_main( "oneDownBasicRecovery", [], parserFn, commandFn, None, \
    """recovering 1 segment(s)""")

# All done tests
printTestResults()