#! /usr/bin/env python
"""
sshSession -- access a host using ssh, implementing password memory
Author: Luke Lonergan <llonergan@greenplum.com>"""

# $Id$

__all__ = ['run', 'ssh', 'scp', 'scprem', 'exists', 'setCMD', 'killCMD',
'setupKeyAccessLocal', 'setupKeyAccessRemote', 'isReady', 'isError', 'makeReady']

__author__ = "Luke Lonergan"
__version__ = "1.0"
__date__ = "2005-09-26"

import os, sys, getopt, string, getpass, threading, signal, tempfile
try:
	import lib.pexpect as pexpect
except ImportError:
	try:
		import pexpect
	except ImportError:
		raise
	pass


# Exception classes used by this module.
class ExceptionSshSession(Exception):
	"""Base class for all exceptions raised by this module."""
	def __init__(self, value):
		self.value = value
	def __str__(self):
		return str(self.value)
	def get_trace(self):
		"""This returns an abbreviated stack trace with lines that only concern the caller.
		In other words, the stack trace inside the Pexpect module is not included.
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

class sshSession(threading.Thread):
    "Session with extra state including the password to be used."
    def __init__(self, user, port, host, password=None, verbose=False):
        threading.Thread.__init__(self)        

	self.lock = threading.RLock()

	self.verbose = False

	self.user = user
	self.port = port
	self.host = host

	self.verbose = verbose

	#Output of last command executed
	self.lastoutput = None

	self.cmd = None
	self.args = []
	self.timeout = 30

	self.PWCheck = False
	self.PWRequired = False
	self.password = password

	self.PPRequired = False
	self.hostKeyPhrase = None

	self.child = None

	#Flag that indicates if this session object is able to execute commands
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

    def isReady(self):
	return self.ready

    def isError(self):
	return self.error

    def __setError(self,error=True):
	self.error=error

    def run(self):
	if self.cmd is "ssh":
		self.lastoutput = self.ssh(self.args[0])

	elif self.cmd is "scp":
		self.lastoutput = self.scp(self.args[0], self.args[1])

	elif self.cmd is "scprem":
		self.lastoutput = self.scprem(self.args[0], self.args[1])

	elif self.cmd is "exists":
		self.lastoutput = self.exists(self.args[0])

	else:
		self.lastoutput = "No interface selected"
		print "Host: " + self.host + ": No interface selected"

	return self.lastoutput

    def __exec(self, command):
	"Execute a command on the remote host.  Return the output."

	self.child = pexpect.spawn(command,[],self.timeout)
	self.child.setmaxread (1000) # Sets buffer to 1000 characters.
	if self.verbose:
		sys.stderr.write("-> " + command + "\n")

	try:
	  pwpass=0
	  pppass=0
	  while True:
	    seen=self.child.expect([pexpect.EOF,'assword:','continue connecting (yes/no)?','Enter passphrase for key.*:'])

	    if self.verbose:
		    sys.stderr.write("<- " + self.child.before + "\n")

	    #The command ran successfully if EOF was seen
	    if seen == 0:
		    self.__setError(False)
		    self.ready=True
		    #Exit the while loop, we've run the command
		    break

	    #Saw the password prompt, need to enter a password or ask for one, then enter it
	    elif seen == 1:
		if self.verbose:
		        sys.stderr.write("Saw password prompt...\n")
		#Mark that a password is required for this connection
		self.PWRequired = True
		#Add one to the password prompt count
		pwpass = pwpass + 1

		#If we have a password and we're on the first pass, try it
		if pwpass == 1:
			if self.password:
				self.child.sendline(self.password)
			else:
				if self.PWCheck:
					break
				self.password = getpass.getpass(\
						'sshSession: Remote password for host ' + self.host + ':')
		    		self.child.sendline(self.password)

		elif pwpass > 1:
				if self.PWCheck:
					#We've tried a trial password and we're on a password check, break
					break
				#We are here because the first password try failed, bomb out
				print 'Too many password attempts for host ' + self.host + ', exiting.'
				self.__die('Too many password attempts for host ' + self.host + ', exiting.')

	    #This host isn't in the known_hosts, enter 'yes' to continue...
	    elif seen == 2:
		if self.verbose:
			sys.stderr.write("Saw 'continue connecting' prompt...\n")
		self.child.sendline("yes")

	    #A password protected local public key is being exported by ssh-agent
	    #Ask for the key passphrase if necessary, then output it to continue
	    elif seen == 3:
		if self.verbose:
		        sys.stderr.write("Saw ssh-agent pass key prompt...\n")
		#Mark that a passphrase is required for this connection
		self.PPRequired = True
		#Add one to the passphrase prompt count
		pppass = pppass + 1

		#If we have a passphrase and we're on the first pass, try it
		if pppass == 1:

			if self.hostKeyPhrase:
				self.child.sendline(self.hostKeyPhrase)
			else:
				if self.PWCheck:
					break
				self.hostKeyPhrase = getpass.getpass(\
						'sshSession: ssh-agent pass key phrase for host ' + self.host + ':')
				self.child.sendline(self.hostKeyPhrase)

		elif pppass > 1:
				if self.PWCheck:
					#We've tried a trial password and we're on a password check, break
					break
				#We are here because the first password try failed, bomb out
				print 'Too many passphrase attempts for host ' + self.host + ', exiting.'
				self.__die('Too many passphrase attempts for host ' + self.host + ', exiting.')

	except:
		#print "Error processing login!"
		raise
		#raise SystemExit, 1


	#If this was a password check, reset the flag
	if self.PWCheck:
		self.killCMD(9)
		self.PWCheck = False

    	return self.child.before

    def ssh(self, command):
	if not self.PWCheck:
		self.__readyOrDie()
	return self.__exec("ssh -n -p %s -l %s %s \"%s\""%(self.port, self.user,self.host,command))

    def scp(self, src, dst):
	self.__readyOrDie()
	return self.__exec("scp -P %s %s %s@%s:%s" % (self.port, src, self.user, self.host, dst))

    def scprem(self, src, dst):
	self.__readyOrDie()
	return self.__exec("scp -P %s %s@%s:%s %s" % (self.port, self.user, self.host, src, dst))

    def exists(self, file):
	"Retrieve file permissions of specified remote file."

	self.__readyOrDie()

	seen = self.ssh("/bin/ls -ld %s" % file)
	if  self.verbose:
		sys.stderr.write("Existence check on %s gave %s\n" % (file,`seen`))
	if string.find(seen, "No such file") > -1:
		return None			# File doesn't exist
	else:
		return seen.split()[0]	# Return permission field of listing.

    def setCMD(self,cmd,args=[]):
		self.cmd=cmd
		self.args=args

    #Public interface to signal the child with signal sig (default signal is 15, or TERM)
    def killCMD(self,sig=signal.SIGTERM):
	if self.child.isalive():
		self.child.kill(sig)

    def __fixperms(self,file):
		"Force perms of remote file to user read on, group/other write off."
		perms = self.exists(file)
		if perms is None:
			print "File " + file + " doesn't exist."
			return None
		else:
			ok = (perms[1] == 'r' and perms[4] == '-' and perms[5] == '-' and perms[6] == '-' and \
		      	perms[7] == '-' and perms[8] == '-')
			if not ok:
	    			self.ssh("/bin/chmod 700 %s" % file)
		return ok
	
    def __forcefile(self,file, command):
    		"Create file with command if it does not already exist."
    		perms = self.exists(file)
    		if perms is None:
	   		self.ssh(command)
	   		perms = self.exists(file)
	
    		return perms

    def setupKeyAccessLocal(self):
    		# Verify that key pairs are present on the local side 
    		try:
	    		os.chdir(os.getenv("HOME"))
    		except:
	    		self.__die("Can't find your home directory!")
	
    		if not os.access(".", os.R_OK):
	    		self.__die("Your home directory is not readable by you. This probably means $HOME is incorrect.")
	
    		if not os.path.exists(".ssh"):
	    		os.mkdir(".ssh")

    		if not os.access(".ssh", os.R_OK):
	    		self.__die("Your .ssh directory is not readable by you. This probably means $HOME is incorrect.")

    		try:
	    		os.chdir(".ssh")
    		except:
	    		self.__die("Can't chdir into your .ssh directory!")

    		public_keys  = filter(lambda f: os.access(f, os.R_OK), ("id_dsa.pub", "id_rsa.pub", "identity.pub"))

    		if not public_keys:
			public_keys = []
			for (keytype, file) in (("rsa1", "identity"), ("dsa", "id_dsa")):
	    			print "About to generate a %s key. You will be prompted for a passphrase," % keytype
	    			print "Pressing enter at that prompt will create a key with no passphrase" 
	    			os.system("ssh-keygen -t %s -f %s" % (keytype, file))
	    			public_keys.append(file + ".pub")

    def setupKeyAccessRemote(self):
    		try:
			#This is actually a no-no with threads, they all share a FS node in Python
			self.lock.acquire()
			dir = os.getenv("HOME") + '/.ssh'
	    		os.chdir(dir)
			self.lock.release()
    		except:
	    		self.__die("Can't chdir to your ssh directory!")
	

    		public_keys  = filter(lambda f: os.access(f, os.R_OK), ("id_dsa.pub", "id_rsa.pub", "identity.pub"))

    		#print "I see the following public keys:", string.join(public_keys,", ")
    		for file in public_keys:
			private_key = file[:-4]
			legend = private_key + " corresponding to " + file + " " 
			if not os.access(private_key, os.R_OK):
				self.__die(legend + "does not exist or is unreadable.")

		# Update remote authorized_keys files
    		try:
	 		keyfile        = tempfile.mktemp() + self.host
	   		keyfile_remote = tempfile.mktemp() + self.host
	   		keyfile_merged = tempfile.mktemp() + self.host

	 		os.system("cat " + string.join(public_keys, " ") + " >" + keyfile)
	 		if self.verbose:
		 		print "Public keys consolidated in %s" % keyfile

	 		# Permissions checks
	 		self.__fixperms(".")
	 		self.__forcefile(".ssh", "mkdir .ssh")
	 		self.__fixperms(".ssh")
	
	 		# OK, install keys remotely if they don't exist
	 		if not self.exists(".ssh/authorized_keys"):
		 		self.scp(keyfile, ".ssh/authorized_keys")
	 		else:
	   			self.scprem(".ssh/authorized_keys",keyfile_remote)
	   			os.system("cat " + keyfile + " " + keyfile_remote + " >" + keyfile_merged)
	   			self.scp(keyfile_merged, ".ssh/authorized_keys")

	   		# Cleanup temp files
	   		try:
      	        		os.remove(keyfile)
      	        		os.remove(keyfile_remote)
      	        		os.remove(keyfile_merged)
	   		except:
				pass

	 		self.__fixperms(".ssh/authorized_keys")

    		except (SystemExit):
			pass

    		except (NOTREADY):
	    		raise

    		except:
	    		print "Unexpected error:", sys.exc_info()[0]
	    		raise

    		else:
	    		print "Keys for %s on %s have been installed." % (self.user, self.host)

    def initializeSession(self,defaultPassword=None,defaultPassPhrase=None):
	"""Initialize the session for checking of passwords and setup of keys"""

	if self.verbose:
		print 'LAL in initializeSession host: %s' % (th.host)

	#Set the password check flags
	self.PWCheck = True
	self.PWRequired = 0
	self.timeout=None
	if defaultPassword is not None:
		self.password = defaultPassword
	if defaultPassPhrase is not None:
		self.hostKeyPhrase = defaultPassPhrase
	
    def makeReady(self,setupKeys=True):
    	"""Prepares session to run commands by gathering passwords, keys, etc"""

	#Set the default state of all of the sessions
	self.initializeSession()

	defaultPassword=None
	th = self
	stopAfter=3
	#Password is required for this session
	while stopAfter != 0 and not th.isReady():
		stopAfter -= 1
		if self.verbose:
			print 'LAL in setupHostAccess host: %s' % (th.host)

		#Initialize the remaining sessions to use the
		#new default password and hostKeyPhrase
		self.initializeSession(th.password,th.hostKeyPhrase)
		#Run a parallel command to discover whether each host needs a PW or not
		self.ssh(['ls -ld '])

		if not th.isReady():
			if th.PWRequired:
				prompt = 'sshSession: Remote password for host ' + th.host + ':'
				th.password = getpass.getpass(prompt)

			if th.PPRequired:
				prompt = 'sshSession: ssh-agent pass key phrase:'
				th.hostKeyPhrase = getpass.getpass(prompt)
 
		else:
			break

	if stopAfter == 0 and not th.isReady():
		raise PasswordFailure('sshSession: Failed to get password')

	#Setup the ssh host keys if any session required a password
	if setupKeys and self.PWRequired:
		#Copy the first session's passphrase to all
		prompt = 'sshSession: ssh-agent pass key phrase:'
		self.hostKeyPhrase = getpass.getpass(prompt)

		#Setup/locate the local keys
  		self.setupKeyAccessLocal()

		#Setup remote ssh host keys
		self.setupKeyAccessRemote()

	return 'sshSession: session is ready to execute commands'

if __name__ == '__main__': 
	a = sshSession('demo1',22,'local1')
	a.makeReady()
	a.setCMD('ssh',['ls'])
        try:
		print '%s' % (a.run())
	except NOTREADY, e:
		print '%s:%s' % (e,'ssh session for ' + a.hostname + ' not ready for commands.')

