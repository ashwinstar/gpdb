import os, sys

gProgramName = os.path.split(sys.argv[0])[-1]
if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % gProgramName)

import figleaf
from gppylib import gplog
from gppylib.commands import unix
from gppylib.interface import configurationInterface as configInterface
from gppylib.implementationGpdb import configurationImplGpdb as systemConf

gExecutableName = os.path.split(__file__)[-1]

def getProgramName():
    global gProgramName
    return gProgramName

#
# createOptionParserFn : a function that takes no arguments and returns an OptParser
# createCommandFn : a function that takes two argument (the options and the args (those that are not processed into
#                   options) and returns an object that has "run" and "cleanup" functions.  Its "run" function must
#                   run and return an exit code.  "cleanup" will be called to clean up before the program exits;
#                   this can be used to clean up, for example, to clean up a worker pool
#
def simple_main( createOptionParserFn, createCommandFn) :
    logger = gplog.get_default_logger()
    configInterface.registerConfigurationProvider( systemConf.ConfigurationProviderUsingGpdbCatalog())
    use_figleaf=os.getenv('USE_FIGLEAF', None)

    if use_figleaf:
        figleaf.start()
    commandObject=None
    parser = None

    try:
        gplog.setup_tool_logging(gExecutableName,unix.getLocalHostname(),unix.getUserName(),nonuser=False)

        parser = createOptionParserFn()
        (options, args) = parser.parse_args()

        if options.verbose:
            gplog.enable_verbose_logging()
        if options.quiet:
            gplog.quiet_stdout_logging()

        logger.info("Starting %s with args: %s" % (gProgramName, ' '.join(sys.argv[1:])))

        commandObject = createCommandFn(options, args)
        exitCode = commandObject.run()
        sys.exit(exitCode)

    except ProgramArgumentValidationException, e:
        print >> sys.stderr, ("Usage: " + gProgramName + " [--help] [options]")
        print >> sys.stderr, ""
        print >> sys.stderr, (gProgramName + ": error: " + e.args[0] )
        sys.exit(2)
    except ExceptionNoStackTraceNeeded, e:
        logger.error( "%s error: %s" % (gProgramName, e))
        sys.exit(2)
    except Exception, e:
        logger.exception("%s failed. (Reason=%s) exiting..." % (gProgramName, e))
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if commandObject:
            commandObject.cleanup()
        if use_figleaf:
            figleaf.stop()
            figleaf.write_coverage(os.path.expanduser("~") + '/.figleaf',append=True)

# Throw this out to main to have the message printed with  
class ProgramArgumentValidationException(Exception):
    pass

class ExceptionNoStackTraceNeeded(Exception):
    pass