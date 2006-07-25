#!/usr/bin/env python
"""
    Logging Facility
    Author : Koneru Srikanth
    Date   : May 12th 2006
===============================================================================
                DEFAULT        -V OPTION          -D Option
            Screen     File    Screen   File     Screen     File
===============================================================================
log.warn      Yes       Yes      Yes      Yes     Yes        Yes
log.error     Yes       Yes      Yes      Yes     Yes        Yes
log.info      No        Yes      Yes      Yes     Yes        Yes
log.debug     No        No       No       No      Yes        Yes    
===============================================================================
"""

__all__=['debug', 'verbose', 'initLogger']


try:
    import logging
    import logging.config
    import logging.handlers
except ImportError, e:
    print "Error Importing necessary modules: %s " % str(e)
    raise SystemExit, -1

import os
import commands

#GLOBAL VARIABLES
verboseMode=False
debugMode=False
#
# Default Values for logging
# Size of Each LogFIle
#
logFileSize=100000
#
# Number of Log Files
#
numLogFiles=6
appName="MPP"
logFileName=""
strmHandler=None
fileHandler=None

logFmt=logging.Formatter("%(asctime)s  %(name)s: %(levelname)-5s: "\
    "%(message)s", "%x %X") 


#
# Function to initialize Logging
#
def initLogger(lName="", lDir="", lfName="", debug=False, verbose=False):
    global appName, strmHandler, logFmt, fileHandler, logFileName
    if not lName:
        lName=appName
    else :
        appName=lName

    if not lDir:
        #lDir='/tmp'
        lDir=commands.getoutput('pwd')
    logger = logging.getLogger(lName)
    logger.setLevel(logging.INFO)
    logger.setLevel(logging.DEBUG)
    logging.raiseExceptions = 0
    # File Handler
    if lfName:
        if lfName[0] != '/':
            logFileName=lDir + '/' + lfName
        else :
            logFileName=lfName
        try:
            fileHandler = logging.handlers.RotatingFileHandler(logFileName, 
                "a", logFileSize, numLogFiles)
        except IOError, e:
            print "Received IO Error %s file:%s " % (str(e), logFileName)
            return
        fileHandler.setFormatter(logFmt)
        fileHandler.setLevel(logging.INFO)
        logger.addHandler(fileHandler)
        # Stream Handler
    strmHandler = logging.StreamHandler()
    strmHandler.setFormatter(logFmt)

    if debug:
        debugMode=True
        strmHandler.setLevel(logging.DEBUG)
    elif verbose:
        verboseMode=True
        strmHandler.setLevel(logging.INFO)
    else :
        strmHandler.setLevel(logging.WARN)

    logger.addHandler(strmHandler)


#
# Toggle Debugging Mode
#
def debug():
    " Function to toggle debugging Mode"
    global strmHandler, logFmt, debugMode, verboseMode
    if(not debugMode):
        print "Enabling Debug mode"
        enableDebug()
        debugMode=True
    else :
        print "Disabling Debug Mode"
        debugMode=False
        if verboseMode:
            enableVerbose()
        else :
            disableDebug()

#
# Enable Debug
#
def enableDebug():
    " enable debug logging."
    global appName, strmHandler, logFmt, fileHandler, logFileName
    logger=logging.getLogger(appName)
    logger.removeHandler(strmHandler)
    logger.removeHandler(fileHandler)
    # Creaet a A New One 
    strmHandler = logging.StreamHandler()
    strmHandler.setFormatter(logFmt)
    strmHandler.setLevel(logging.DEBUG)
    logger.addHandler(strmHandler)
    # Creaet a A New File Handler
    try :
        fileHandler = logging.handlers.RotatingFileHandler(logFileName, 
            "a", logFileSize, numLogFiles)
    except IOError, e:
        print "Received IO Error %s file:%s " % (str(e), logFileName)
        return
    fileHandler.setFormatter(logFmt)
    fileHandler.setLevel(logging.DEBUG)
    logger.addHandler(fileHandler)

#
# Disable Debug
#
def disableDebug():
    " Disable debug logging."
    global appName, strmHandler, logFmt, fileHandler, logFileName
    logger=logging.getLogger(appName)
    logger.removeHandler(strmHandler)
    logger.removeHandler(fileHandler)
    # Create a new handler for Stream
    strmHandler = logging.StreamHandler()
    strmHandler.setFormatter(logFmt)
    strmHandler.setLevel(logging.WARN)
    logger.addHandler(strmHandler)
    # Create a New Handler for File
    try :
        fileHandler = logging.handlers.RotatingFileHandler(logFileName, "a", 
            logFileSize, numLogFiles)
    except IOError, e:
        print "Received IO Error %s file:%s " % (str(e), logFileName)
        return
    fileHandler.setFormatter(logFmt)
    fileHandler.setLevel(logging.INFO)
    logger.addHandler(fileHandler)
#
# Enable Debug
#
def verbose():
    " Toggle verbose mode."
    global strmHandler, logFmt, verboseMode
    if(not verboseMode):
        print "Enabling Verbose mode"
        verboseMode=True
        enableVerbose()
    else :
        print "Disabling Verbose Mode"
        verboseMode=False
        disableVerbose()
#
# Enable verbose
#
def enableVerbose():
    "Enable verbose logging."
    global appName, strmHandler, logFmt
    logger=logging.getLogger(appName)
    logger.removeHandler(strmHandler)
    # Creaete a A New One 
    strmHandler = logging.StreamHandler()
    strmHandler.setFormatter(logFmt)
    strmHandler.setLevel(logging.INFO)
    logger.addHandler(strmHandler)

#
# Disable verbose
#
def disableVerbose():
    "Disable verbose logging."
    global appName, strmHandler, logFmt
    logger=logging.getLogger(appName)
    logger.removeHandler(strmHandler)
    # Creaete a A New One 
    strmHandler = logging.StreamHandler()
    strmHandler.setFormatter(logFmt)
    strmHandler.setLevel(logging.WARN)
    logger.addHandler(strmHandler)
#
# Function to test logging
#
def logTest():
    """ logTest File"""
    global appName, verboseMode, debugMode

    logger=logging.getLogger(appName)
    print "Log Testing %s" % appName
    if verboseMode:
        print "VERBOSE IS ENABLED\n"
    if debugMode:
        print "DEBUG MODE IS ENABLED\n"

    if not logger:
        print "ERROR logger not initialized"
        return
    print "\n\n++++++++++++++++++++++++++++++++++++++"
    logger.error("This is error message.")
    logger.warn("Warning Warning.")
    logger.info("Information for free.")
    logger.debug(" debug gVal=34")
    print "---------------------------------------\n\n"

#
# Test the logging functionality
#
if __name__=='__main2__':
    print "Initializing Logging"
    print "LogFile name = /tmp/logTestLog"
    initLogger("LogTest", '/tmp', 'logTestLog', False)
    print "Test 1"
    print "Only Warning and Error messages should be seen"
    logTest()
    debug()
    logTest()
    debug()
    logTest()
    debug()
    logTest()
    rangeInt=0
    for i in range(rangeInt):
        verbose()
        logTest()

    for i in range(rangeInt):
        debug()
        logTest()

def msgLogger(loggerName, fileName=""):
    """ 
        Return a logger, that will log to file and screen.
        All messages should be logger.info
    """
    logger= logging.getLogger(loggerName)
    #logging.addLevelName("MESSAGE", message)
    logger.setLevel(logging.INFO)
    # fileHandler
    if (fileName):
        fileHandler = logging.handlers.RotatingFileHandler(fileName, "a")
        fileHandler.setLevel(logging.INFO)
        logger.addHandler(fileHandler)
    # Stream Handler
    strmHandler = logging.StreamHandler()
    strmHandler.setLevel(logging.INFO)
    logger.addHandler(strmHandler)

    strmHandler = logging.StreamHandler()
    strmHandler.setLevel(55)
    strmHandler.setFormatter(logFmt)
    logger.addHandler(strmHandler)

    return logger


    

if __name__=='__main__':
    logging.addLevelName(55, "Message")

    logger=msgLogger("MyLog")
    logger.debug("This is Debug, Nothing to File")
    logger.info("This is Info, Nothing to File")
    logger.error("This is error, Nothing to File")

    # 
    # Thiw will print twice to screen
    #

    logger=msgLogger("MyLog")
    logger.debug("This is Debug, Nothing to File")
    logger.info("This is Info, Nothing to File")
    logger.error("This is error, Nothing to File")

    # 
    # Thiw will print thrice to screen
    #
    logger=msgLogger("MyLog")
    logger.debug("This is Debug, Nothing to File")
    logger.info("This is Info, Nothing to File")
    logger.error("This is error, Nothing to File")

    logger1=msgLogger("Foo", "/tmp/mylog.log")
    logger1.debug("This is Debug, should go to File")
    logger1.info("This is Info, should go to File")
    logger1.error("This is Error, should go to File")
    logger1.log(logging.ERROR, "This is error 2, should go to File")
    logger1.log("Message", "This is error 2, should go to File")
    logger1.log(55, "This is error 3, should go to File")


