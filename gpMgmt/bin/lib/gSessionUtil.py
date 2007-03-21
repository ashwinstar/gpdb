#! /usr/bin/env python
"""
sshSession -- access a host using ssh, implementing password memory
Author: Luke Lonergan <llonergan@greenplum.com>"""

# $Id$

#__all__ = [getLocalSSHDir, getgSessionDir, addRemoteHostKey]

__author__ = "Koneru Srikanth"
__version__ = "1.0"
__date__ = "2006-05-15"

import os, sys, getopt, string, getpass, threading, signal, tempfile

if sys.version_info < (2, 3):
    print "Error sshSession is supported for python versions 2.3 or greater"
    print "This is required for the ssh module paramiko."
    raise SystemExit, -1

import traceback
import socket
import commands
from stat import *

try :
    import logging
    import base64
    import paramiko
    import gpLogger
except ImportError, e:
    print "Error Importing necessary modules: %s " % str(e)
    raise SystemExit, -1


#
# Global variables
#
maxTries=3
logName='groupSession'
BLK_SIZE=1024
debug=False

keyFiles={'rsa':'id_rsa', 'rsa1':'identity', 'dsa':'id_dsa'}

def getLocalSSHDir(create=True):
    """ Return ssh directory on localhost.
        create if not present.
    """
    # Make Sure ~/.ssh is present
    logger=logging.getLogger('groupSession')
    try:
        homeDir=os.getenv("HOME")
    except:
        logger.error("Error evnironment variable HOME is not set properly.")
        return None
    sshDir = homeDir + '/.ssh'
    logger.debug("SSH Directory : %s ", sshDir)
    if not create:
        return sshDir
    try :
        if not os.access(sshDir, os.R_OK):
            logger.debug("Creating " + sshDir)
            os.mkdir(sshDir)
            logger.debug("chmod 700 " + sshDir)
            os.system('chmod 700 ' + sshDir)
            return sshDir
    except Exception, e:
        logger.error("*** Caught exception (%s) message: (%s )" \
            % (str(e.__class__), str(e)))
        return None
    return sshDir

def getGSDir(emptyDir=False, create=True):
    """
        Create a gSession directory on the localhost. ~/.ssh/gSession
    """
    logger=logging.getLogger('groupSession')
    localSSHDir=getLocalSSHDir()
    gSessionDir=localSSHDir+'/'+'gSession'
    try:
        if not os.access(gSessionDir, os.R_OK):
            os.mkdir(gSessionDir)
    except Exception, e:
        logger.error("*** mkdir %s Caught exception (%s) message: (%s )" \
            % ( gSessionDir, str(e.__class__), str(e)))
        return None
    #
    # empty contents
    #
    if emptyDir:
        rmFileCmd='rm -rf %s/*' % gSessionDir
        stdout=commands.getoutput(rmFileCmd)
    return gSessionDir


def addRemoteHostKey(hostName, fileName="", authType="rsa", prompt=False):
    """ Add a remote hostKey to ~/.ssh/known_hosts file"""
    logger=logging.getLogger('groupSession')
    if fileName == "":
        sshDir=getLocalSSHDir()
        if not sshDir:
            logger.error("Unable to determine local Home Directory")
            return False
        fileName = sshDir + '/known_hosts'
    logger.debug("Updating key for %s to fileName= %s" % (hostName, fileName))
    scanCmd='ssh-keyscan  -t %s %s ' % (authType, hostName)
    remoteKey=commands.getoutput(scanCmd)
    logger.debug("Remote Host Key  = %s " % remoteKey)
    if prompt: 
        print "Adding %s to \n %s (yes/no):" %(remoteKey, fileName),
        resp= sys.stdin.readline().strip()
        if(resp == 'Yes' or resp == 'y'  or resp=='Y' or resp == 'YES' or \
            resp =='yes'):
            pass
        else :
            return False
    catCmd=scanCmd + " >> %s " % fileName
    res=commands.getoutput(catCmd)
    logger.warning("Added remoteKey : %s to %s ", remoteKey, fileName)
    return True

