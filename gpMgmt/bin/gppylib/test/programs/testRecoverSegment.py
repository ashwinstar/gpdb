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
# First test: no arguments.
driver.initOneHostConfiguration()
test_main( "noArguments", [], parserFn, commandFn, None, \
    """Validation error: neither --host nor --seg_dbid specified.  Exactly one should be specified.""")

# All done tests
printTestResults()