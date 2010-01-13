import os, sys

gProgramName = os.path.split(sys.argv[0])[-1]
if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % gProgramName)

import figleaf
from gppylib import gplog
from gppylib.commands import unix

gExecutableName = os.path.split(__file__)[-1]

#
# createOptionParserFn : a function that takes no arguments and returns an OptParser
# createCommandFn : a function that takes one argument (the options) and returns an object that has "run" and "cleanup"
#                   functions the "run" function msut run and return an exit code.  Cleanup will be called to
#                   clean up, for example, a worker pool
#
def simple_main( createOptionParserFn, createCommandFn) :
    logger = gplog.get_default_logger()
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

        commandObject = createCommandFn(options)
        exitCode = commandObject.run()
        sys.exit(exitCode)

    except ProgramArgumentValidationException, e:
        print >> sys.stderr, ("Usage: " + gProgramName + " [--help] [options]")
        print >> sys.stderr, ""
        print >> sys.stderr, (gProgramName + ": error: " + e.args[0] )
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
