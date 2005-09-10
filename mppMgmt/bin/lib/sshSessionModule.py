#! /usr/bin/env python
# sshSession -- ssh access a hosts, implementing password memory
"""

From the early implementation by Eric S. Raymond <esr@thyrsus.com>
Author: Luke Lonergan <llonergan@greenplum.com>"""

import os, sys, getopt, string, getpass, lib.pexpect, threading

class sshSession(threading.Thread):
    "Session with extra state including the password to be used."
    def __init__(self, user, port, host, password=None, verbose=0):
        threading.Thread.__init__(self)        

	self.user = user
	self.port = port
	self.host = host

	self.verbose = verbose

	#Output of last command executed
	self.lastoutput = None

	self.cmd = None
	self.args = []
	self.timeout = 30

	self.password = password
	self.PWCheck = 0
	self.PWRequired = None

	self.hostKeyPhrase = None
	self.PPRequired = None

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
	child = lib.pexpect.spawn(command,[],self.timeout)
	if self.verbose:
		sys.stderr.write("-> " + command + "\n")

	try:
	  pwpass=0
	  while True:
	    seen=child.expect([lib.pexpect.EOF,'assword:','continue connecting (yes/no)?','Enter passphrase for key.*:'])

	    if self.verbose:
		    sys.stderr.write("<- " + child.before + "\n")

	    #The command ran successfully if something other than the above was seen
	    if seen == 0:
		    #Exit the while loop, we've run the command
		    break

	    #Saw the password prompt, need to enter a password or ask for one, then enter it
	    elif seen == 1:
		if self.verbose:
		        sys.stderr.write("Saw password prompt...\n")
		#Add one to the password prompt count
		pwpass = pwpass + 1

	    #This is a password check and we don't have a trial password
		if self.PWCheck == 1 and not self.password:
			#Mark that a password is required for this connection
			self.PWRequired = 1
			break

		#If we have a password and we're on the first pass, try it
		if pwpass == 1:
		  if self.password:
			child.sendline(self.password)

		  elif not self.password:
			self.password = getpass.getpass('Remote password: ')
		    	child.sendline(self.password)

		elif pwpass > 1:
				if self.PWCheck == 1:
					#Mark that a password is required for this connection
					self.PWRequired = 1
					#We've tried a trial password and we're on a password check, break
					break
				#We are here because the first password try failed, bomb out
				print 'Too many password attempts for host ' + self.host + ', exiting.'
				raise SystemExit, 1

	    #This host isn't in the known_hosts, enter 'yes' to continue...
	    elif seen == 2:
		if self.verbose:
			sys.stderr.write("Saw 'continue connecting' prompt...\n")
		child.sendline("yes")

	    #A password protected local public key is being exported by ssh-agent
	    #Ask for the key passphrase if necessary, then output it to continue
	    elif seen == 3:
		if self.verbose:
		        sys.stderr.write("Saw ssh-agent pass key prompt...\n")
		self.PPRequired = 1
		if self.PWCheck is 1:
			break
		if not self.hostKeyPhrase:
			self.hostKeyPhrase = getpass.getpass('ssh-agent pass key phrase: ')
		child.sendline(self.hostKeyPhrase)

	except:
		print "Error processing login: " + sys.exc_info()[0]
		raise SystemExit, 1


	#If this was a password check, reset the flag
	if self.PWCheck is 1:
		if child.isalive():
			child.kill(9)
		self.PWCheck = 0

    	return child.before

    def ssh(self, command):
	    return self.__exec("ssh -p %s -l %s %s \"%s\""%(self.port, self.user,self.host,command))

    def scp(self, src, dst):
	    return self.__exec("scp -P %s %s %s@%s:%s" % (self.port, src, self.user, self.host, dst))

    def scprem(self, src, dst):
	    return self.__exec("scp -P %s %s@%s:%s %s" % (self.port, self.user, self.host, src, dst))

    def exists(self, file):
	"Retrieve file permissions of specified remote file."
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

