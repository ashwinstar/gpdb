#!/usr/bin/env python

import fcntl
import os
import select
import subprocess

bootstrapSource="""
import sys
exec(sys.stdin.read(int(sys.stdin.readline())))
"""

pysyncSource="""

import cPickle
import socket
import sys
import time

class RemotePysync:

    def doCommand(self,*args):
	a = cPickle.dumps(args)
	sys.stdout.write('%d\\n%s'%(len(a),a))
	sys.stdout.flush()
	size = int(sys.stdin.readline())
	data = sys.stdin.read(size)
	assert(size==len(data))
	return cPickle.loads(data)

    def run(self):
	sockname = self.doCommand('getsockname')
	list = self.doCommand('getlist')
	sys.stderr.write('list="%s" sockname=%s\\n'%(list,str(sockname)))
	sys.stderr.flush()
	time.sleep(1)
	self.doCommand('quit',0)

if 'pysyncSource' not in globals():
    RemotePysync().run()
"""

exec(pysyncSource)

class LocalPysync:

    def __init__(self,argv):
	if len(argv)!=3:
	    self.usage(argv)
	self.sourcedir = argv[1]
	self.destdir = argv[2]
	i = self.destdir.find(':')
	if i==-1:
	    self.usage(argv)
	self.userAndHost = self.destdir[:i]
	self.destdir = self.destdir[i+1:]
	self.socket = socket.socket()
	self.socket.bind((socket.gethostname(),0))

    def usage(self,argv):
	sys.stderr.write("usage:\n")
	sys.stderr.write("    python "+argv[0]+" sourcedir [user@]host:destdir\n")
	sys.exit(1)

    def doCommand(self,what):
	if what[0]=='getsockname':
	    return self.socket.getsockname()
	if what[0]=='getlist':
	    return "file%name\r\n "
	if what[0]=='quit':
	    sys.exit(what[1])
	assert 0

    def work(self):
	ready = select.select([self.p.stdout,self.p.stderr],[],[self.p.stdin,self.p.stdout,self.p.stderr])
	if ready[2]:
	    raise IOError()
	if self.p.stderr in ready[0]:
	    sys.stderr.write(self.p.stderr.read(1))
	if self.p.stdout in ready[0]:
	    size = int(self.p.stdout.readline())
	    data = self.p.stdout.read(size)
	    assert len(data)==size
	    answer = cPickle.dumps(self.doCommand(cPickle.loads(data)))
	    self.p.stdin.write("%d\n%s"%(len(answer),answer))
	    self.p.stdin.flush()

    def run(self):
	self.p = subprocess.Popen(["ssh",self.userAndHost,"python -u -c '%s'"%bootstrapSource],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
	try:
	    self.p.stdin.write("%d\n%s"%(len(pysyncSource),pysyncSource))
	    while 1:
		self.work()
	finally:
	    os.kill(self.p.pid,9)

LocalPysync(sys.argv).run()
