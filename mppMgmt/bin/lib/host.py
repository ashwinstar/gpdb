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
	def __init__(self,hostname,user=None,port=22):
		self.hostname = hostname
		if user != None:
			self.addSession(user,port)
		self.ostype = 'Unknown'

	hostname = None
	filesystems = {}
	cpus = []
	nics = []
	#Dictionary of ssh sessions, keyed on username
	sshsessions = {}
	ostype = None

	#>>> Generic methods that add to information about this host
	def addSession(self,user,port=22):
		"""Add an ssh session, keyed on the username"""
		self.sshsessions[user] = sshSessionModule.sshSession(user,port,self.hostname)

	def addNIC(self,nicname=None):
		if nicname is None:
			nicname = 'eth%d' % (len(self.nics))
		self.nics.append(nicname)
		return

	def addCPU(self,cpuType='Unknown'):
		cpuid = 'cpu%d' % (len(self.cpus))
		self.cpus.append((cpuid,cpuType))
		return

	def addFilesystem(self,FSName,FSType='Unknown',FSPartition='Unknown'):
		fsid = 'fs%d' % (len(self.filesystems))
		if not self.filesystems.has_key(FSName):
			self.filesystems[FSName] = (fsid,FSType,FSPartition)
		else:
			raise DuplicateInfo('Duplicate filesystem information:'+FSName)
		return

	#End Methods that add information about this host <<<

	#>>> Discovery method, what kind of host is at the other end of the host/user/port
	#designed to be called from the class (without instantiation)
	def getHostType(host,user,port=22):
		a = sshSessionModule.sshSession(user,port,host)
		try:
			a.makeReady()
		except sshSessionModule.PasswordFailure, e:
			print '%s:%s' % (e,' for: ' + a.hostname)
			return 'Unknown'

		a.setCMD('ssh',['uname -s'])
		try:
			ostype = a.run()
		except sshSessionModule.NOTREADY, e:
			print '%s:%s' % (e,'ssh session for ' + a.hostname + ' not ready for commands.')
			return 'Unknown'

		return ostype

	#Mechanism used to create a static callable method from within this object
	getHostType = Callable(getHostType)

	#>>> Factory for returning an instance of Host subclassed for a specific OS type
	def getHostObject(host,user,port=22):
		ostype = Host.getHostType(host,user,port)
		#print 'OS type is %s for host %s.' % (ostype,host)
		if ostype == 'Unknown':
			return Host(host,user)
		elif 'Linux' in ostype:
			return LinuxHost(host,user)
		elif 'Darwin' in ostype:
			return OSXHost(host,user)
		elif 'SunOS' in ostype:
			return SolarisHost(host,user)

	#Mechanism used to create a static callable method from within this object
	getHostObject = Callable(getHostObject)

	#>>> Interfaces for methods to gather host information, meant to be overloaded for
	#a given host type

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
			print '%s %s %s' % (self.filesystems[key])

#>>> Implementations of this class for known operating systems
class LinuxHost(Host):
	def __init__(self,hostname,user):
		super(LinuxHost,self).__init__(hostname,user)
		self.ostype='Linux'

class OSXHost(Host):
	def __init__(self,hostname,user):
		super(OSXHost,self).__init__(hostname,user)
		self.ostype='OSX'

class SolarisHost(Host):
	def __init__(self,hostname,user):
		super(SolarisHost,self).__init__(hostname,user)
		self.ostype='Solaris'

if __name__ == '__main__':

	#print 'OS type is %s for host %s.' % (Host.getHostType('local1','demo1'),'local1')

	a = Host.getHostObject('local1','demo1')
	if a is None:
		print 'No object returned for getHostObject call!'
		raise SystemExit, -1

	a.addNIC()
	a.addNIC('myNIC')

	a.addCPU()
	a.addCPU('Opteron')
	a.addCPU('Xeon')

	#a.addSession('demo1')

	try:
		a.addFilesystem('/dbfast1','ext3','/dev/md0')
		a.addFilesystem('/dbfast2','ext2','/dev/md1')
		a.addFilesystem('/dbfast3','reiserfs','/dev/md2')
		a.addFilesystem('/dbfast4','ufs','/dev/md2')
		a.addFilesystem('/dbfast4','ufs','/dev/md2')
	except DuplicateInfo,e:
		print '%s: %s' % (e,'Attempt to add filesystem added with duplicate name.')
		pass

	a.printHostinfo()

	print 'Activating session...'
	keysorted = a.sshsessions.keys()
	keysorted.sort()
	for key in keysorted:
		try:
			a.sshsessions[key].makeReady()
		except sshSessionModule.PasswordFailure, e:
			print '%s:%s' % (e,' for: ' + a.hostname)

		a.sshsessions[key].setCMD('ssh',['ls'])
		try:
			print '%s' % (a.sshsessions[key].run())
		except sshSessionModule.NOTREADY, e:
			print '%s:%s' % (e,'ssh session for ' + a.hostname + ' not ready for commands.')
