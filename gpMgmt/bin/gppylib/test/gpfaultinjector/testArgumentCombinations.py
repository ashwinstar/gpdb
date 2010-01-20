#!/usr/bin/env python
#
#
from gppylib.test.testDriver import TestDriver
from gppylib.programs.clsInjectFault import GpInjectFaultProgram
from gppylib.mainUtils import *
from gppylib.test.testUtils import *
from gppylib.system import fileSystemImplTest, fileSystemInterface

programName = sys.argv[0]
parserFn = GpInjectFaultProgram.createParser
commandFn = GpInjectFaultProgram.createProgram

driver = TestDriver()

class TempFileLogger:
    def __init__(self, index, driver):
        self.__index = index
        self.__driver = driver

    def generate(self):
        index = self.__index
        fs = self.__driver.getFileSystem()
        if fs.hasTemporaryFileDataForTesting(index):
            testOutput("BEGIN File %s:" % index)
            testOutput(fs.getTemporaryFileDataForTesting(index))
            testOutput(":END File %s" % index)
        else:
            testOutput("FILE %s NOT GENERATED" % index)


###############
#
# Now the tests:
#
# First test: no arguments.
driver.initOneHostConfiguration()
test_main( "noArguments", [], parserFn, commandFn, None, \
    """Validation error: neither --host nor --seg_dbid specified.  Exactly one should be specified.""")

driver.initOneHostConfiguration()
test_main( "noRoleArgForHost", ["--host", "this-is-my-host"], parserFn, commandFn, None, \
    """Validation error: --role not specified when --host is specified.  Role is required when targeting a host.""")

driver.initOneHostConfiguration()
test_main( "noHostArgForRole", ["--role", "primary"], parserFn, commandFn, None, \
    """Validation error: neither --host nor --seg_dbid specified.  Exactly one should be specified.""" )

driver.initOneHostConfiguration()
test_main( "noMatchingHost", ["--host", "unknown", "--role", "primary"], parserFn, commandFn, None, \
    """Injecting fault on 0 segment(s)""")

driver.initOneHostConfiguration()
test_main( "invalidRole", ["--host", "unknown", "--role", "foo"], parserFn, commandFn, None, \
    """Injecting fault on 0 segment(s)""")

# Contact primaries
driver.initOneHostConfiguration()
test_main( "noMatchingHost", ["--host", "this-is-my-host", "--role", "primary"], parserFn, commandFn, \
          [TempFileLogger(0,driver)], \
"""Injecting fault on 2 segment(s)
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 50001 -i /tmp/temporaryNamedFile0
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 50002 -i /tmp/temporaryNamedFile0
BEGIN File 0:
faultInject





1
10
:END File 0"""
    )

# Contact mirrors with args
driver.initOneHostConfiguration()
test_main( "noMatchingHost", ["--host", "this-is-my-host", "--role", "mirror", \
            "-f", "myFaultName", \
            "-y", "myFaultType", \
            "-c", "create table", \
            "-d", "db1", \
            "-t", "table10", \
            "-o", "55", \
            "-z", "11" \
            ], parserFn, commandFn, \
          [TempFileLogger(0,driver)], \
"""Injecting fault on 2 segment(s)
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 60001 -i /tmp/temporaryNamedFile0
exec $GPHOME/bin/gp_primarymirror -h this-is-my-host -p 60002 -i /tmp/temporaryNamedFile0
BEGIN File 0:
faultInject
myFaultName
myFaultType
create table
db1
table10
55
11
:END File 0"""
    )


## TODO: test alternate argument forms (--xxx instead of -y)
## TODO: test sync/async mode!!!  Will need to be able to dynamically adjust return codes of test commands!

# All done tests
printTestResults()