#!/usr/bin/env python

import fcntl
import select
import subprocess

bootstrapSource="""
import sys
exec(sys.stdin.read(int(sys.stdin.readline())))
"""

pysyncSource="""

import cPickle
import os
import socket
import stat
import sys
import time

def statToTuple(s,name):
    if stat.S_ISDIR(s.st_mode):
	return ('d',s.st_mode)
    if stat.S_ISREG(s.st_mode):
	return ('-',s.st_mode,s.st_size)
    if stat.S_ISLNK(s.st_mode):
	return ('l',0,os.readlink(name))
    if stat.S_ISCHR(s.st_mode):
	return ('c',s.st_mode,s.st_rdev)
    if stat.S_ISBLK(s.st_mode):
	return ('b',s.st_mode,s.st_rdev)
    if stat.S_ISFIFO(s.st_mode):
	return ('p',s.st_mode)
    if stat.S_ISSOCK(s.st_mode):
	return ('s',s.st_mode)
    raise Exception('Pysync can not handle %s.'%name)

class RemotePysync:

    def doCommand(self,*args):
	a = cPickle.dumps(args)
	sys.stdout.write('%d\\n%s'%(len(a),a))
	sys.stdout.flush()
	size = int(sys.stdin.readline())
	data = sys.stdin.read(size)
	assert(size==len(data))
	return cPickle.loads(data)

    def removeJunk(self,list):
	for root,dirs,files in os.walk('.',False):
	    os.chmod(root,0700)
	    for i in dirs:
		name = os.path.join(root,i)
		if name not in list or list[name][0]!='d':
		    self.doCommand('log','rmdir '+name)
		    os.rmdir(name)
	    for i in files:
		name = os.path.join(root,i)
		if name not in list or list[name][0]=='d':
		    self.doCommand('log','remove '+name)
		    os.remove(name)

    def createDirs(self,list):
	for name in sorted(list.keys()):
	    value = list[name]
	    if value[0]=='d' and not os.path.exists(name):
		self.doCommand('log','mkdir '+name)
		os.mkdir(name,0700)

    def createFiles(self,list):
	for name,value in list.iteritems():
	    if value[0]=='d' or value[0]=='L':
		continue
	    if os.path.lexists(name):
		t = statToTuple(os.lstat(name),name)
		if value[0]==t[0]:
		    if value[0]!='l':
			os.chmod(name,0700)
		    if value[0]=='-':
			if t[2]>value[2]:
			    self.doCommand('log','truncate '+name)
			    f = open(name,'r+b')
			    f.truncate(value[2])
			    f.close()
			continue
		    if value[2:]==t[2:]:
			continue
		self.doCommand('log','remove '+name)
		os.remove(name)
	    if value[0]=='-':
		self.doCommand('log','create '+name)
		open(name,'w+b').close()
	    elif value[0]=='l':
		self.doCommand('log','ln -s '+value[2]+' '+name)
		os.symlink(value[2],name)
	    elif value[0]=='c':
		self.doCommand('log','mknod '+name+' c '+value[2])
		os.mknod(name,0700|stat.S_IFCHR,value[2])
	    elif value[0]=='b':
		self.doCommand('log','mknod '+name+' b '+value[2])
		os.mknod(name,0700|stat.S_IFBLK,value[2])
	    elif value[0]=='p':
		self.doCommand('log','mkfifo '+name)
		os.mkfifo(name,0700)
	    elif value[0]=='s':
		self.doCommand('log','bind '+name)
		f = socket.socket(socket.AF_UNIX)
		f.bind(name)
		f.close()
	    else:
		assert 0

    def linkLinks(self,list):
	for name,value in list.iteritems():
	    if value[0]=='L':
		self.doCommand('log','ln '+value[1]+' to '+name)
		if os.path.lexists(name):
		    os.remove(name)
		os.link(value[1],name)

    def copyData(self,list):
	pass

    def fixPermissions(self,list):
	for name,value in list.iteritems():
	    if value[0]!='L' and value[0]!='l':
		self.doCommand('log','chmod '+name)
		os.chmod(name,value[1]&01777)

    def run(self):
	try:
	    os.chdir(self.doCommand('getdestdir'))
	    self.sockname = self.doCommand('getsockname')
	    list = self.doCommand('getlist')
	    self.doCommand('log','list=%s'%str(list))
	    self.doCommand('log','removeJunk')
	    self.removeJunk(list)
	    self.doCommand('log','createDirs')
	    self.createDirs(list)
	    self.doCommand('log','createFiles')
	    self.createFiles(list)
	    self.doCommand('log','linkLinks')
	    self.linkLinks(list)
	    self.doCommand('log','copyData')
	    self.copyData(list)
	    self.doCommand('log','fixPermissions')
	    self.fixPermissions(list)
	    self.doCommand('log','quit')
	    self.doCommand('quit',0)
	except Exception,e:
	    self.doCommand('raise',e)

if 'pysyncSource' not in globals():
    RemotePysync().run()
"""

exec(pysyncSource)

class LocalPysync:

    def __init__(self,argv):
	self.verbose = False
	a = argv[1:]
	while a:
	    if a[0]=='-verbose':
		a.pop(0)
		self.verbose = True
	    else:
		break
	if len(a)!=2:
	    self.usage(argv)
	sourcedir = a[0]
	dest = a[1]
	i = dest.find(':')
	if i==-1:
	    self.usage(argv)
	os.chdir(sourcedir)
	self.userAndHost = dest[:i]
	self.destdir = dest[i+1:]
	self.socket = socket.socket()
	self.socket.bind((socket.gethostname(),0))

    def usage(self,argv):
	sys.stderr.write("usage:\n")
	sys.stderr.write("    python "+argv[0]+" [-verbose] sourcedir [user@]host:destdir\n")
	sys.exit(1)

    def getlist(self):
	list = dict()
	inomap = dict()
	for root,dirs,files in os.walk('.'):
	    for i in dirs+files:
		name = os.path.join(root,i)
		s = os.lstat(name)
		if s.st_ino in inomap:
		    list[name] = ('L',inomap[s.st_ino])
		    continue
		inomap[s.st_ino] = name
		list[name] = statToTuple(s,name)
	return list

    def doCommand(self,what):
	if what[0]=='getsockname':
	    return self.socket.getsockname()
	if what[0]=='getdestdir':
	    return self.destdir
	if what[0]=='getlist':
	    return self.getlist()
	if what[0]=='raise':
	    raise what[1]
	if what[0]=='log':
	    if self.verbose:
		print what[1]
	    return
	if what[0]=='quit':
	    sys.exit(what[1])
	assert 0

    def work(self):
	rset = [self.p.stdout,self.p.stderr]
	while rset:
	    ready = select.select(list(rset),[],[self.p.stdout,self.p.stderr,self.p.stdin])
	    if ready[2]:
		raise IOError()
	    if self.p.stderr in ready[0]:
		a = self.p.stderr.read(1)
		if len(a)==0:
		    rset.remove(self.p.stderr)
		else:
		    sys.stderr.write(a)
	    if self.p.stdout in ready[0]:
		a = self.p.stdout.readline()
		if len(a)==0:
		    rset.remove(self.p.stdout)
		else:
		    size = int(a)
		    data = self.p.stdout.read(size)
		    assert len(data)==size
		    answer = cPickle.dumps(self.doCommand(cPickle.loads(data)))
		    self.p.stdin.write("%d\n%s"%(len(answer),answer))
		    self.p.stdin.flush()

    def run(self):
	self.p = subprocess.Popen(["ssh",self.userAndHost,"python -u -c '%s'"%bootstrapSource],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
	try:
	    self.p.stdin.write("%d\n%s"%(len(pysyncSource),pysyncSource))
	    self.work()
	finally:
	    os.kill(self.p.pid,9)

LocalPysync(sys.argv).run()
