#!/usr/bin/env python

"""
host -- stores information about a host

Author: Luke Lonergan <llonergan@greenplum.com>"""

# $Id$

#__all__ = ['']

__author__ = "Luke Lonergan"
__version__ = "1.0"
__date__ = "2005-09-26"

try:
    import sshSessionModule
except:
    print 'Unable to import the sshSession module'
    raise SystemExit, 1

import string,re

class ExceptionHost(Exception):
        """Base class for all exceptions raised by this module."""
        def __init__(self, value):
                        self.value = value
        def __str__(self):
                        return str(self.value)
class DuplicateInfo(ExceptionHost):
    """Raised when duplicate info is entered into a resource list"""

class Callable:
    """Wrapper class to allow for calling "static" methods from within a python class"""
    def __init__(self, anycallable):
        self.__call__ = anycallable

class Host(object):
    def __init__(self,hostname,user=None,port=22,session=None):
        self.hostname = hostname
        if user != None:
            self.addSession(user,port,session)
        self.ostype = 'Unknown'

    hashcommentpattern = re.compile('.*(?=#)|.*(?!#)')
    slashbeginspattern = re.compile('^\/.*')

    #hostname = None
    filesystems = None
    cpus = None
    nics = None
    #Dictionary of ssh sessions, keyed on username
    sshsessions = None
    ostype = None
    totalRam = 0

    #>>> Generic methods that add to information about this host
    def addSession(self,user,port=22,session=None):
        """Add an ssh session, keyed on the username"""
        if not self.sshsessions:
            self.sshsessions={}
        if session is None:
            self.sshsessions[user] = sshSessionModule.sshSession(user,port,self.hostname)
        else:
            self.sshsessions[user] = session

    def addNIC(self,nicname=None):
        if not self.nics:
            self.nics=[]
        if nicname is None:
            nicname = 'eth%d' % (len(self.nics))
        self.nics.append(nicname)
        return

    def addCPU(self,cpuType='Unknown'):
        if not self.cpus:
            self.cpus=[]
        cpuid = 'cpu%d' % (len(self.cpus))
        self.cpus.append((cpuid,cpuType))
        return

    def addFilesystem(self,FSName,FSType='Unknown',FSPartition='Unknown'):
        if not self.filesystems:
            self.filesystems={}
        if not self.filesystems.has_key(FSName):
            self.filesystems[FSName] = (FSType,FSPartition)
        else:
            raise DuplicateInfo('Duplicate filesystem information:'+FSName)
        return

    #End Methods that add information about this host <<<

    #>>> Discovery method, what kind of host is at the other end of the host/user/port
    #designed to be called from the class (without instantiation)
    def getHostType(host,user,port=22, sshSession=None):
        if not sshSession:
            a = sshSessionModule.sshSession(user,port,host)
            try:
                a.makeReady()
            except sshSessionModule.PasswordFailure, e:
                print '%s:%s' % (e,' for: ' + a.hostname)
                return 'Unknown'
        else :
            a= sshSession
        a.setCMD('ssh',['uname -s'])
        try:
            ostype = a.run()
        except sshSessionModule.NOTREADY, e:
            print '%s:%s' % (e,'ssh session for ' + a.hostname + ' not ready for commands.')
            return 'Unknown'
        return (a,ostype)

    #Mechanism used to create a static callable method from within this object
    getHostType = Callable(getHostType)

    #>>> Factory for returning an instance of Host subclassed for a specific OS type
    def getHostObject(host,user,port=22, sshSession=None):
        (session,ostype) = Host.getHostType(host,user,port, sshSession)
        #print 'OS type is %s for host %s.' % (ostype,host)
        if ostype.find('Unknown')!=-1:
            return Host(host,user,port,session)
        elif ostype.find('Linux')!=-1:
            return LinuxHost(host,user,port,session)
        elif ostype.find('Darwin')!=-1:
            return OSXHost(host,user,port,session)
        elif ostype.find('SunOS')!=-1:
            return SolarisHost(host,user,port,session)

    #Mechanism used to create a static callable method from within this object
    getHostObject = Callable(getHostObject)

    def printHostinfo(self):
        print 'Hostname:\t%s' % self.hostname

        for i in range(len(self.nics)):
            nic = self.nics[i]
            print 'NIC#%d:\t\t%s' % (i+1,nic)
        for i in range(len(self.cpus)):
            cpu = self.cpus[i]
            print 'CPU#%d:\t\t%s %s' % (i+1,cpu[0],cpu[1])

        keysorted = self.filesystems.keys()
        keysorted.sort()
        for i in range(len(keysorted)):
            key = keysorted[i]
            print 'FS#%d:\t\t' % (i+1),
            print '%s %s %s' % (key,self.filesystems[key][0],self.filesystems[key][1])
        print "Total RAM: %d" % self.totalRam

    def runSSH(self,command,session=None):
        """Run an ssh command on the first session (unless one passed in)"""

        if not session:
            keys = self.sshsessions.keys()
            session = self.sshsessions[keys[0]]
        if not session:
            return 'Unknown'

        session.setCMD('ssh',[command])
        try:
            result = session.run()
        except sshSessionModule.NOTREADY, e:
            print '%s:%s' % (e,'ssh session for ' + session.hostname + ' not ready for commands.')
            return 'Unknown'
        return result

    def stripComment(self,line):
        match = self.hashcommentpattern.match(line)
        return match.group()

#>>> Implementations of this class for known operating systems
class LinuxHost(Host):
    def __init__(self, *args, **kwds):
        super(LinuxHost,self).__init__(*args, **kwds)
        self.ostype='Linux'
        self.discoverFilesystems()
        self.discoverCPUs()
        self.discoverNICs()
        try:
            self.discoverTotalRam()
        except Exception, e:
            print "Unable to determine RAM, Received exception %s mesg: %s" % \
                (str(e.__class__), str(e))
            self.totalRam=0

    def discoverNICs(self):
        result = self.runSSH('cat /proc/net/dev')
        for line in result.split('\n'):
            if ':' in line:
                nicname = line.split(':')[0]
                if not (nicname.find('sit')!=-1 or nicname.find('lo')!=-1):
                #if not ('sit' in nicname or 'lo' in nicname): #2.3 python allows this
                    self.addNIC(nicname.strip())

    def discoverCPUs(self):
        result = self.runSSH('cat /proc/cpuinfo')
        for line in result.split('\n'):
            if line.find('model name')!=-1:
            #if 'model name' in line: #2.3 python allows this
                modelname = line.split(':')[1]
                self.addCPU(modelname.strip())

    def discoverFilesystems(self):
        result = self.runSSH('cat /etc/mtab')
        for line in result.split('\n'):
            #print '%s' % self.stripComment(line)
            if self.stripComment(line):
                # partition,fsname,fstype,options,fsck1,fsck2
                items = line.split(' ')
                #only allow filesystems with a leading slash in their partition
                match = self.slashbeginspattern.match(items[0])
                if match:
                    try:
                        self.addFilesystem(items[0],items[1],items[2])
                    except DuplicateInfo:
                        pass
    def discoverTotalRam(self):
        """ Discover the RAM Size"""
        result= self.runSSH("cat /proc/meminfo  | grep MemTotal ")
        memTotalLine=result.strip()
        memList=memTotalLine.split(' ')
        memVal = memList[len(memList)-2]
        memFactor= memList[len(memList)-1]
        if memFactor == 'kB':
            self.totalRam=int(memVal) * 1024
            




class OSXHost(Host):
    def __init__(self, *args, **kwds):
        super(OSXHost,self).__init__(*args, **kwds)
        self.ostype='OSX'
        self.discoverFilesystems()
        self.discoverCPUs()
        self.discoverNICs()
        try:
            self.discoverTotalRam()
        except Exception, e:
            print "Unable to determine RAM, Received exception %s mesg: %s" % \
                (str(e.__class__), str(e))
            self.totalRam=0

    def discoverNICs(self):
        result = self.runSSH('ifconfig -lu')
        result.strip()
        for nicname in result.split(' '):
            if not (nicname.find('lo')!=-1 or \
                nicname.find('gif')!=-1 or \
                nicname.find('stf')!=-1 or \
                nicname.find('fw')!=-1):
                self.addNIC(nicname.strip())

    def discoverCPUs(self):
        result = self.runSSH('sysctl hw.ncpu')
        result.strip()
        ncpus = result.split(':')
        ncpus = int(ncpus[1])
        for i in range(ncpus):
            self.addCPU('UnknownAppleCPU')

    def discoverFilesystems(self):
        result = self.runSSH('mount')
        for line in result.split('\n'):
            #print '%s' % self.stripComment(line)
            if self.stripComment(line):
                # partition,fsname,fstype,options,fsck1
                items = line.split(' ')
                #only allow filesystems with a leading slash in their partition
                match = self.slashbeginspattern.match(items[0])
                if match:
                    try:
                        self.addFilesystem(items[0],items[2],'UnknownType')
                    except DuplicateInfo:
                        pass

    def discoverTotalRam(self):
        """ Discover the RAM Size"""
        memTotalLine= self.runSSH("sysctl hw.physmem")
        memTotalLine.strip()
        memList=memTotalLine.split(' ')
        self.totalRam = int(memList[1])



class SolarisHost(Host):
    def __init__(self, *args, **kwds):
        super(SolarisHost,self).__init__(*args, **kwds)
        self.ostype='Solaris'
        self.discoverFilesystems()
        self.discoverCPUs()
        self.discoverNICs()
        try:
            self.discoverTotalRam()
        except Exception, e:
            print "Unable to determine RAM, Received exception %s mesg: %s" % \
                (str(e.__class__), str(e))
            self.totalRam=0

    def discoverNICs(self):
        result = self.runSSH('netstat -i | tail +2')
        for line in result.split('\n'):
            if len(line) > 10:
                nicname = line.split(' ')[0]
                if not (nicname.find('lo')!=-1):
                    self.addNIC(nicname.strip())

    def discoverCPUs(self):
        modelname = self.runSSH('optisa `isalist`')
        result = self.runSSH('prtconf -D | grep cpu')
        for line in result.split('\n'):
            if line.find('cpu, instance')!=-1:
                self.addCPU(modelname.strip())

    def discoverFilesystems(self):
        result = self.runSSH('cat /etc/mnttab')
        for line in result.split('\n'):
            #print '%s' % self.stripComment(line)
            if self.stripComment(line):
                # partition,fsname,fstype,options,fsck1
                items = line.split('\t')
                #only allow filesystems with a leading slash in their partition
                match = self.slashbeginspattern.match(items[0])
                if match and not \
                (items[2].find('lofs')!=-1 or \
                 items[2].find('devfs')!=-1 or \
                 items[2].find('udfs')!=-1 or \
                 items[2].find('pcfs')!=-1):
                    try:
                        self.addFilesystem(items[0],items[1],items[2])
                    except DuplicateInfo:
                        pass
        
    def discoverTotalRam(self):
        """ Discover the RAM Size"""
        memTotalLine= self.runSSH("prtconf | awk '/^Memory/{print}'")
        memTotalLine.strip()
        memList=memTotalLine.split(' ')
        memVal = memList[2]
        memFactor= memList[3]
        print memVal
        print memFactor
        if 'Megabytes' in memFactor:
            self.totalRam = int(memVal) * 1024 * 1024

        


if __name__ == '__main__':
    import sys,os

    if len(sys.argv) < 2:
        print 'usage: %s [username@]host[:port]' % (sys.argv[0])
        raise SystemExit, -1

    host = sys.argv[1]

    defaultport = 22

    #print 'OS type is %s for host %s.' % (Host.getHostType('local1','demo1'),'local1')

    if '@' in host:
        (user, host) = string.split(host, '@')
    else:
        user = os.environ.get('LOGNAME') or os.environ.get('USER')

    if ':' in host:
        (host,port) = string.split(host, ':')
    else:
        port = defaultport

    a = Host.getHostObject(host,user,port)
    #a = Host.getHostObject('buzzard1','root',22)
    #b = Host.getHostObject('buzzard2','root',22)
    if not a:
        print 'No object returned for getHostObject call!'
        raise SystemExit, -1

    print 'OS Type = %s' % (a.ostype)

    a.printHostinfo()
    #b.printHostinfo()

    print 'Testing session...',
    if a.runSSH('ls -d .').strip() == '.':
        print 'session is ok.'
    else:
        print 'session is not ok.'
