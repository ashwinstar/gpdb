#!/usr/bin/env python

"""
groupSession -- ssh access to multiple hosts at once

Usage:
    groupSession <options> hostfile

hostfile - a list of remote hosts, each entry of the form:
    username@host
    username@host2
    ...
    userN@hostN
or just:
    host1
    host2
    ...
    hostN

Options:
-h,-?     Print usage summary and exit
-v        Report all commands sent to host and output received.
-p <port> Connect on a particular port in place of 22
-s        Disable the setup of trusted hosts key access
-l <fileName> Log all the messages to this file
-x        Establish trusted key access across all sessions, using ssh-keys. 


Runs commands on a list of hosts.  Manages passwords and trusted key access
using ssh.

Author: Luke Lonergan <llonergan@greenplum.com>"""

# $Id$

__all__ = ['runCommand', 'handlerTERM']

__author__ = "Luke Lonergan"
__version__ = "1.0"
__date__ = "2005-09-26"

import os, sys, threading, time, string, getpass, getopt, signal, commands
import traceback

if sys.version_info < (2, 3):
    print "Error groupSession is supported on Python versions 2.3 or greater."
    print "Please upgrade python installed on this machine."
    raise SystemExit, -1
#
# A tiny Kludge that will add '/lib' to the path
#
sys.path.append(sys.path[0]+'/lib')

try :
    import sshParamikoModule as sshParamikoModule, threadpool as threadpool
    import paramiko
    import cmd, atexit
    import gpLogger
    import logging
    import gSessionUtil
except ImportError, e:
    print "Error Importing necessary modules: %s " % str(e)
    raise SystemExit, -1


try:
    import readline
    #Read in the saved command history, if any
    histfile = os.path.join(os.environ["HOME"], ".gshist")
    # Set the maximum number of commands to 100
    readline.set_history_length(500)
    try:
        readline.read_history_file(histfile)
    except IOError:
        pass
    atexit.register(readline.write_history_file, histfile)
except Exception, e:
    print "Note: command history unsupported on this machine ..."

class groupSession(cmd.Cmd):
    "A group of sessions attached to a set of remote hosts"

    """Init Args:
        hosts     = an array of tuples (hostname,user)
        port    = the port to communicate with
        """
    def __init__(self,hostuserport, setupKeys=True, verboseOpt=False, debug=False, \
        gLogger=None):
        cmd.Cmd.__init__(self)

        self.prompt = "=> "
        self.intro = "GroupSession - execute commands on a cluster of machines\n"\
        " \"help\" for a list of commands"
        self.verbose = verboseOpt

        if gLogger:
            self.logger=gLogger
        else :
            gpLogger.initLogger(lName='groupSession', debug=debug, \
                verbose=verboseOpt)
            self.logger=logging.getLogger('groupSession')
                
        self.logger.info(" Initializing groupSession .. \n")



        #Create a threadpool to run the commands
        self.pool = threadpool.ThreadPool(len(hostuserport))

        # Create an sshSession object for each host
        self.sessions = []
        self.hostList=[]
        for host,user,port in hostuserport:
            #Create a new instance of an sshSession object initialized with this
            #host/user/port
            sshSess = sshParamikoModule.sshSession(user,port,host, logger=gLogger)
            self.sessions.append(sshSess)

        try:
            if not self.__authenticateHosts():
                self.__die("Failed to athenticate.. ")
            if setupKeys:
                self.__setupHostAccessKeys()
        except sshParamikoModule.PasswordFailure, e:
            print '%s: %s' % (e,'Failed to authenticate.')
            raise SystemExit, -1
        except sshParamikoModule.NOTREADY, e:
            errStr="*** Received NOTREADY Exception Error %s "% str(e)
            print errStr
            raise SystemExit, -1
        except KeyboardInterrupt:
            raise
        except SystemExit:
            raise
        except Exception, e:
            errStr="*** Received Exception Error %s %s "% \
                (str(e.__class__),str(e))
            traceback.print_exc()
            print errStr
            raise SystemExit, -1


    #Command class overrides ------------------------------------------------
    def default(self,line):
        cmd = 'ssh'
        args = [line,None]
        
        #Run the command on all sessions
        try:
            interrupted = self.runCommand(cmd,args,line)
            if interrupted:
                print 
                print "Command is interrupted."
                print
        except SystemExit:
            raise
        except KeyboardInterrupt:
            raise



    def do_rcopy(self,line):
        logger=self.logger
        i=0
        source = None
        dest = None
        for str in line.split():
            i = i+1
            if i == 1:
                source = str.strip()
            if i == 2:
                dest = str.strip()
        if i > 2:
            self.help_rcopy()
            return

        if source is None:
            print 'source file (on this host): ',
            source = sys.stdin.readline().strip()
            print ''

        if dest is None:
            print 'destination file on remote host <'+source+'>: ',
            dest = sys.stdin.readline().strip()
            print ''

        if dest == '':
            dest = source

        if os.path.isdir(os.path.expanduser(source)):
            print '%s is a directory, directory copy is not supported.' % \
                os.path.expanduser(source)
            return
        else :
            cmd = 'scp'

        args = [source,dest]

        #Run the command on all sessions
        try:
            interrupted = self.runCommand(cmd,args)
            if interrupted:
                print "\nCopy Interrupted\n"
        except KeyboardInterrupt:
            print "Command Interrupted..."
            raise
        except Exception, e:
            errStr="Caught Exception %s: %s "% \
                (str(e.__class__),  str(e))
            if self.debug:
                traceback.print_exc()
        #Has to Reuturn String for runCommand in groupSession

    def help_rcopy(self):
        print '\nCopies the source file on this machine to a destination on the remote machine.'
        print 'You can specify source and/or destination on the command line, or if either are'
        print 'omitted, you will be prompted.\n'
        print 'usage:\nrcopy [source_file] [dest_file]'

    def emptyline(self):
        pass

    def do_hist(self, args):
        """Print a list of commands that have been entered"""
        print self._hist

    def precmd(self, line):
        """ This method is called after the line has been input but before
        it has been interpreted. If you want to modifdy the input line
        before execution (for example, variable substitution) do it here.
        """
        self._hist += [ line.strip() ]
        return line

    def postloop(self):
        """Take care of any unfinished business.
        Despite the claims in the Cmd documentaion, Cmd.postloop() is not a stub.
        """
        cmd.Cmd.postloop(self)   ## Clean up command completion
        print "groupSession exit."

    def preloop(self):
        """Initialization before prompting user for commands.
        Despite the claims in the Cmd documentaion, Cmd.preloop() is not a stub.
        """
        cmd.Cmd.preloop(self)   ## sets up command completion
        self._hist    = []      ## No history yet
        self._locals  = {}      ## Initialize execution namespace for user
        self._globals = {}


    def do_help(self, args):
        """groupSession help:
        'help' or '?' with no arguments prints a list of commands for which help is available
        'help <command>' or '? <command>' gives help on <command>
        Other than these special commands, what you type will be executed on the remote hosts
        """
        ## The only reason to define this method is for the help text in the doc string
        cmd.Cmd.do_help(self, args)

    def do_exit(self, args):
        """Exits from the console"""
        for th in self.sessions:
            th.killCMD(signal.SIGKILL)
        return -1

    def do_quit(self,args):
        """Exits from the console"""
        return -1

    def do_q(self,args):
        """Exits from the console"""
        return -1

    ## Command defunitions to support Cmd object functionality ##
    def do_EOF(self, args):
        """Exit on system end of file character"""
        return self.do_exit(args)

    def do_shell(self, args):
        """Pass command to a system shell when line begins with '!'"""
        os.system(args)

    #End command class overrides ---------------------------------------------

    def runCommand(self,cmd,args, command="", begin=0,end=0,quiet=False, \
        retDict=False,  resDict=None, useHost=False):
        #This kludge is here because python doesn't support copiable iterators
        logger=self.logger
        if end == 0:
            end = len(self.sessions)

        #Put commands into threaded queue
        for i in range(begin,end):
            th = self.sessions[i]
            logger.debug("Executing command on host:%s " % th.userHost)
            th.setCMD(cmd,args)
            self.pool.putRequest(threadpool.WorkRequest(th.run))
        interrupted = False
        #Wait for completion of all in queue, or a keyboard interrupt
        while True:
            try:
                self.pool.poll(block=True)
                #self.pool.poll(block=False)
            except (threadpool.NoResultsPending):
                interrupted=False
                break
            except (KeyboardInterrupt):
                #self.pool.wait()
                for i in range(begin,end):
                    th = self.sessions[i]
                    th.killCMD(signal.SIGKILL)
                interrupted=True
                break
        if interrupted:
            return interrupted
        if not quiet:
          if not interrupted:
            #Print the output
            lastTmp = None
            for th in self.sessions:
                if th.lastoutput == lastTmp:
                    print '%-16s: %-s' % (th.userHost,  'Output from host ' + 
                        th.host + ' is the same as the last host')
                else:
                      for str in th.lastoutput.splitlines(True):
                        print '%-16s: %-s' % (th.userHost, str),
                      lastTmp = th.lastoutput
          else:
                print
                print 'Command interrupted.'
                print
          print ""
        if retDict:
            if not interrupted:
                for th in self.sessions:
                    if useHost:
                        resDict[th.host]=th.lastoutput
                    else:
                        resDict[th.userHost]=th.lastoutput
                    if self.verbose:
                        print th.host, th.lastoutput
        return interrupted

    def handlerTERM(self,signo,frame):
        print 'Killing commands on remote hosts...'
        for th in self.sessions:
            th.killCMD(signal.SIGKILL)
        print 'done.  Exiting.'
        self.__die('\nSignal caught, program ')
    
    def __die(self,msg):
        print 
        print msg + ' exiting.'
        print 
        sys.exit()

    def obtainLocalKey(self, authType, def_path, maxTries=3):
        """ Obtain the key from path provided 
            Return Key or None on Failure
        """
        logger=self.logger
        key=None
        # check authType
        if authType != 'rsa' and authType != 'dsa':  
            logger.error("Invalid authType (%s) ", authType)
            return None
        # Check access
        if not os.access(def_path, os.R_OK):
            logger.debug("%s is not accessible (probably not there)", def_path)
            return None
        # Try to get the key witout password
        try:
            if authType == 'rsa':
                key = paramiko.RSAKey.from_private_key_file(def_path)
            else :
                key = paramiko.DSSKey.from_private_key_file(def_path)
        except paramiko.PasswordRequiredException:
            tries = 0 
            while tries < maxTries and not key:
                # Enter passphrase for key '/Users/ksrikanth/.ssh/id_dsa':
                password = getpass.getpass("Enter passphrase for key '%s':" %\
                    def_path)
                try :
                    if authType=='rsa':
                        key = paramiko.RSAKey.from_private_key_file(
                            def_path, password)
                    else : 
                        key = paramiko.DSSKey.from_private_key_file(
                            def_path, password)
                except paramiko.SSHException, e:
                    logger.debug("Received SSHException  %s, %s for %s ", \
                        str(e.__class__),  str(e), authType)
                    key = ""
                    tries+=1
                    continue
            if tries >= maxTries:
                return None
        return key
            
    def __authenticateHosts(self, maxTries=3):
        """ Authenticate the list of hosts
            if id_rsa and id_rsa.pub are present try it
            if id_dsa and id_rsa.pub are present try it
            if identidy and identiry.pub are present try it
            then try password
        """
        # Initialize
        logger=self.logger
        print "Authenticating hosts.."
        rsaKey=dsaKey=passwd=None
        rsaFailed=dsaFailed=idFailed=False
        retVal=True
        # Get Local SSH Dir
        dotSSH=gSessionUtil.getLocalSSHDir(False)
        icurr=0
        while icurr < len(self.sessions):
            eSess= self.sessions[icurr]
            icurr += 1
            try:
                if (not eSess.initializeSession()):
                    print "Unable to initialize session: %s." % eSess.userHost
                    retVal=False
                    break
            except sshParamikoModule.sshSocketConnection, e:
                # TODO Do something nice here
                retVal=False
                break
            # Authenticate... 
            authType=""
            # RSA
            if rsaKey:
                if(eSess.authPublicKey(rsaKey)):
                    authType='rsa'
            elif not rsaFailed :
                keyType='rsa'
                rsaPriv=dotSSH + '/' + gSessionUtil.keyFiles[keyType]
                rsaKey=self.obtainLocalKey(keyType, rsaPriv)
                if not rsaKey:
                    rsaFailed=True
                else :
                    if(eSess.authPublicKey(rsaKey)):
                        authType='rsa'
                        logger.info("%s is authenticated via %s", eSess.userHost, 
                            authType)
            else :
                pass
            # DSA
            if not authType:
                if dsaKey:
                    if(eSess.authPublicKey(dsaKey)):
                        authType='dsa'
                elif not dsaFailed :
                    keyType='dsa'
                    dsaPriv=dotSSH + '/' + gSessionUtil.keyFiles[keyType]
                    dsaKey=self.obtainLocalKey(keyType, dsaPriv)
                    if not dsaKey:
                        dsaFailed=True
                    else:
                        if(eSess.authPublicKey(dsaKey)):
                            authType='dsa'
                            logger.info("%s is authenticated via %s", eSess.userHost,
                                authType)
                else :
                    pass
            # Password
            if not authType:
                if passwd:
                    if(eSess.authPasswd(passwd)):
                        authType='PASSWORD'
                tries = 0;
                while tries < maxTries and not authType :
                    passwd = getpass.getpass('Password for %s@%s:%d ' % \
                        (eSess.user,  eSess.host, eSess.port))
                    if(eSess.authPasswd(passwd)):
                        authType='PASSWORD'
                        logger.info("%s is authenticated via %s", eSess.userHost,
                                authType)
                    else:
                        tries+=1
            if not authType:
                print "Authentication failed for %s " % eSess.userHost
                retVal=False
                # Probably can return and break.
            else :
                eSess.setAuthMode(authType)
                try:
                    eSess.startSftp()
                except sshParamikoModule.PasswordFailure, e:
                    print "Unable to start sftpClient.. \n"
                    retVal=False
                logger.info("%s is Authenticated (%s)" % (eSess.userHost, authType))
        if retVal == False:
            for j in range (0,  len(self.sessions)):
                jSess= self.sessions[j]
                jSess.closeTrans()
        return retVal
                    
    def __setupHostAccessKeys(self):
        # SetUp Local Keys
        self.sessions[0].setupKeyAccessLocal()
        for i in range(0,len(self.sessions)):
            th = self.sessions[i]
            self.pool.putRequest( threadpool.WorkRequest(th.setupKeys,
                None,None,None))
            #Wait for completion of all in queue, or a keyboard interrupt
        while True:
            try:
                self.pool.poll(block=True)
            except (threadpool.NoResultsPending,KeyboardInterrupt):
                    break
    

    def confirmFileRemoval(self):
        """ Check if there are files that will be removed.
            return True if user has opted to remove return False 
        """
        for i in range(0,len(self.sessions)):
            th = self.sessions[i]
            self.pool.putRequest( threadpool.WorkRequest(th.checkKeyFiles,
                None,None,None))
            #Wait for completion of all in queue, or a keyboard interrupt
        interrupted=False
        while True:
            try:
                self.pool.poll(block=True)
            except threadpool.NoResultsPending:
                interrupted = False
                break
            except KeyboardInterrupt:
                interrupted = True
                break
        if interrupted:
            print "\n Interrupted... \n"
            return False
        # Aggregate the results
        rmFiles=[] 
        for i in range(0,len(self.sessions)):
            th = self.sessions[i]
            if th.keyFileList:
                for eachFile in th.keyFileList:
                    rmFiles.append(th.userHost + ':/' + eachFile)

        if rmFiles:
            print "\n *** Files that will be over-written: *** \n"
            for eFile in rmFiles:
                print eFile
            print "\nThe above list of files will be removed, Proceed(Yes/No)",
            resp= sys.stdin.readline().strip()
            if(resp == 'Yes' or resp == 'y'  or resp=='Y' or resp == 'YES' or \
                resp =='yes'):
                return True
            else :
                print 
                return  False
        return True

    def updateKnownHosts(self):
        """
            Update hostKeys across Hosts that are present in the sessions.
        """
        self.getHostList()
        print "Updating host entrys for \n"
        cntr=0
        for eachHost in self.hostList:
            if cntr%5==0:
                print
            print "%s,  " % eachHost,
            cntr+=1
        print "\n\n"

        for i in range(0,len(self.sessions)):
            th = self.sessions[i]
            self.pool.putRequest( threadpool.WorkRequest( \
                th.updateRemoteHostKeys, [self.hostList],None,None))
            #Wait for completion of all in queue, or a keyboard interrupt
        interrupted=False
        while True:
            try:
                self.pool.poll(block=True)
            except threadpool.NoResultsPending:
                interrupted = False
                break
            except KeyboardInterrupt:
                interrupted = True
                break

    def getHostList(self):
        """ Obtain the host list for the list of sessions. 
            eliminate repeated hosts.
        """
        self.hostList=[]
        for i in range(0, len(self.sessions)):
            th=self.sessions[i]
            if th.host in self.hostList:
                pass
            else:
                self.hostList.append(th.host)


