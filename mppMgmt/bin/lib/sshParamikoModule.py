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
import commands

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
sshPasswd=""
sshRsaPass=""
sshDsaPass=""
debug=False

keyFiles={'rsa':'id_rsa', 'rsa1':'identity', 'dsa':'id_dsa', 'gpRsa':'id_rsagp'}

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

def setupLocalKey(authType, overWrite=False, emptyPhrase=False):
    """ Generate ssh Key on the local machine 
        
        authType= 'rsa', 'rsa1', 'dsa', 'gpRsa'
        overWrite :: Overwrite Existing File
        emptyPhrase :: Generate Key with Empty Phrase.
        Return  :: True on success
    """
    logger=logging.getLogger('groupSession')
    global keyFiles
    if not keyFiles.has_key(authType):
        logger.error("UnSupported authentication type: %s ", authType)
        return False
    # Make Sure ~/.ssh is present
    try:
        homeDir=os.getenv("HOME")
    except:
        logger.error("Error determinig HOME evnironment variable.")
        return False
    sshDir = homeDir + '/.ssh'
    logger.debug("SSH Directory : %s ", sshDir)
    try :
        if not os.access(sshDir, os.R_OK):
            logger.debug("Creating " + sshDir)
            os.mkdir(sshDir)
            logger.debug("chmod 700 " + sshDir)
            os.system('chmod 700 ' + sshDir)
    except Exception, e:
        logger.error("*** Caught exception (%s) message: (%s )" \
            % (str(e.__class__), str(e)))
        return False
    authKeyPath=sshDir+'/'+keyFiles[authType]
    authKeyBackUp=authKeyPath+'_backUp'
    authKeyFile=keyFiles[authType]
    try:
        if os.access(authKeyPath, os.R_OK) :
            if not overWrite:
                return True
            else:
                cmd='mv ' + authKeyPath+ ' ' + authKeyBackUp
                print cmd
                os.system('mv ' + authKeyPath+ ' ' + authKeyBackUp)
        if authType == 'gpRsa':
            sshAuth='rsa'
        else :
            sshAuth = authType
        genCommand = 'ssh-keygen -f ' + authKeyPath + ' -t ' + sshAuth
        if emptyPhrase:
            genCommand=genCommand+ ' -P "" ' 
        try :
            logger.debug("Executing ::" + genCommand)
            os.system(genCommand)
        except Exception, e:    
            errMsg="*** Error  %s caught exception (%s) :"\
            "message: (%s ) " % (genCommand, str(e.__class__), str(e))
            logger.error(errMsg)
            if os.access(authKeyBackUp, os.W_OK):
                os.system('mv ' + authKeyBackUp + ' ' + authKeyPath)
            return False
        if os.access(authKeyBackUp, os.W_OK):
            os.system('rm -rf ' + authKeyBackUp)
    except Exception, e:    
        errMsg="*** Generating ssh Key %s Caught exception (%s) :"\
        "message: (%s )" %  \
        (genCommand, str(e.__class__), str(e))
        logger.error(errMsg)
        return False
    return True

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
        setupKeysFlag=True):
        threading.Thread.__init__(self)        
        logger=logging.getLogger(logName)
        logger.info("Initializing sshSession %s@%s:%d ", user, host, port)
        self.stdout=""
        self.stderr=""
        self.exitStatus=0

        self.lock = threading.RLock()
        self.verbose = verbose
        self.user = user
        self.port = port
        self.host = host
        self.password = password
        self.userHost=self.user + '@' + self.host
        self.authMode=""

        #Output of last command executed
        self.lastoutput = None
        self.cmd = None
        self.args = []
        self.timeout = 30

        self.PWCheck = False
        self.PWRequired = False

        self.PPRequired = False
        self.hostKeyPhrase = None

        self.child = None

        # Paramiko Variables
        self.trans=None
        self.chan=None

        self.setUpKeys=setupKeysFlag

        #Flag that indicates if this session object is able to execute 
        #commands
        self.ready = False
        self.error = False

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
        logger=logging.getLogger('groupSession')
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
        logger = logging.getLogger('groupSession')
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
        logger=logging.getLogger('groupSession')
        #Expand the ~ and ~<userName> at the begining of the file
        lPath=os.path.expanduser(localPath)
        rPath="%s@%s:%s" %(self.user, self.host, remotePath)
        logger.info("Copying %s to %s", lPath, rPath)
        try :
            self.sftpClnt.put(lPath, remotePath)
        except IOError, e:
            errStr='Unable to Copy (%s to %s):%s' % (lPath, rPath, str(e))
            logger.error(errStr)
            return errStr
        except SFTPError, e:
            errStr= 'Received SFTP Client Error (%s to %s):%s' % \
                (lPath, rPath, str(e))
            logger.error(errStr)
            return errStr
        except Exception, e:
            errStr="Caught Exception %s: %s (%s to %s) "% \
                (str(e.__class__),  str(e), lPath, rPath)
            logger.error(errStr)
            if self.debug:
                traceback.print_exc()
            return errStr 
        #Has to Reuturn String for runCommand in groupSession
        logger.info("Finished Copying %s to %s", lPath, rPath)
        return  ""

    def execCmd(self, command):
        """ Wrapper for run command, Returns only stdout"""
        logger=logging.getLogger('groupSession')
        logger.debug("execCmd %s on %s ", command, self.userHost)
        stdout=""
        stderr=""
        try:
            (exitStatus, stdout, stderr)=self.runCommand(command)
        except runCommandException, err:
            logger.info('Error Executing %s on %s %s',  \
                command, self.userHost, err)
        except Exception, e:
            logger.error('*** Caught exception %s: %s executing %s on %s ', 
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
        logger=logging.getLogger("groupSession")
        self.chan=None
        if (not self.trans):
            raise runCommandException("Transport is not initialized.")
        if (not self.trans.is_authenticated()):
            raise runCommandException("Transport is not authenticated.")
        if (not command):
            raise runCommandException("Command not provided.")
        try:
            self.chan = self.trans.open_session()
            self.chan.exec_command(command)
        except paramiko.SSHException, e:
            raise runCommandException(str(e))
        except Exception, e:
            errMsg="*** Caught exception (%s) message: (%s ), command (%s)"\
                " host : %s " % (str(e.__class__), str(e), command, self.host)
            logger.debug(errMsg)
            raise runCommandException(errMsg)
        logger.debug("Waiting for error Code")
        exit_status=0
        #
        # TODO Need to implement a timeout here 
        # However, taking care of CTRL-C would really solve the issue here.
        #
        exit_status=self.chan.recv_exit_status()
        logger.debug("Error Code : %d %s executed on %s \n", exit_status,
            command, self.host)
        stdout=""
        stderr=""
        # Obtain the stderr of the command
        # Buffered stderr.
        while(self.chan.recv_stderr_ready()):
            recvBytes=self.chan.recv_stderr(1024)
            if not recvBytes:
                break
            stderr+=recvBytes
        # Obtain the stdout of the command
        while (True):
            recvBytes=self.chan.recv_stderr(1024)
            if not recvBytes:
                break
            stderr+=recvBytes
        # Obtain the stdout of the command
        while(self.chan.recv_ready()):
            recvBytes=self.chan.recv(1024)
            if not recvBytes:
                break
            stdout+=recvBytes
        while (True):
            recvBytes=self.chan.recv(1024)
            if not recvBytes:
                break
            stdout+=recvBytes
        self.chan.close()
        self.chan=None
        logger.debug("Command = <%s> exit_status=%d \n stdout=%s stderr=%s\n",
            command, exit_status, stdout, stderr)
        self.stdout=stdout
        self.stderr=stderr
        self.exitStatus=exit_status
        return (self.exitStatus, self.stdout, self.stderr)

    def killCMD(self,sig=signal.SIGTERM):
        if self.chan:
            logger=logging.getLogger('groupSession')
            logger.info("Closing channel %s@%s", self.user, self.port)
            self.chan.close()

    def run(self):
        if self.cmd is "ssh":
            #print self.args[0]
            #print self.args
            self.lastoutput = self.execCmd(self.args[0])
        
        elif self.cmd is "scp":
            self.lastoutput = self.putFile(self.args[0], self.args[1])
                    
        elif self.cmd is "dirCopy":
            self.lastoutput = self.putDir(self.args[0], self.args[1])
                    
        elif self.cmd is "scpOld":
                scpOptions=[]
                for i in range(2, len(self.args)):
                    scpOptions.append(self.args[i])
                self.lastoutput = self.scp(self.args[0], self.args[1], \
                        scpOptions)
        elif self.cmd is "scprem":
                self.lastoutput = self.scprem(self.args[0], self.args[1])

        elif self.cmd is "dircopy":
                self.lastoutput = self.dircopy(self.args[0], self.args[1])

        elif self.cmd is "exists":
                self.lastoutput = self.exists(self.args[0])
        else:
                self.lastoutput = "No interface selected"
        return self.lastoutput

    def exists(self, file):
        "Retrieve file permissions of specified remote file."
        logger=logging.getLogger('groupSession')
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

    def setupRemoteKey(self):
        """ Copy Local Public Key to remote Machine 

            authType : 'rsa', 'rsa1',  'dsa', 'gprsa'
        """
        logger=logging.getLogger('groupSession')
        global keyFiles
        if not self.isReady(True):
            return False
        try:
            homeDir=os.getenv("HOME")
        except:
            logger.error("Error evnironment variable HOME is not set properly.")
            return False
        sshDir = homeDir + '/.ssh'
        # Determine Remote Node Stuff.
        try:
            (exitStatus, stdout, stderr)=self.runCommand('echo $HOME')
        except runCommandException, err:
            logger.error("Unable to determine HOME for %s@%s: %s",
                self.user, self.host, str(err))
            return False
        except Exception, e:
            logger.error('*** Caught exception %s: %s ', str(e.__class__), \
                str(e))
            traceback.print_exc()
            return False
        rmtHomeDir=stdout.strip()
        if not rmtHomeDir:
            logger.error("Unable to determine the Home Directory for  "\
                " %s@%s " , self.user, self.home)
            return False
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
                return False
        except Exception, e:
            errMsg="*** Caught exception (%s) message: (%s ), "\
            " remote SSHDIR (%s) host : %s " % (str(e.__class__), str(e),  
                self.host, rmtSSHDir)
            logger.error(errMsg)
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
                    rmtFilePath=rmtSSHDir+'/' + authKeyFile + '_' + self.host
                    scpErr=self.putFile(authKeyPath, rmtFilePath)
                    if scpErr:
                        logger.error(scpErr)
                        return False
                    rmtAuthKeyFile=rmtSSHDir+'/' + 'authorized_keys'
                    try:
                        cmd = 'cat ' + rmtFilePath  + ' >> ' + rmtAuthKeyFile 
                        (exitStatus, stdout, stderr)=self.runCommand(cmd)
                        self.sftpClnt.remove(rmtFilePath)
                        self.sftpClnt.stat(rmtAuthKeyFile)
                        self.sftpClnt.chmod(rmtAuthKeyFile, 
                            (stat.st_mode & ~0777) | 0700)
                    except runCommandException, err:
                        logger.error("Command : %s for %s@%s: %s", cmd, 
                            self.user, self.host, str(err))
                        return False
                except Exception, e:
                    logger.error('***Caught exception %s messaeg: %s'\
                        ' copy Authorized Key ' , str(e.__class__),  str(e))
                    return False
            except Exception, e:
                errMsg="*** Caught exception (%s) message: (%s ), "\
                "setupRemoteHost : %s " % (str(e.__class__), str(e),  self.host)
                logger.error(errMsg)
                return False
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
            for (keytype, file) in (("rsa", "id_rsa"), ("rsa1", "identity"), ("dsa", "id_dsa")):
                print "About to generate a %s key. You will be prompted for a "\
                            " passphrase," % keytype
                print "Pressing enter at that prompt will create a key with no"\
                    " passphrase" 
                os.system("ssh-keygen -t %s -f %s" % (keytype, file))
                public_keys.append(file + ".pub")

    def setupKeyAccessRemote(self):
        try:
            #This is actually a no-no with threads, they all share a 
            # FS node in Python
            self.lock.acquire()
            dir = os.getenv("HOME") + '/.ssh'
            os.chdir(dir)
            self.lock.release()
        except:
            self.__die("Can't chdir to your ssh directory!")
        public_keys  = filter(lambda f: os.access(f, os.R_OK), \
            ("id_dsa.pub", "id_rsa.pub", "identity.pub"))

        #print "I see the following public keys:", string.join(public_keys,", ")
        for file in public_keys:
            private_key = file[:-4]
            legend = private_key + " corresponding to " + file + " " 
            if not os.access(private_key, os.R_OK):
                self.__die(legend + "does not exist or is unreadable.")


    def initializeSession(self, defaultPassword=None, defaultPassPhrase=None):
        """ Initialize ssh session """
        logger=logging.getLogger('groupSession')
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
                addRemoteHostKey(self.host, prompt=False)
        except Exception, e:
            logger.error('*** Caught exception %s:%s host : %s ',  
                str(e.__class__),  str(e), self.host)
            raise sshSocketConnection, str(e)

    def makeReady(self):
        """ Authenticate an ssh Connection """
        logger=logging.getLogger('groupSession')
        try :
            self.initializeSession()
        except sshSocketConnection, err:
            logger.error('*** Initialization error host %s : %s ' , self.host,\
                str(err))
            raise PasswordFailure, str(err)

        if not self.trans:
            raise PasswordFailure, " *** Transport is not initialized."
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
        logger=logging.getLogger('groupSession')

        #print self.authMode
        if self.authMode == 'PASSWORD':
            self.setupKeyAccessLocal()
            self.setupRemoteKey()
            print "Keys are setup for %s@%s" % (self.user, self.host)
        return

    def agent_auth(self):
        """ Authenticate using local agent """
        logger=logging.getLogger('groupSession')
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

        logger=logging.getLogger('groupSession')
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

    def passwd_auth(self):
        """ Try authenticating using password.
            First try the globalPassword, Second sessionPasword
            Third Prompt user 3 times for password from the key board
        """
        logger=logging.getLogger('groupSession')
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

if __name__ == '__main__': 
    gpLogger.initLogger(lName="groupSession", lfName="groupSession",  \
        debug=False, verbose=False)
    logger=logging.getLogger('groupSession')
    try :
        a = sshSession('testuser2',22,'supertracs')
        a.makeReady()
        a.setCMD('ssh', ['uname -a'])
        res=a.run()
        print res
    except Exception, e:
        logger.error('*** Caught exception %s:%s ',  
                str(e.__class__),  str(e))
    raise  SystemExit, 0
    try:
        a.setCMD('ssh', ['uname -a'])
        #a.setCMD('ssh', ['uname -a; sleep 2; uname -i; uname -m'])
        #print '%s' % (a.run())
        #a.setCMD('scp', ['~/xx.sh', 'x1.sh'])
        #a.run()
    except NOTREADY, e:
        print '%s:%s' % (e,'ssh session for ' + a.hostname + \
            ' not ready for commands.')

