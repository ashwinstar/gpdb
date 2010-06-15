import os, sys, traceback

gProgramName = os.path.split(sys.argv[0])[-1]
if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % gProgramName)

from gppylib import gplog
from gppylib.commands import unix
from gppylib.commands.base import ExecutionError
from gppylib.system import configurationInterface, configurationImplGpdb, fileSystemInterface, \
        fileSystemImplOs, osInterface, osImplNative, faultProberInterface, faultProberImplGpdb
from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
from gppylib.gpcoverage import GpCoverage


# Throw this out to main to have the message printed with
class ProgramArgumentValidationException(Exception):
    def __init__(self, msg, shouldPrintHelp=False):
        self.__shouldPrintHelp = shouldPrintHelp
        self.__msg = msg

    def shouldPrintHelp(self): return self.__shouldPrintHelp
    def getMessage(self): return self.__msg

class ExceptionNoStackTraceNeeded(Exception):
    pass

#
# UserAbortedException should be thrown when a user decides to stop the program (at a y/n prompt, for example) 
#
class UserAbortedException(Exception):
    pass
    
def getProgramName():
    global gProgramName
    return gProgramName

def simple_main( createOptionParserFn, createCommandFn, mainOptions=None) :
    """
     createOptionParserFn : a function that takes no arguments and returns an OptParser
     createCommandFn : a function that takes two argument (the options and the args (those that are not processed into
                       options) and returns an object that has "run" and "cleanup" functions.  Its "run" function must
                   run and return an exit code.  "cleanup" will be called to clean up before the program exits;
                   this can be used to clean up, for example, to clean up a worker pool

        mainOptions can include:   forceQuietOutput (map to bool),
                            programNameOverride (map to string)
                            suppressStartupLogMessage (map to bool)
                            useHelperToolLogging (map to bool)

    """
    coverage = GpCoverage()
    coverage.start()
    try:
        simple_main_internal(createOptionParserFn, createCommandFn, mainOptions)
    finally:
        coverage.stop()
        coverage.generate_report()

def simple_main_internal(createOptionParserFn, createCommandFn, mainOptions):
    """
    Not to be called externally -- use simple_main instead
    """
    logger = gplog.get_default_logger()
    configurationInterface.registerConfigurationProvider( configurationImplGpdb.GpConfigurationProviderUsingGpdbCatalog())
    fileSystemInterface.registerFileSystemProvider( fileSystemImplOs.GpFileSystemProviderUsingOs())
    osInterface.registerOsProvider( osImplNative.GpOsProviderUsingNative())
    faultProberInterface.registerFaultProber( faultProberImplGpdb.GpFaultProberImplGpdb())

    commandObject=None
    parser = None

    forceQuiet = mainOptions is not None and mainOptions.get("forceQuietOutput")
    options = None

    if mainOptions is not None and mainOptions.get("programNameOverride"):
        global gProgramName
        gProgramName = mainOptions.get("programNameOverride")
    suppressStartupLogMessage = mainOptions is not None and mainOptions.get("suppressStartupLogMessage")

    useHelperToolLogging = mainOptions is not None and mainOptions.get("useHelperToolLogging")

    # NOTE: if this logic is changed then also change test_main in testUtils.py
    try:
        execname = getProgramName()
        hostname = unix.getLocalHostname()
        username = unix.getUserName()

        parser = createOptionParserFn()
        (options, args) = parser.parse_args()

        if useHelperToolLogging:
            gplog.setup_helper_tool_logging(execname, hostname, username)
        else:
            gplog.setup_tool_logging(execname, hostname, username,
                                        logdir=options.ensure_value("logfileDirectory", None), nonuser=False )

        if forceQuiet:
            gplog.quiet_stdout_logging()
        else:
            if options.ensure_value("verbose", False):
                gplog.enable_verbose_logging()
            if options.ensure_value("quiet", False):
                gplog.quiet_stdout_logging()

        if options.ensure_value("masterDataDirectory", None) is not None:
            options.master_data_directory = os.path.abspath(options.masterDataDirectory)

        if not suppressStartupLogMessage:
            logger.info("Starting %s with args: %s" % (gProgramName, ' '.join(sys.argv[1:])))

        commandObject = createCommandFn(options, args)
        exitCode = commandObject.run()
        sys.exit(exitCode)

    except ProgramArgumentValidationException, e:
        if e.shouldPrintHelp():
            parser.print_help()
        logger.error("%s: error: %s" %(gProgramName, e.getMessage()))
        sys.exit(2)
    except ExceptionNoStackTraceNeeded, e:
        logger.error( "%s error: %s" % (gProgramName, e))
        sys.exit(2)
    except UserAbortedException, e:
        logger.info("User abort requested, Exiting...")
        sys.exit(4)
    except ExecutionError, e:
        logger.fatal("Error occurred: %s\n Command was: '%s'\n"
                     "rc=%d, stdout='%s', stderr='%s'" %\
                     (e.summary,e.cmd.cmdStr, e.cmd.results.rc, e.cmd.results.stdout,
                     e.cmd.results.stderr ))
        sys.exit(2)
    except Exception, e:
        if options is None:
            logger.exception("%s failed.  exiting...", gProgramName)
        else:
            if options.ensure_value("verbose", False):
                logger.exception("%s failed.  exiting...", gProgramName)
            else:
                logger.fatal("%s failed. (Reason='%s') exiting..." % (gProgramName, e))
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if commandObject:
            commandObject.cleanup()

def addStandardLoggingAndHelpOptions(parser, includeNonInteractiveOption):
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
    addTo.add_option("-l", None, dest="logfileDirectory", metavar="<directory>", type="string",
                  help="Logfile directory")

    if includeNonInteractiveOption:
        addTo.add_option('-a', dest="interactive" , action='store_false', default=True,
                    help="quiet mode, do not require user input for confirmations")


def addMasterDirectoryOptionForSingleClusterProgram(addTo):
    """
    Add the -d master directory option, intended to provide the value of the master data directory

    For programs that operate on multiple clusters at once, this function is not appropriate
    """
    addTo.add_option('-d', '--master_data_directory', type='string',
                        dest="masterDataDirectory",
                        metavar="<master data directory>",
                        help="Optional. The master host data directory. If not specified, the value set"\
                            "for $MASTER_DATA_DIRECTORY will be used.")
    
