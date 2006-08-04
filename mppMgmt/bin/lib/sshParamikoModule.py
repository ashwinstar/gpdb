#! /usr/bin/env python
"""
sshSession -- access a host using ssh, implementing password memory
Author: Luke Lonergan <llonergan@greenplum.com>"""

# $Id$

__all__ = ['run', 'ssh', 'scp', 'scprem',  'setCMD',  'killCMD', \
    'setupKeyAccessLocal', 'setupRemoteKey', 'isReady',  'isError', \
     'makeReady']

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
import select
import commands
from stat import *

try :
    import logging
    import base64
    import paramiko
    import gpLogger
    import gSessionUtil
    from gSessionUtil import keyFiles
except ImportError, e:
    print "Error Importing necessary modules: %s " % str(e)
    raise SystemExit, -1


#
# Global variables
#
maxTries=3
BLK_SIZE=1024
sshPasswd=""
sshRsaPass=""
sshDsaPass=""
debug=False


# Exception classes used by this module.
class ExceptionSshSession(Exception):
    """Base class for all exceptions raised by this module."""
    def __init__(self, value):
        self.value = value
    def __str__(self):
        return str(self.value)
    def get_trace(self):
        """This returns an abbreviated stack trace with lines that only 
           concern the caller. 
        """
        tblist = traceback.extract_tb(sys.exc_info()[2])
        tblist = filter(self.__filter_not_sshSession, tblist)
        tblist = traceback.format_list(tblist)
        return ''.join(tblist)

    def __filter_not_sshSession(self, trace_list_item):
        if trace_list_item[0].find('sshSession.py') == -1:
            return True
        else:
            return False

class NOTREADY(ExceptionSshSession):
      """Raised when the session isn't ready"""

class PasswordFailure(ExceptionSshSession):
      """Raised when we can't get a password that works"""

class sshSocketConnection(ExceptionSshSession):
      """ Raised when not able to connect to the remote host"""

class runCommandException(ExceptionSshSession):
      """ Raised when failed to execute a command on remote node"""


class sshSession(threading.Thread):
    "Session with extra state including the password to be used."
    def __init__(self, user, port, host, password=None, verbose=False, \
        debug=False, logger=None, setupKeysFlag=True):
        threading.Thread.__init__(self)        
        self.stdout=""
        self.stderr=""
        self.exitStatus=0
        self.debug=debug
        self.verbose=verbose
        self.lock = threading.RLock()
        self.user = user
        self.port = port
        self.host = host
        self.password = password
        self.userHost=self.user + '@' + self.host
        self.authMode=""
        self.sshDir=""
        self.keyFileList=[]

        #Output of last command executed
        self.lastoutput = None
        self.cmd = None
        self.args = []

        # Paramiko Variables
        self.trans=None
        self.sftpClnt=None
        self.chan=None

        self.setUpKeys=setupKeysFlag

        #Flag that indicates if this session object is able to execute 
        #commands
        self.ready = False
        self.error = False

        if logger:
            self.logger=logger
        else :
            gpLogger.initLogger(lName="sshSession", debug=debug, \
                verbose=verbose)
            self.logger=logging.getLogger("sshSession")

    def __die(self,msg=None):
        if not msg == None:
            print 'sshSession command exiting: %s' % (msg)
        self.ready=False
        self.__setError()
        raise NOTREADY ('sshSession command NOTREADY')

    def __readyOrDie(self):
        if not self.isReady():
            self.__die('ssh session not set up to run commands.')

    def isReady(self, printError=True):
        logger=self.logger
        if (not self.trans):
            if printError:
                logger.error("Transport for %s is not initialized.", 
                    self.userHost)
            return False
        if (not self.trans.is_authenticated()):
            if printError:
                logger.error("Transport for %s is not yet authenticated.",
                    self.userHost)
            return False
        if (not self.sftpClnt ):
            if printError:
                logger.error("sftpCleint for %s is not initialized.",
                self.userHost)
            return False
        return True

    def isError(self):
        return self.error

    def __setError(self,error=True):
        self.error=error

    def putDir(self, localPath, remotePath):
        """ Copy local Directory to remote Directory """
        logger = self.logger
        if  not self.isReady():
            return "" 
        logger.debug('authMode = %s ', self.authMode)
        if not (self.authMode == 'RSA' or self.authMode == 'DSA' or \
            self.authMode == 'RSAGP') :
            logger.error("Directory copy is supported only if " \
                " certificates without out any pass Phrase are exchanged.")
            return ""
        lPath=os.path.expanduser(localPath)
        if not os.access(lPath, os.R_OK):
            logger.error("Unable to access : %s ",  lPath)
            return ""
        rPath="%s@%s:/%s" %(self.user, self.host, remotePath)
        logger.info("Copying %s to %s", lPath, rPath)
        idFile=""
        if self.authMode == 'RSAGP':
            idFile= " -i %s " % self.rsaGPFilePath
        if self.verbose:    
            verbOpt=""
        else :
            verbOpt= ' -q ' 
        scpCmd='scp -P %d %s %s -r %s %s@%s:%s' % (self.port, idFile, verbOpt,
            lPath,  self.user, self.host, remotePath)
        logger.info(scpCmd)
        #res=commands.getoutput(scpCmd)
        res=""

        if self.verbose:
            os.system(scpCmd)
        else :
            res=commands.getoutput(scpCmd)

        logger.info("Finished Copying %s to %s, %s", lPath, rPath, res)
        return res

    def putFile(self, localPath, remotePath):
        """ Copy a local file to remote location.
            localPath, remotePath 
            Has to return a string for groupSession
        """
        logger=self.logger
        #Expand the ~ and ~<userName> at the begining of the file
        lPath=os.path.expanduser(localPath)
        rPath="%s@%s:%s" %(self.user, self.host, remotePath)
        try :
            try :
                rStat=self.sftpClnt.stat(remotePath)
                if S_ISDIR(rStat.st_mode):
                    lbaseName=os.path.basename(localPath)
                    rPath=rPath+'/'+lbaseName
                    remotePath=remotePath+'/'+lbaseName
            except Exception, e:
                # Probably IOError
                #logger.error("*** Caught exception (%s) message: (%s )" \
                    #% (str(e.__class__), str(e)))
                pass
            logger.info("Copying %s to %s", lPath, rPath)
            self.sftpClnt.put(lPath, remotePath)
        except KeyboardInterrupt:
            logger.debug("runCmd(Executing) Received KeyBoardInterrupt")
            raise
        except IOError, e:
            errStr='Unable to Copy (%s to %s):%s' % (lPath, rPath, str(e))
            logger.error(errStr)
            return errStr
        except paramiko.SFTPError, e:
            errStr= 'Received SFTP Client Error (%s to %s):%s' % \
                (lPath, rPath, str(e))
            logger.error(errStr)
            return errStr
        except Exception, e:
            errStr="Caught Exception putFile %s: %s (%s to %s) "% \
                (str(e.__class__),  str(e), lPath, rPath)
            logger.error(errStr)
            if self.debug:
                traceback.print_exc()
            return errStr 
        #Has to Reuturn String for  in groupSession
        logger.info("Finished Copying %s to %s", lPath, rPath)
        return  ""

    def execCmd(self, command):
        """ Wrapper for run command, Returns only stdout"""
        logger=self.logger
        logger.debug("execCmd %s on %s ", command, self.userHost)
        stdout=""
        stderr=""
        try:
            (exitStatus, stdout, stderr)=self.runCommand(command)
        except runCommandException, err:
            logger.info('Error Executing %s on %s %s',  \
                command, self.userHost, err)
        except SystemExit:
            raise
        except KeyboardInterrupt:
            logger.debug("execCmd Received KeyBoardInterrupt")
            raise
        except Exception, e:
            logger.error('*** Caught execCmd exception %s: %s executing %s on %s ', 
                   str(e.__class__), str(e), command, self.userHost)
            traceback.print_exc()
        self.lastoutput=stdout
        if  not stdout and stderr:
            stdout = stderr 
        return stdout

    def runCommand(self, command ):
        """ Execute a shell command on a remote machine
            return (exitCode, stdout, stderr)
            raise runCommmandException, for other errors 
        """
        logger=self.logger
        rmChannel=None
        if (not self.trans):
            raise runCommandException("Transport is not initialized.")
        if (not self.trans.is_authenticated()):
            raise runCommandException("Transport is not authenticated.")
        if (not command):
            raise runCommandException("Command not provided.")
        try:
            rmChannel = self.trans.open_session()
            self.chan=rmChannel
            rmChannel.exec_command(command)
        except paramiko.SSHException, e:
            raise runCommandException(str(e))
        except SystemExit:
            raise
        except KeyboardInterrupt:
            logger.debug("runCmd Received KeyBoardInterrupt")
            raise
        except Exception, e:
            errMsg="*** Caught exception runCommand (%s) message: (%s ), command (%s)"\
                " host : %s " % (str(e.__class__), str(e), command, self.host)
            logger.error(errMsg)
            print errMsg
            raise runCommandException(errMsg)
        logger.debug(" %s Waiting for error Code: (%s)", self.userHost, command)
        exit_status=0
        #
        # TODO Need to implement a timeout here 
        # However, taking care of CTRL-C would really solve the issue here.
        #
        try:
            stdout = ""
            stderr = ""
            rmChannel.set_combine_stderr(True)
            rmChannel.setblocking(0)
            recvBlk=BLK_SIZE
            try:
                while True:
                    try:
                        recvBytes= rmChannel.recv(recvBlk)
                        if not recvBytes:
                            break
                        stdout+= recvBytes
                        logger.debug("%s len = %d " %( self.userHost, 
                                len(recvBytes)))
                        if len(recvBytes) == recvBlk:
                            recvBlk *= 2
                        select.select([], [], [], 0.1)
                    except select.error:
                        select.select([], [], [], 0.1)
                        pass
                       #print "Received Select (block) Error"
                       # raise KeyboardInterrupt
                    except socket.timeout:
                        select.select([], [], [], 0.1)
                        pass
            except select.error:
                pass
                #print "Received Select Error"
                #raise KeyboardInterrupt
            except KeyboardInterrupt:
                logger.debug("runCmd(Waiting) Received KeyBoardInterrupt")
                raise
            exitStatus = rmChannel.recv_exit_status()
            logger.debug("%s stdout = %d \n" % (self.userHost, len(stdout)))
            #exit_status=rmChannel.recv_exit_status()
            logger.debug("Error Code : %d %s executed on %s \n", exit_status,
                command, self.host)
            # Only Log first 1024 Bytes.
            logger.debug("Command = <%s> exit_status=%d \n stdout=%d \n",
                command, exit_status, len(stdout))
            self.stdout=stdout
            self.stderr=stderr
            self.exitStatus=exit_status
        except KeyboardInterrupt:
            logger.debug("runCmd(Executing) Received KeyBoardInterrupt")
            if rmChannel:
                rmChannel.close()
                rmChannel=None
                self.chan=None
            raise
        if rmChannel:
            rmChannel.close()
            rmChannel=None
            self.chan=None
        return (self.exitStatus, self.stdout, self.stderr)

    def killCMD(self,sig=signal.SIGTERM):
        if self.chan:
            logger=self.logger
            logger.info("Closing channel %s", self.userHost)
            self.chan.close()

    def shutdown(self):
        """ Close the trans """
        if self.trans:
            self.trans.close()


    def run(self):
        logger=self.logger
        try:
            if self.cmd is "ssh":
                self.lastoutput = self.execCmd(self.args[0])
            elif self.cmd is "scp":
                self.lastoutput = self.putFile(self.args[0], self.args[1])
            elif self.cmd is "dirCopy":
                self.lastoutput = self.putDir(self.args[0], self.args[1])
            elif self.cmd is "exists":
                    self.lastoutput = self.exists(self.args[0])
            else:
                self.lastoutput = "No interface selected"
            return self.lastoutput
        except SystemExit:
            raise
        except KeyboardInterrupt:
            logger.debug("run Received KeyBoard Interrupt")
            raise
        except Exception, e:
            errMsg="%s Caught exception run (%s) message: (%s ), "\
            "run(%s:%s)" % (self.userHost, str(e.__class__), str(e),   \
                self.cmd, self.args[0])
            logger.error(errMsg)
            return None

    def exists(self, file):
        "Retrieve file permissions of specified remote file."
        logger=self.logger
        logger.info("Closing channel %s@%s", self.user, self.port)
        self.__readyOrDie()
        seen = self.ssh("/bin/ls -ld %s" % file)
        if  self.verbose:
                logger.info("Existence check on %s gave %s\n" % (file,`seen `))
        if string.find(seen, "No such file") > -1:
                return None                     # File doesn't exist
        else:
                return seen.split()[0]  # Return permission field of listing.

    def setCMD(self,cmd,args=[]):
        self.cmd=cmd
        self.args=args

    def getRemoteSSHDir(self, createFlag=True):
        """ Return remote SSHDIr
            Most of the cases it is ~/.ssh
            Create and set correct permisssions if not present
        """
        logger=self.logger
        # Determine Remote Node Stuff.
        if self.sshDir:
            return self.sshDir
        try:
            (exitStatus, stdout, stderr)=self.runCommand('echo $HOME')
        except runCommandException, err:
            logger.error("Unable to determine HOME for %s@%s: %s",
                self.user, self.host, str(err))
            return ""
        except Exception, e:
            logger.error('*** Caught exception getRSSHDir %s: %s ', str(e.__class__), \
                str(e))
            traceback.print_exc()
            return "" 
        rmtHomeDir=stdout.strip()
        #print "rmtHomeDir=%s" % rmtHomeDir
        if not rmtHomeDir:
            logger.error("Unable to determine the Home Directory for  "\
                " %s@%s " , self.user, self.home)
            return ""
        rmtSSHDir = rmtHomeDir + '/.ssh'
        logger.debug("Remote SSH Directory %s@%s : %s ", self.user,
            self.host, rmtSSHDir)
        try:
            stat=self.sftpClnt.stat(rmtSSHDir)
        except IOError, e:
            logger.info('%s is not present on the host %s ', rmtSSHDir, 
                self.host)
            logger.info('Creating %s on %s', rmtSSHDir, self.host)
            try :
                self.sftpClnt.mkdir(rmtSSHDir)
                stat = self.sftpClnt.stat(rmtSSHDir)
                self.sftpClnt.chmod(rmtSSHDir ,
                    (stat.st_mode & ~0777) | 0700)
            except IOError, e:
                logger.info('Received IO Error %s on host %s@%s ', \
                    rmtSSHDir, self.host)
                return ""
        except Exception, e:
            errMsg="*** Caught exception stat (%s) message: (%s ), "\
            " remote SSHDIR (%s) host : %s " % (str(e.__class__), str(e),  
                self.host, rmtSSHDir)
            logger.error(errMsg)
            return ""
        self.sshDir=rmtSSHDir
        return rmtSSHDir

    def updateAuthorizedKeys(self, authKeyPath=""):
        """  Copy a local file to remote sessions and 
            append to ~/.ssh/authorizedKeys)
        """
        logger=self.logger
        rmtSSHDir = self.getRemoteSSHDir()
        if not authKeyPath:
            authKeyPath=self.localSSHDir+'/gSession.pub'
        if not rmtSSHDir:
            logger.error("%s, Unable to determine Remote SSH directory.", 
                self.userHost)
            return False
        try:
            if not os.access(authKeyPath, os.R_OK) :
                logger.error("%s Unable to access:%s \n", self.userHost,
                    authKeyPath)
                return False
            # Copy local file to remote
            try :
                rmtFilePath=rmtSSHDir+'/' + 'gSession.auth'
                scpErr=self.putFile(authKeyPath, rmtFilePath)
                if scpErr:
                    logger.error(scpErr)
                    return False
                rmtAuthKeyFile=rmtSSHDir+'/' + 'authorized_keys'
                try:
                    cmd = 'cat ' + rmtFilePath  + ' >> ' + rmtAuthKeyFile 
                    (exitStatus, stdout, stderr)=self.runCommand(cmd)
                    self.sftpClnt.remove(rmtFilePath)
                    stat=self.sftpClnt.stat(rmtAuthKeyFile)
                    self.sftpClnt.chmod(rmtAuthKeyFile, 
                        (stat.st_mode & ~0777) | 0700)
                except runCommandException, err:
                    logger.error("Command : %s for %s@%s: %s", cmd, 
                        self.user, self.host, str(err))
                    return False
            except Exception, e:
                logger.error('***Caught exception rCopy %s messaeg: %s'\
                        ' copy Authorized Key ' , str(e.__class__),  str(e))
                return False
        except Exception, e:
            errMsg="*** Caught exception (%s)access message: (%s ), "\
            "setupRemoteHost : %s " % (str(e.__class__), str(e),  self.host)
            logger.error(errMsg)
            return False


    def setupRemoteKey(self):
        """ Copy Local Public Key to remote Machine 

            authType : 'rsa', 'rsa1',  'dsa', 'gprsa'
        """
        logger=self.logger
        global keyFiles
        if not self.isReady(True):
            return False
        try:
            homeDir=os.getenv("HOME")
        except:
            logger.error("%s, Please check HOME environment variable", 
                self.userHost)
            return False
        sshDir = homeDir + '/.ssh'
        # Remote SSH Dir
        rmtSSHDir = self.getRemoteSSHDir()
        if not rmtSSHDir:
            logger.error("%s, Unable to determine Remote SSH directory.", 
                self.userHost)
            return False

        for authType in ['rsa', 'rsa1', 'dsa']:
            if not keyFiles.has_key(authType):
                logger.error("UnSupported authentication type: %s ", authType)
                continue
            # Check if the connection is ready
            # Make Sure ~/.ssh is present
            authKeyPath=sshDir+'/'+keyFiles[authType]+'.pub'
            authKeyFile=keyFiles[authType]
            logger.debug("SSH Directory : %s authKeyPath=%s authKeyFile=%s", \
                sshDir, authKeyPath, authKeyFile)
            try:
                if not os.access(authKeyPath, os.R_OK) :
                    continue
                # Copy local file to remote
                try :
                    rmtFilePath=rmtSSHDir+'/' + authKeyFile + '_' + \
                        self.userHost
                    scpErr=self.putFile(authKeyPath, rmtFilePath)
                    if scpErr:
                        logger.error(scpErr)
                        return False
                    rmtAuthKeyFile=rmtSSHDir+'/' + 'authorized_keys'
                    try:
                        cmd = 'cat ' + rmtFilePath  + ' >> ' + rmtAuthKeyFile 
                        (exitStatus, stdout, stderr)=self.runCommand(cmd)
                        self.sftpClnt.remove(rmtFilePath)
                        stat=self.sftpClnt.stat(rmtAuthKeyFile)
                        self.sftpClnt.chmod(rmtAuthKeyFile, 
                            (stat.st_mode & ~0777) | 0700)
                    except runCommandException, err:
                        logger.error("Command : %s for %s@%s: %s", cmd, 
                            self.user, self.host, str(err))
                        return False
                except Exception, e:
                    logger.error('***Caught exception rCopy %s messaeg: %s'\
                        ' copy Authorized Key ' , str(e.__class__),  str(e))
                    return False
            except Exception, e:
                errMsg="*** Caught exception (%s)access message: (%s ), "\
                "setupRemoteHost : %s " % (str(e.__class__), str(e),  self.host)
                logger.error(errMsg)
                return False
        return True

    def checkKeyFiles(self):
        """ Check if any of the private public keys are present """
        # Initialize
        global keyFiles
        logger=self.logger
        rmtSSHDir=self.getRemoteSSHDir()
        self.keyFileList=[]
        if not self.sftpClnt:
            logger.error("%s sftpClient is not yet initialized")
            return False
        for eachKey in keyFiles.keys():
            file=keyFiles[eachKey]
            privateKey=rmtSSHDir+'/'+file
            publicKey=rmtSSHDir+'/'+file+'.pub'
            statPriv=statPub=None
            try:
                statPriv=self.sftpClnt.stat(privateKey)
                statPub=self.sftpClnt.stat(publicKey)
            except IOError, e:
                pass
            if statPriv :
                self.keyFileList.append(privateKey)
            if statPub :
                self.keyFileList.append(publicKey)
        return True

    def generatePubKeys(self):
        """ Generate rsa,dsa,rsa1 private, public keys with empty passphrase
            -- Delete existings private, public keys if they are present.
            -- Generate rsa, dsa, rsa1, private public keys.
        """
        global keyFiles
        logger=self.logger
        rmtSSHDir=self.getRemoteSSHDir()
        if not rmtSSHDir:
            logger.error('Unable to determine SSH directory for %s',
                self.userHost);
            return False
        # Delete the files.
        #keyFiles={'rsa':'id_rsa', 'rsa1':'identity', 'dsa':'id_dsa'}
        print "\n Generating public-private keys for : %s \n"% self.userHost
        for eachKey in keyFiles.keys():
            file=keyFiles[eachKey]
            privateKey=rmtSSHDir+'/'+file
            publicKey=rmtSSHDir+'/'+file+'.pub'
            try :
                logger.info("Deleting keys:(%s) files %s, %s \n",
                    self.userHost, privateKey, publicKey)
                self.sftpClnt.remove(privateKey)
                self.sftpClnt.remove(publicKey)
            except IOError, e:
                # If its not there don't worry.
                pass
            except Exception, e:
                errMsg="***Error:%s Deleting Keys exception(%s) message:(%s )"\
                % (self.userHost, str(e.__class__), str(e))
                return
        for eachKey in keyFiles.keys():
            file=keyFiles[eachKey]
            privateKey=rmtSSHDir+'/'+file
            keyGenCmd='ssh-keygen -t %s -N \"\" -f %s ' % (eachKey, privateKey)
            logger.debug("Executing %s on %s \n" %(keyGenCmd, self.userHost))
            stdout=self.execCmd(keyGenCmd)
            logger.info("Executed:%s command  %s", self.userHost, keyGenCmd);

    def obtainRemotePublicKeys(self):
        """ 
            Obtain the remote Public Keys and dump them in a local file
            If the file is not there ignore.
        """
        global keyFiles
        logger=self.logger
        rmtSSHDir=self.getRemoteSSHDir()
        if not rmtSSHDir:
            logger.error("Unable to determine SSH directory for :%s ", \
                self.userHost)
            return False
        self.lock.acquire()
        localSSHHome=self.localSSHDir
        localGSDir  =self.localGSDir
        self.lock.release()
        if not localSSHHome:
            logger.error("Unable to obtain .ssh on this machine.")
            return False
        print "Obtaining public keys for %s\n" % self.userHost
        for eachKey in keyFiles.keys():
            file=keyFiles[eachKey]
            publicKey=rmtSSHDir+'/'+file+'.pub'
            if localGSDir:
                localCopy = localGSDir+ '/' + self.userHost+'_' + file + '.pub'
            else :
                localCopy=localSSHHome+'/'+'gSession/'+ \
                    self.userHost+'_' + file+'.pub'
            try :
                logger.info("Copying %s:%s to %s ", self.userHost, publicKey,
                    localCopy)
                self.sftpClnt.get(publicKey, localCopy)
            except Exception, e:
                errMsg="***Error:%s Copying Remote Keys (%s->%s) exception(%s)"\
                    "message:(%s )"  % (self.userHost, publicKey, localCopy, \
                        str(e.__class__),str(e))
                logger.error(errMsg)
        
    def setupKeyAccess(self, rsaPPhrase=None, dsaPPhrase=None, \
        rsa1PPhrase=None ):
        """ Set up keys for the user on this host """
        logger=self.logger
        rmtSSHDir=self.getRemoteSSHDir()
        if not rmtSSHDir:
            logger.error("Unable to determine SSH directory for :%s ", \
                self.userHost)
            return False

        for (keytype, file) in (("rsa", "id_rsa"), ("rsa1", "identity"), \
            ("dsa", "id_dsa")):
            rmtPubFile= rmtSSHDir+ '/' + file+ '.pub'
            logger.debug("Checking file %s :" % rmtPubFile)
            try:
                fileStat = self.sftpClnt.stat(rmtPubFile)
                #
                # The file already exists, So don't worry about it.
                # Support OverWrite in Future.
                #
                continue
            except IOError, e:
                pass
            except Exception, e:
                errStr="Caught Exception %s: %s (%s to %s) "% \
                (str(e.__class__),  str(e), lPath, rPath)
                return False
            if keytype=='rsa':  pPhrase=rsaPPhrase
            if keytype=='dsa':  pPhrase=dsaPPhrase
            if keytype=='rsa1': pPhrase=rsa1PPhrase
            if pPhrase == None: 
                # We don't support remote interactive as of now.
                continue
            else :
                keyGenCmd='ssh-keygen -t %s -f %s -N \"%s\" ' %(keytype, rmtPubFile,
                    pPhrase)
            logger.debug("About to generate a %s key. " % keytype)

            stdout=self.execCmd(keyGenCmd)
            print stdout
        return True



    def setupKeyAccessLocal(self):
        # Verify that key pairs are present on the local side 
        try:
            os.chdir(os.getenv("HOME"))
        except:
            self.__die("Can't find your home directory!")
        
        if not os.access(".", os.R_OK):
            self.__die("Your home directory is not readable by you. "\
                "This probably means $HOME is incorrect.")
        if not os.path.exists(".ssh"):
            os.mkdir(".ssh")
        if not os.access(".ssh", os.R_OK):
            self.__die("Your .ssh directory is not readable by you. "\
                    "This probably means $HOME is incorrect.")
        try:
            os.chdir(".ssh")
        except:
            self.__die("Can't chdir into your .ssh directory!")

        public_keys  = filter(lambda f: os.access(f, os.R_OK), \
            ("id_dsa.pub", "id_rsa.pub"))
        if not public_keys:
            public_keys = []
            for (keytype, file) in (("rsa", "id_rsa"), ("rsa1", "identity"), \
            ("dsa", "id_dsa")):
                print "About to generate a %s key. You will be prompted for a "\
                            " passphrase," % keytype
                print "Pressing enter at that prompt will create a key with no"\
                    " passphrase" 
                os.system("ssh-keygen -t %s -f %s" % (keytype, file))
                public_keys.append(file + ".pub")

    def updateRemoteHostKeys(self, hostArray):
        """ Update the remote host keys for each entry in the hostArray. 
        Update hostKey for eachentry in the file ~/.ssh/known_hosts
        """
        if not hostArray: return
        logger=self.logger
        # Set the Variables
        rmtSSHDir=self.getRemoteSSHDir()
        if not rmtSSHDir:
            logger.error("Unable to determine SSH directory for :%s ",\
                self.userHost)
            return False
        khPath=rmtSSHDir+'/known_hosts'
        khNewPath=khPath+'.new'
        # if the file is not there create one
        try:
            khStat=self.sftpClnt.stat(khPath)
        except IOError, e:
            touchKhStat='touch %s' % (khPath)
            stdout=self.execCmd(touchKhStat)
            logger.debug("%s : cmd : %s output: %s ", self.userHost,
                touchKhStat, stdout)
            try:
                khStat=self.sftpClnt.stat(khPath)
            except IOError, e:
                eMsg= "Error on %s: Unable to create:%s msg:%s " % \
                    (self.userHost, khPath, str(e))
                logger.error(eMsg)
                return False
        # Now, execute the keyscan command for each host, use rsa
        # TODO is rsa sufficient, it required add the all the entries
        # -t rsa, -t dsa, -t rsa1 to ~/known_hosts.
        print "Updating remote host keys on %s \n"% self.userHost
        for eachHost in hostArray:
            logger.info("update hostKey for %s on %s", eachHost, self.userHost)
            upEntCmd="grep -v %s %s > %s ; ssh-keyscan -t %s %s >> %s ;"\
            " rm -rf %s; cp %s %s" %(eachHost, khPath, khNewPath, 'rsa', \
            eachHost, khNewPath, khPath, khNewPath, khPath)
            logger.debug("%s : %s", self.userHost, upEntCmd)
            stdout=self.execCmd(upEntCmd)
            if stdout:
                logger.debug("%s: \n cmd=%s \noutPut=%s \n", self.userHost,
                    upEntCmd, stdout)
        # Fix the permissions
        try:
            khStat=self.sftpClnt.stat(khPath)
            self.sftpClnt.chmod(khPath,
                    (khStat.st_mode & ~0777) | 0644)
        except Exception, e:
            errMsg="*** Caught Exception (%s) Msg : %s while setting "\
            " permissions for %s on %s \n" % \
            (str(e.__class__), str(e), khPath, self.userHost)
            logger.error(errMsg)
            return False
        return True 

    def initializeSession(self, defaultPassword=None, defaultPassPhrase=None):
        """ Initialize ssh session """
        logger=self.logger
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.connect((self.host, self.port))
        except Exception, e:
            errStr="*** Connection %s:%d failed Error:%s" %( self.host,\
                self.port, str(e))
            logger.error(errStr)
            if debug:
                traceback.print_exc()
            return False
        try :
            self.trans= paramiko.Transport(self.sock)
            try:
                self.trans.start_client()
            except paramiko.SSHException, e:
                logger.error("*** ssh negotiation failed for %s:%d, %s", \
                    self.host, self.port, str(e))
                self.trans.close()
                return False
            try:
                hostKeyFile= os.path.expanduser('~/.ssh/known_hosts')
                logger.debug("Verifying host keys with %s", hostKeyFile)
                keys = paramiko.util.load_host_keys(hostKeyFile)
            except IOError, e:
                logger.info("Unable to load host key file %s", hostKeyFile);
                keys={}
            # Get Remote Server Key
            key = self.trans.get_remote_server_key()
            updateLocalKey=False
            if not keys.has_key(self.host):
                logger.warning('*** Unknown host key for %s ', self.host)
                updateLocalKey=True
            elif not keys[self.host].has_key(key.get_name()):
                logger.warning('*** Unknown host key for %s', self.host)
                updateLocalKey=True
            elif keys[self.host][key.get_name()] != key:
                logger.error('*** Host key has changed!!! for %s', self.host)
                logger.error('*** Please edit ~/.ssh/known_hosts and update')
                self.trans.close()
                return False
            else :
                logger.info("Host Key OK for %s", self.host)
            if updateLocalKey:
                gSessionUtil.addRemoteHostKey(self.host, prompt=False)
        except Exception, e:
            logger.error('*** Caught exception %s:%s host : %s ',  
                str(e.__class__),  str(e), self.host)
            raise sshSocketConnection, str(e)
        return True

    def makeReady(self, authenticate=False):
        """ Authenticate an ssh Connection """
        logger=self.logger
        try :
            self.initializeSession()
        except sshSocketConnection, err:
            logger.error('*** Initialization error host %s : %s ' , self.host,\
                str(err))
            raise PasswordFailure, str(err)
        if not self.trans:
            raise PasswordFailure, " *** Transport is not initialized."
        if not authenticate:
            return
        self.agent_auth()
        if self.trans.is_authenticated():
            pass
        else:
            self.publicKey_auth()
        if self.trans.is_authenticated():
            pass 
        else:
            self.passwd_auth()
        if not self.trans.is_authenticated():
            self.trans.close()
            raise PasswordFailure, " **** Transport is not authenticated" 
        logger.info("ssh Connection %s@%s:%d is authenticated : %s \n",
            self.user, self.host, self.port, self.authMode)
        self.startSftp()

    def startSftp(self):
        #
        # Start a sftpClient from the authenticated trans
        #
        try :
            self.sftpClnt= paramiko.SFTPClient.from_transport(self.trans)
        except paramiko.SFTPError, e:
            errStr='*** Unable to create sftp Client .. %s' % str(e)
            raise PasswordFailure, errStr
            return False
        except Exception, e:
            errStr="*** Sftp Client %s:%d failed Error:%s" % \
                ( self.host, self.port, str(e))
            if self.debug:
                traceback.print_exc()
            raise PasswordFailure, errStr
        #
        # Transport is now authenticated, however we need to make sure
        # we have PassPhraseLess Certificate Authentication
        #
    def setupKeys(self, setupLocal=False, authType='gpRsa'):
        """ Set Up keys for remote host """
        logger=self.logger
        if self.authMode == 'PASSWORD':
            #self.etupKeyAccessLocal()
            # NFS issue
            self.lock.acquire()
            if (self.setupRemoteKey()):
                print "Keys are setup for %s@%s" % (self.user, self.host)
            self.lock.release()
        return

    def agent_auth(self):
        """ Authenticate using local agent """
        logger=self.logger
        try :
            agent = paramiko.Agent()
        except paramiko.SSHException, e:
            # Agent is running but speaks an incompatible protocol.
            logger.error("Error, Agent is speaking an incompatible protocol %s",
                str(e))
            return False
        try :
            agent_keys = agent.get_keys()
            if(len(agent_keys)== 0):
                # Probably Agent is not running
                logger.debug("Probably agent is not running")
                return False
        except Exception, e:
            logger.error('*** Caught exception %s:%s host:%s',  
                str(e.__class__),  str(e), self.host)
            return False
        for key in agent_keys:
            logger.debug("Trying ssh-agent key %s" ,  
                paramiko.util.hexify(key.get_fingerprint()))
            try:
                self.trans.auth_publickey(self.user, key)
                logger.info("Connection established for %s@%s:%d "\
                    "using ssh-agent", self.user, self.host, self.port)
                self.authMode="AGENT"
                return True
            except paramiko.SSHException, e:
                logger.debug("Received SSHException...  Erro: %s", str(e))

    def publicKey_auth(self, rsaPassPhrase="", dsaPassPhrase=""):
        """ Public Key Authentication 
            rsagp is a rsa authentication, that use ~/.ssh/id_rsagp file
            The PassPhrase is empty for this Private Key.
        """
        #self.publicKey_auth_RSA_DSA("rsagp", keyLess=True)
        #if self.trans.is_authenticated():
        #    return True

        self.publicKey_auth_RSA_DSA("dsa", keyLess=True)
        if self.trans.is_authenticated():
            return True
        self.publicKey_auth_RSA_DSA("rsa", keyLess=True)
        if self.trans.is_authenticated():
            return True

        #
        # Now try with Key
        #
        self.publicKey_auth_RSA_DSA("rsa", rsaPassPhrase)
        if self.trans.is_authenticated():
            return True
        self.publicKey_auth_RSA_DSA("dsa", dsaPassPhrase)
        if self.trans.is_authenticated():
            return True


    def publicKey_auth_RSA_DSA(self, authType="rsa",  passPhrase="", \
        keyLess=False):
        """ Try rsa or dsa public key certificates"""
        global sshRsaPass 
        global sshDsaPass
        authMode=""
        password=""

        logger=self.logger
        logger.debug("Trying " + authType + " public key authentication ")

        if authType == "rsa" or authType ==  "dsa" or authType == 'rsagp' :
            pass
        else :
            return False

        default_path = os.path.join(os.environ['HOME'], '.ssh', 'id_' + 
                authType)
        if (authType == 'rsagp'):
            self.rsaGPFilePath=default_path

        logger.debug("Trying public keys in %s ..", default_path)
        key=None
        try:
            if authType == "rsa" or authType== 'rsagp' :
                key = paramiko.RSAKey.from_private_key_file(default_path)
                if authType == 'rsa':
                    authMode="RSA"
                if authType == 'rsagp':
                    authMode="RSAGP"
            else :
                key = paramiko.DSSKey.from_private_key_file(default_path)
                authMode="DSA"
        except paramiko.PasswordRequiredException:
            # Try with the global Variable
            if keyLess:
                return False
            if authType == "rsa":
                authMode="RSAPHRASE"
            else :
                authMode="DSAPHRASE"
            if authType == "rsa" and sshRsaPass:
                authMode='RSAPHRASE'
                try:
                    key = paramiko.RSAKey.from_private_key_file(
                            default_path, sshRsaPass)
                except paramiko.SSHException, e:
                    logger.debug("Received SSHException  %s, %s for %s ", \
                        str(e.__class__),  str(e), self.host)
                    key = ""
            if authType == "dsa" and sshDsaPass:
                authMode='DSAPHRASE'
                try:
                    key = paramiko.DSSKey.from_private_key_file(
                            default_path, sshDsaPass)
                except paramiko.SSHException, e:
                    logger.debug("Received SSHException  %s, %s for %s ", \
                        str(e.__class__),  str(e), self.host)
                    key = ""
            tries=0
            while tries < maxTries and not key:
                # Enter passphrase for key '/Users/ksrikanth/.ssh/id_dsa':
                password = getpass.getpass("Enter passphrase for key '%s':" %\
                    default_path)
                try :
                    if authType=='rsa':
                        key = paramiko.RSAKey.from_private_key_file(
                            default_path, password)
                    else : 
                        key = paramiko.DSSKey.from_private_key_file(
                            default_path, password)
                except paramiko.SSHException, e:
                    logger.debug("Received SSHException  %s, %s for %s ", \
                        str(e.__class__),  str(e), self.host)
                    key = ""
                    tries+=1
                    continue
                if authType == 'rsa':
                    sshRsaPass=password
                else :
                    sshDsaPass=password
                break
            if tries >= maxTries:
                logger.error("Reached Maximum tries for key '%s:'",default_path)
                return False
        except IOError:
            logger.debug("Received IO Error, probably %s is not present", \
                default_path)
        except paramiko.SSHException, e:
            logger.info("Received SSHException  %s, %s for %s ", \
                str(e.__class__),  str(e), self.host)
        if key:
            try :        
                self.trans.auth_publickey(self.user, key)
            except paramiko.SSHException, e:
                logger.info("Received SSHException  %s, %s for %s ", \
                    str(e.__class__),  str(e), self.host)
            # Update the global password
            #print self.trans.is_authenticated()

            if True or self.trans.is_authenticated() :
                if password :
                    #print "Setting %s global Pass" % authType
                    if authType == 'rsa':
                        sshRsaPass=password
                    else :
                        sshDsaPass=password
                self.authMode=authMode
        else :
            return False

    def authPublicKey(self, key):
        """ Authenticate using a public Key """
        rTrans=self.trans
        if not rTrans:
            return False
        try:
            rTrans.auth_publickey(self.user, key)
        except paramiko.SSHException, e:
            return False
        if rTrans.is_authenticated():
            return True
        return False

    def authPasswd(self, password):
        """ Authenticate using the password provided """
        rTrans=self.trans
        if not rTrans:
            return False
        try:
            rTrans.auth_password(self.user, password)
        except paramiko.SSHException, e:
            return False
        if rTrans.is_authenticated():
            return True

    def setAuthMode(self, authMode):
        """ Set the authentication mode"""
        self.authMode= authMode

    def setLocalSSHDir(self, dirName):
        """ Set the authentication mode"""
        self.localSSHDir=dirName

    def setLocalGSDir(self, dirName):
        """ Set the authentication mode"""
        self.localGSDir =dirName


    def closeTrans(self):
        """ Close the transport opened for this session."""
        if not self.trans:
            return
        self.trans.close()


    def passwd_auth(self):
        """ Try authenticating using password.
            First try the globalPassword, Second sessionPasword
            Third Prompt user 3 times for password from the key board
        """
        logger=self.logger
        logger.debug('Trying passwd auth... ')
        global sshPasswd
        if (sshPasswd):
            logger.debug('Trying default Password : %s ', sshPasswd)
            try:
                self.trans.auth_password(self.user, sshPasswd)
            except paramiko.SSHException, e:
                logger.debug("Received SSHException  %s, %s for %s ", \
                    str(e.__class__),  str(e), self.host)
        if (self.trans.is_authenticated()):
            self.authMode='PASSWORD'
            return True

        if (self.password):
            logger.debug('Trying self Password : %s ', self.password)
            try:
                self.trans.auth_password(self.user, self.password)
            except paramiko.SSHException, e:
                logger.debug("Received SSHException  %s, %s for %s ", \
                    str(e.__class__),  str(e), self.host)

        if (self.trans.is_authenticated()):
            logger.info("Connection established for %s@%s:%d ", self.user, 
                self.host, self.port)
            self.authMode='PASSWORD'
            return True
        # Now try from the keyboard
        tries = 0
        while tries < maxTries:
            pw = getpass.getpass('Password for %s@%s:%d ' % (self.user, \
                self.host, self.port))
            try :
                self.trans.auth_password(self.user, pw)
            except paramiko.SSHException, e:
                logger.info("Received SSHException  %s, %s for %s ", \
                    str(e.__class__),  str(e), self.host)
                tries+=1
                continue
            # Successful attempt
            if self.trans.is_authenticated():
                sshPasswd=pw
            break

        if tries == maxTries:
            logger.error("Reached maximum tries for password authentication.")
            return False

        if self.trans.is_authenticated():
            logger.info("Connection established for %s@%s:%d ", self.user, 
                self.host, self.port)
            self.authMode='PASSWORD'
        return True


def rptTest(sess, cmd='uptime', times=10):
    cntr=times
    while cntr:
        sess.setCMD('ssh', [cmd])
        res=sess.run()
        print res
        cntr-=1
    
    
    
if __name__ == '__main__': 
    try :
        a = sshSession('testuser2',22,'10.0.0.216')
        a.makeReady(authenticate=True)
        a.setCMD('ssh', ['uname -a'])
        print "Authenticated..."
        #signal.signal(signal.SIGTERM, handlerTERM)
        cmd='date; sleep 10; date '
        print "Running command"
        try:
            print "executing %s " % cmd
            #a.setCMD('scp', ['/Users/ksrikanth/fileOne', 'fileOne'])
            #res=a.run()
            #print res
            #res=a.execCmd(cmd)
            #print res
            #a.setCMD('ssh', ['date; sleep 1; date '])
            #res=a.run()
            #print res
            #print "Copying File"
            #a.setCMD('scp', ['/Users/ksrikanth/fileOne', 'fileOne'])
            #res=a.run()
            #print res
            #a.setCMD('ssh', ['~/tmpdir1/stream'])
            #a.setCMD('ssh', ['cat fileOne'])
            #res=a.run()
            #print len(res)
            rptTest(a, 'uptime', times=100)
        except SystemExit:
            raise
        except KeyboardInterrupt:
            print "Received KeyBoard Interrupt"
            raise SystemExit, 2
        raise SystemExit, 0

        a.checkKeyFiles()
        if a.keyFileList:
            for eFile in a.keyFileList:
                print eFile
        #a.generatePubKeys()
        #a.checkKeyFiles()
        #if a.keyFileList:
            #for eFile in a.keyFileList:
                #print eFile
        a.updateRemoteHostKeys(['10.0.0.216', '10.0.0.214', \
            'buzzard1', \
            'buzzard2', \
            'buzzard3', \
            'buzzard1' ])
    except SystemExit:
        raise
    except Exception, e:
        print ('*** Caught exception %s:%s ',  
                str(e.__class__),  str(e))
    raise  SystemExit, 0

