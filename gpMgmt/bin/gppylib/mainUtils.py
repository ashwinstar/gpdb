# Line too long - pylint: disable=C0301
# Invalid name  - pylint: disable=C0103

"""
mainUtils.py
------------

This file provides a rudimentary framework to support top-level option
parsing, initialization and cleanup logic common to multiple programs.

It also implements workarounds to make other modules we use like
GpCoverage() work properly.

The primary interface function is 'simple_main'.  For an example of
how it is expected to be used, see gprecoverseg.

It is anticipated that the functionality of this file will grow as we
extend common functions of our gp utilities.  Please keep this in mind
and try to avoid placing logic for a specific utility here.
"""

import os, sys, signal, errno

gProgramName = os.path.split(sys.argv[0])[-1]
if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % gProgramName)

from gppylib import gplog
from gppylib.commands import gp, unix
from gppylib.commands.base import ExecutionError
from gppylib.system import configurationInterface, configurationImplGpdb, fileSystemInterface, \
        fileSystemImplOs, osInterface, osImplNative, faultProberInterface, faultProberImplGpdb
from optparse import OptionGroup
from gppylib.gpcoverage import GpCoverage
from lockfile.pidlockfile import PIDLockFile, LockTimeout


def getProgramName():
    """
    Return the name of the current top-level program from sys.argv[0]
    or the programNameOverride option passed to simple_main via mainOptions.
    """
    global gProgramName
    return gProgramName


class SimpleMainLock:
    """
    Tools like gprecoverseg prohibit running multiple instances at the same time
    via a simple lock file created in the MASTER_DATA_DIRECTORY.  This class takes
    care of the work to manage this lock as appropriate based on the mainOptions
    specified.

    Note that in some cases, the utility may want to recursively invoke
    itself (e.g. gprecoverseg -r).  To handle this, the caller may specify
    the name of an environment variable holding the pid already acquired by
    the parent process.
    """
    def __init__(self, mainOptions):
        self.pidfilename   = mainOptions.get('pidfilename', None)       # the file we're using for locking
        self.parentpidvar  = mainOptions.get('parentpidvar', None)      # environment variable holding parent pid
        self.parentpid     = None                                       # parent pid which already has the lock
        self.ppath         = None                                       # complete path to the lock file
        self.pidlockfile   = None                                       # PIDLockFile object
        self.pidfilepid    = None                                       # pid of the process which has the lock
        self.locktorelease = None                                       # PIDLockFile object we should release when done

        if self.parentpidvar is not None and self.parentpidvar in os.environ:
            self.parentpid = int(os.environ[self.parentpidvar])

        if self.pidfilename is not None:
            self.ppath       = os.path.join(gp.get_masterdatadir(), self.pidfilename)
            self.pidlockfile = PIDLockFile( self.ppath )


    def acquire(self):
        """
        Attempts to acquire the lock this process needs to proceed.

        Returns None on successful acquisition of the lock or 
          the pid of the other process which already has the lock.
        """
        # nothing to do if utiliity requires no locking
        if self.pidlockfile is None:
            return None

        # look for a lock file
        self.pidfilepid = self.pidlockfile.read_pid()
        if self.pidfilepid is not None:

            # we found a lock file
            # allow the process to proceed if the locker was our parent
            if self.pidfilepid == self.parentpid:
                return None

            # cleanup stale locks
            try:
                os.kill(self.pidfilepid, signal.SIG_DFL)
            except OSError, exc:
                if exc.errno == errno.ESRCH:
                    self.pidlockfile.break_lock()
                    self.pidfilepid = None

        # try and acquire the lock
        try:
            self.pidlockfile.acquire(1)

        except LockTimeout:
            self.pidfilepid = self.pidlockfile.read_pid()
            return self.pidfilepid

        # we have the lock
        # prepare for a later call to release() and take good
        # care of the process environment for the sake of our children
        self.locktorelease = self.pidlockfile
        self.pidfilepid    = self.pidlockfile.read_pid()
        if self.parentpidvar is not None:
            os.environ[self.parentpidvar] = str(self.pidfilepid)

        return None


    def release(self):
        """
        Releases the lock this process acquired.
        """
        if self.locktorelease is not None:
            self.locktorelease.release()
            self.locktorelease = None



#
# exceptions we handle specially by the simple_main framework.
#

class ProgramArgumentValidationException(Exception):
    """
    Throw this out to main to have the message possibly
    printed with a help suggestion.
    """
    def __init__(self, msg, shouldPrintHelp=False):
        self.__shouldPrintHelp = shouldPrintHelp
        self.__msg = msg
    def shouldPrintHelp(self): 
        return self.__shouldPrintHelp
    def getMessage(self): 
        return self.__msg


class ExceptionNoStackTraceNeeded(Exception):
    """
    Our code throws this exception when we encounter a condition
    we know can arise which demands immediate termination.
    """
    pass


class UserAbortedException(Exception):
    """
    UserAbortedException should be thrown when a user decides to stop the 
    program (at a y/n prompt, for example).
    """
    pass


def simple_main( createOptionParserFn, createCommandFn, mainOptions=None) :
    """
     createOptionParserFn : a function that takes no arguments and returns an OptParser
     createCommandFn : a function that takes two argument (the options and the args (those that are not processed into
                       options) and returns an object that has "run" and "cleanup" functions.  Its "run" function must
                       run and return an exit code.  "cleanup" will be called to clean up before the program exits;
                       this can be used to clean up, for example, to clean up a worker pool

     mainOptions can include: forceQuietOutput (map to bool),
                              programNameOverride (map to string)
                              suppressStartupLogMessage (map to bool)
                              useHelperToolLogging (map to bool)
                              setNonuserOnToolLogger (map to bool, defaults to false)
                              pidfilename (string)
                              parentpidvar (string)

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
    If caller specifies 'pidfilename' in mainOptions then we manage the
    specified pid file within the MASTER_DATA_DIRECTORY before proceeding
    to execute the specified program and we clean up the pid file when
    we're done.
    """
    sml = None
    if mainOptions is not None and 'pidfilename' in mainOptions:
        sml      = SimpleMainLock(mainOptions)
        otherpid = sml.acquire()
        if otherpid is not None:
            logger = gplog.get_default_logger()
            logger.error("An instance of %s is already running (pid %s)" % (getProgramName(), otherpid))
            return

    # at this point we have whatever lock we require
    try:
        simple_main_locked(createOptionParserFn, createCommandFn, mainOptions)
    finally:
        if sml is not None:
            sml.release()


def simple_main_locked(createOptionParserFn, createCommandFn, mainOptions):
    """
    Not to be called externally -- use simple_main instead
    """
    logger = gplog.get_default_logger()

    configurationInterface.registerConfigurationProvider( configurationImplGpdb.GpConfigurationProviderUsingGpdbCatalog())
    fileSystemInterface.registerFileSystemProvider( fileSystemImplOs.GpFileSystemProviderUsingOs())
    osInterface.registerOsProvider( osImplNative.GpOsProviderUsingNative())
    faultProberInterface.registerFaultProber( faultProberImplGpdb.GpFaultProberImplGpdb())

    commandObject = None
    parser = None

    forceQuiet = mainOptions is not None and mainOptions.get("forceQuietOutput")
    options = None

    if mainOptions is not None and mainOptions.get("programNameOverride"):
        global gProgramName
        gProgramName = mainOptions.get("programNameOverride")
    suppressStartupLogMessage = mainOptions is not None and mainOptions.get("suppressStartupLogMessage")

    useHelperToolLogging = mainOptions is not None and mainOptions.get("useHelperToolLogging")
    nonuser = True if mainOptions is not None and mainOptions.get("setNonuserOnToolLogger") else False

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
                                        logdir=options.ensure_value("logfileDirectory", None), nonuser=nonuser )

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


def addStandardLoggingAndHelpOptions(parser, includeNonInteractiveOption, includeUsageOption=False):
    """
    Add the standard options for help and logging
    to the specified parser object.
    """
    parser.set_usage('%prog [--help] [options] ')
    parser.remove_option('-h')

    addTo = parser
    addTo.add_option('-h', '-?', '--help', action='help',
                      help='show this help message and exit')
    if includeUsageOption:
        parser.add_option('--usage', action="briefhelp")

    addTo = OptionGroup(parser, "Logging Options")
    parser.add_option_group(addTo)
    addTo.add_option('-v', '--verbose', action='store_true', 
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
    Add the -d master directory option to the specified parser object
    which is intended to provide the value of the master data directory.

    For programs that operate on multiple clusters at once, this function/option
    is not appropriate.
    """
    addTo.add_option('-d', '--master_data_directory', type='string',
                        dest="masterDataDirectory",
                        metavar="<master data directory>",
                        help="Optional. The master host data directory. If not specified, the value set"\
                            "for $MASTER_DATA_DIRECTORY will be used.")
    
