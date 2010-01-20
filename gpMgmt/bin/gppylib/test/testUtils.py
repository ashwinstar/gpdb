#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
#
# THIS IMPORT MUST COME FIRST
# import mainUtils FIRST to get python version check
#
from gppylib.mainUtils import *

import os, sys, traceback

gProgramName = os.path.split(sys.argv[0])[-1]
import figleaf
from gppylib.commands.base import setExecutionContextFactory, ExecutionContext,CommandResult
from gppylib import gplog
from gppylib.commands import unix
from gppylib.system import configurationInterface as configInterface
from gppylib.system import configurationImplGpdb as systemConf

class TestExecutionContext(ExecutionContext):

    # todo: clean this up (make private), but only when completed in LocalExecutionContext is inspected
    completed = False
    halt = False

    def __init__(self, execution_context_id, remoteHost, stdin):
        self.execution_context_id = execution_context_id
        self.remoteHost = remoteHost
        self.stdin = stdin

    def execute(self,cmd):
        testOutput("exec %s" % cmd.cmdStr)
        (rc,stdoutValue,stderrValue) = (0, [], []) # todo: fetch desired output values from a lookup table or callback or something 

        self.completed=True

        result = CommandResult(rc,"".join(stdoutValue),"".join(stderrValue), self.completed, self.halt)
        cmd.set_results(result)

    def interrupt(self):
        raise Exception("not implemented") # implement this when needed for testing

    def cancel(self):
        raise Exception("not implemented") # implement this when needed for testing

class TestExecutionContextFactory:
    def createExecutionContext(self,execution_context_id, remoteHost, stdin):
        return TestExecutionContext(execution_context_id, remoteHost, stdin)
    

gTestResults = []
gTestOutput = None
def testOutput(o) :
    global gTestOutput
    if gTestOutput is not None:
        gTestOutput.append(str(o))

def finishTest(expectedOutputStr):
    global gTestOutput
    global gTestName

    output = "\n".join(gTestOutput)

    if output == expectedOutputStr:
        gTestResults.append((gTestName, True, None))
    else:
        msg = "Test %s failed.  EXPECTED OUTPUT (surrounding triple quotes added by this output):\n\"\"\"%s\"\"\"\n\n" \
            "ACTUAL OUTPUT (surrounding triple quotes added by this output):\n\"\"\"%s\"\"\"" % (gTestName, expectedOutputStr, output)
        gTestResults.append((gTestName, False,msg))

    gTestOutput = None
    gTestName = None


def startTest(testName):
    global gTestOutput
    global gTestName

    gTestOutput = []
    gTestName = testName

def printTestResults():
    global gTestResults

    numFailures = 0
    numSuccesses = 0
    for test in gTestResults:
        if ( test[1]):
            numSuccesses += 1
            print >> sys.stderr, "SUCCESS: %s passed" % test[0]
        else:
            numFailures += 1
            print >> sys.stderr, "FAILURE: %s failed\n%s\n\n" % (test[0], test[2])

    if numFailures == 0:
        print >> sys.stderr, "ALL %s TESTS SUCCEEDED" % numSuccesses
    else:
        print >> sys.stderr, "%s tests succeeded" % numSuccesses
        print >> sys.stderr, "%s tests FAILED" % numFailures

def resetTestResults():
    global gTestResults

    gTestResults = []

def test_main( testName, newProgramArgs, createOptionParserFn, createCommandFn, extraOutputGenerators, expectedOutput) :
    global gTestOutput

    # update args
    previousArgs = sys.argv
    sys.argv = []
    sys.argv.append(getProgramName())
    sys.argv.extend(newProgramArgs)

    # register command factory
    setExecutionContextFactory(TestExecutionContextFactory())

    commandObject=None
    parser = None

    startTest(testName)

    try:
        gplog.setup_tool_logging(gExecutableName,unix.getLocalHostname(),unix.getUserName(),nonuser=False)

        parser = createOptionParserFn()
        (options, args) = parser.parse_args()
        gplog.enable_verbose_logging()

        commandObject = createCommandFn(options, args)
        exitCode = commandObject.run()
        # todo MIRRORING: use a testing version of sysexit sys.exit(exitCode)

    except ProgramArgumentValidationException, e:
        testOutput( "Validation error: %s" % e.args[0])
    except ExceptionNoStackTraceNeeded, e:
        testOutput( str(e))
    except Exception, e:
        testOutput( "EXCEPTION: %s\n%s" % (e, traceback.format_exc()))
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if commandObject:
            commandObject.cleanup()

        # clean up test settings
        sys.argv = previousArgs
        setExecutionContextFactory(None)

    if extraOutputGenerators is not None:
        for gen in extraOutputGenerators:
            gen.generate()

    finishTest(expectedOutput)

