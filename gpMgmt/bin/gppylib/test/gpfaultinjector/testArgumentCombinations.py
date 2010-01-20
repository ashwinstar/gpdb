#!/usr/bin/env python
#
#
from gppylib.test.testDriver import TestDriver
from gppylib.programs.clsInjectFault import GpInjectFaultProgram
from gppylib.mainUtils import *
from gppylib.test.testUtils import *

programName = sys.argv[0]
parserFn = GpInjectFaultProgram.createParser
commandFn = GpInjectFaultProgram.createProgram


# First test: no arguments.
driver = TestDriver().initOneHostConfiguration()
test_main( "noArguments", [], parserFn, commandFn, \
    """Validation error: neither --host nor --seg_dbid specified.  Exactly one should be specified.""")

driver = TestDriver().initOneHostConfiguration()
test_main( "noRoleArgForHost", ["--host", "this-is-my-host"], parserFn, commandFn, \
    """Validation error: --role not specified when --host is specified.  Role is required when targeting a host.""")

driver = TestDriver().initOneHostConfiguration()
test_main( "noHostArgForRole", ["--role", "primary"], parserFn, commandFn, \
    """Validation error: neither --host nor --seg_dbid specified.  Exactly one should be specified.""" )

driver = TestDriver().initOneHostConfiguration()
test_main( "noMatchingHost", ["--host", "unknown", "--role", "primary"], parserFn, commandFn, \
    """Injecting fault on 0 segment(s)""")

driver = TestDriver().initOneHostConfiguration()
test_main( "invalidRole", ["--host", "unknown", "--role", "foo"], parserFn, commandFn, \
    """Injecting fault on 0 segment(s)""")

driver = TestDriver().initOneHostConfiguration()
test_main( "noMatchingHost", ["--host", "this-is-my-host", "--role", "primary"], parserFn, commandFn, \
"""Injecting fault on 2 segment(s)
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 50001 -i /var/folders/gj/gjCihknoFwiycvHu5WHdsU+++TM/-Tmp-/tmp8poFsB
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 50002 -i /var/folders/gj/gjCihknoFwiycvHu5WHdsU+++TM/-Tmp-/tmp8poFsB"""
    )

# All done tests
printTestResults()