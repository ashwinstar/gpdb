#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
"""
Set of Classes for executing unix commands.


"""
import os
import platform
import socket
import sys

from gppylib.gplog import *
from gppylib.commands.base import *


logger = gplog.get_default_logger()


SYSTEM="unknown"

SUNOS="sunos"
LINUX="linux"
DARWIN="darwin"
FREEBSD="freebsd"


class CommandNotFoundException(Exception):
    def __init__(self,cmd,paths):
        self.cmd=cmd
        self.paths=paths
    def __str__(self):
        return "Could not locate command: '%' in this set of paths: %s" % (self.cmd,repr(self.paths))
    
    

#TODO: worth caching in a dict?
def findCmdInPath(cmd):
    CMDPATH = ('/usr/kerberos/bin', '/usr/sfw/bin', '/opt/sfw/bin', '/usr/local/bin', '/bin', 
               '/usr/bin', '/sbin', '/usr/sbin', '/usr/ucb', '/sw/bin')

    for p in CMDPATH:
        f = os.path.join(p, cmd)
        if os.path.exists(f):
            return f
    logger.critical('Command %s not found' % cmd)
    raise CommandNotFoundException(cmd,CMDPATH)

def setup():
    global SYSTEM
    plat = platform.uname()
    self.SYSTEM = plat[0].lower()    
    
def getLocalHostname():
    return socket.gethostname().split('.')[0]

def getUserName():
    return os.environ.get('LOGNAME') or os.environ.get('USER')



      
        
#---------------ping--------------------
class Ping(Command):
    def __init__(self,name,hostToPing,ctxt=LOCAL,remoteHost=None):        
        self.hostToPing=hostToPing
        cmdStr = "%s -c 1 %s" % (findCmdInPath('ping'),hostToPing)        
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod    
    def ping_list(host_list):        
        for host in host_list:
            yield Ping("ping",host,ctxt=LOCAL,remoteHost=None)
    
    @staticmethod
    def local(name,hostToPing):        
        p=Ping(name,hostToPing)
        p.run(validateAfter=True)
    
    @staticmethod
    def remote(name,hostToPing,hostToPingFrom):
        p=Ping(name,hostToPing,ctxt=REMOTE,remoteHost=hostToPingFrom)
        p.run(validateAfter=True)    

        
#---------------du--------------------
class DiskUsage(Command):    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):        
        self.directory=directory
        cmdStr="ls -l -R %s | %s '{t+=\$5;} END{print t}'" % (directory,findCmdInPath('awk'))
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def get_size(name,remote_host,directory):
        duCmd=DiskUsage(name,directory,ctxt=REMOTE,remoteHost=remote_host)
        duCmd.run(validateAfter=True)
        return duCmd.get_bytes_used()
            
                        
    def get_bytes_used(self):   

        rawIn=self.results.stdout.split('\t')[0]

        if rawIn[0] == 'd':
            raise UnixOperationError, "du command could not find directory: cmd: %s"\
                  "resulted in stdout: '%s' stderr: '%s'" % (self.cmdStr, self.results.stdout, self.results.stderr)
        else:
            dirBytes=int(rawIn)
            return dirBytes
    

#-------------df----------------------
class DiskFree(Command):
    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):        
        self.directory=directory
        cmdStr="%s %s" % (findCmdInPath('df'),directory) 
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def get_size(name,remote_host,directory):
        dfCmd=DiskFree(name,directory,ctxt=REMOTE,remoteHost=remote_host)
        dfCmd.run(validateAfter=True)
        return dfCmd.get_bytes_free()
    
       
    
    def get_bytes_free(self):
        '''expected output of the form:        
           Filesystem   512-blocks      Used Available Capacity  Mounted on
           /dev/disk0s2  194699744 158681544  35506200    82%    /
        '''
        rawIn=self.results.stdout.split('\n')[1]
        bytesFree=int(rawIn.split()[3])
        return bytesFree

   

#-------------mkdir------------------
class MakeDirectory(Command):
    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):        
        self.directory=directory
        cmdStr="%s -p %s" % (findCmdInPath('mkdir'),directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,directory):    
        mkdirCmd=MakeDirectory(name,directory)
        mkdirCmd.run(validateAfter=True)
            
    @staticmethod
    def remote(name,remote_host,directory):
        mkdirCmd=MakeDirectory(name,directory,ctxt=REMOTE,remoteHost=remote_host)
        mkdirCmd.run(validateAfter=True)
        
        
#-------------rmdir------------------
class RemoveDirectory(Command):
    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):
        self.directory=directory
        cmdStr="%s %s" % (findCmdInPath('rmdir'),directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name,directory):
        rmCmd=RemoveDirectory(name,directory)
        rmCmd.run(validateAfter=True) 
        
    
    @staticmethod
    def remote(name,remote_host,directory):
        rmCmd=RemoveDirectory(name,directory,ctxt=REMOTE,remoteHost=remote_host)
        rmCmd.run(validateAfter=True) 
        

    
#-------------rm -rf ------------------
class RemoveFiles(Command):
    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):        
        self.directory=directory
        cmdStr="%s -rf %s" % (findCmdInPath('rm'),directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def remote(name,remote_host,directory):
        rmCmd=RemoveFiles(name,directory,ctxt=REMOTE,remoteHost=remote_host)
        rmCmd.run(validateAfter=True) 
    

#-------------scp------------------
class RemoteCopy(Command):
    def __init__(self,name,srcDirectory,dstHost,dstDirectory,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory=srcDirectory
        self.dstHost=dstHost
        self.dstDirectory=dstDirectory        
        cmdStr="%s -r %s %s:%s" % (findCmdInPath('scp'),srcDirectory,dstHost,dstDirectory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

#------------ ssh + tar ------------------
#TODO:  impl this.
#tar czf - srcDir/ | ssh user@dstHost tar xzf - -C dstDir

   
#-------------create tar------------------
class CreateTar(Command):
    def __init__(self,name,srcDirectory,dstTarFile,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory=srcDirectory
        self.dstTarFile=dstTarFile        
        cmdStr="%s cvPf %s -C %s  ." % (findCmdInPath('tar'), self.dstTarFile,srcDirectory)
        logger.info("CreateTar cmd: %s" % cmdStr)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    
        
#-------------extract tar---------------------
class ExtractTar(Command):
    def __init__(self,name,srcTarFile,dstDirectory,ctxt=LOCAL,remoteHost=None):        
        self.srcTarFile=srcTarFile
        self.dstDirectory=dstDirectory
        tarCmd = None
        try:
            tarCmd = findCmdInPath('gtar')
        except CommandNotFoundException:
            tarCmd = findCmdInPath('tar')        
        cmdStr="%s -C %s -xf %s" % (tarCmd, dstDirectory, srcTarFile)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

        

#--------------kill ----------------------
class Kill(Command):
    def __init__(self,name,pid,signal,ctxt=LOCAL,remoteHost=None):
        self.pid=pid
        self.signal=signal
        cmdStr="%s -s %s %s" % (findCmdInPath('kill'), signal, pid)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name,pid,signal):
        cmd=Kill(name,pid,signal)
        cmd.run(validateAfter=True)
            
    @staticmethod
    def remote(name,pid,signal,remote_host):
        cmd=Kill(name,pid,signal,ctxt=REMOTE,remoteHost=remote_host)        
        cmd.run(validateAfter=True) 
     

#--------------hostname ----------------------
class Hostname(Command):
    def __init__(self, name, ctxt=LOCAL, remoteHost=None):
        Command.__init__(self, name, findCmdInPath('hostname'), ctxt, remoteHost)

    def get_hostname(self):
        if not self.results:
            raise Exception, 'Command not yet executed'
        return self.results.stdout.strip()

#--------------hostname ----------------------
class Chmod(Command):
    def __init__(self, name, dir, perm, ctxt=LOCAL, remoteHost=None):
        cmdStr = '%s %s %s' % (findCmdInPath('chmod'), perm, dir)
        Command.__init__(self, name, cmdStr, ctxt, remoteHost)