#!/usr/bin/env python

import subprocess

bootstrapSource="""
import sys
exec(sys.stdin.read(int(sys.stdin.readline())))
"""

pysyncSource="""

import cPickle
import md5
import os
import socket
import stat
import sys
import zlib

CHUNK_SIZE=4000000

class Options:
    pass

def statToTuple(s,name):
    if stat.S_ISREG(s.st_mode):
	return ('-',s.st_mode,s.st_size)
    if stat.S_ISLNK(s.st_mode):
	return ('l',0,os.readlink(name))
    if stat.S_ISDIR(s.st_mode):
	return ('d',s.st_mode)
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

    def log(self,what):
	if self.options.verbose:
	    sys.stderr.write(what+'\\n')
	    sys.stderr.flush()

    def doCommand(self,*args):
	a = cPickle.dumps(args)
	sys.stdout.write('%d\\n%s'%(len(a),a))
	sys.stdout.flush()
	size = int(sys.stdin.readline())
	data = sys.stdin.read(size)
	assert size==len(data)
	return cPickle.loads(data)

    def removeJunk(self,list):
	for root,dirs,files in os.walk('.',False):
	    os.chmod(root,0700)
	    for i in dirs+files:
		name = os.path.join(root,i)
		t = statToTuple(os.lstat(name),name)
		if name not in list and not self.options.minusn or name in list and list[name][0]!=t[0]:
		    if t[0]=='d':
			self.log('rmdir '+name)
			os.rmdir(name)
		    else:
			self.log('remove '+name)
			os.remove(name)

    def createDirs(self,list):
	for name in sorted(list.keys()):
	    value = list[name]
	    if value[0]=='d' and not os.path.exists(name):
		self.log('mkdir '+name)
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
			    self.log('truncate '+name)
			    f = open(name,'r+b')
			    f.truncate(value[2])
			    f.close()
			continue
		    if value[2:]==t[2:]:
			continue
		self.log('remove '+name)
		os.remove(name)
	    if value[0]=='-':
		self.log('create '+name)
		open(name,'w+b').close()
	    elif value[0]=='l':
		self.log('ln -s '+value[2]+' '+name)
		os.symlink(value[2],name)
	    elif value[0]=='c':
		self.log('mknod '+name+' c '+value[2])
		os.mknod(name,0700|stat.S_IFCHR,value[2])
	    elif value[0]=='b':
		self.log('mknod '+name+' b '+value[2])
		os.mknod(name,0700|stat.S_IFBLK,value[2])
	    elif value[0]=='p':
		self.log('mkfifo '+name)
		os.mkfifo(name,0700)
	    elif value[0]=='s':
		self.log('bind '+name)
		f = socket.socket(socket.AF_UNIX)
		f.bind(name)
		f.close()
	    else:
		assert 0

    def linkLinks(self,list):
	for name,value in list.iteritems():
	    if value[0]=='L':
		self.log('ln '+value[1]+' to '+name)
		if os.path.lexists(name):
		    os.remove(name)
		os.link(value[1],name)

    def getData(self,name,offset,size):
	if not self.socket:
	    ss = socket.socket()
	    ss.bind((socket.gethostname(),0))
	    ss.listen(1)
	    self.log('connect '+str(ss.getsockname()))
	    self.doCommand('connect',ss.getsockname())
	    self.socket = ss.accept()[0]
	    ss.close()
	s = self.doCommand('getData',name,offset,size)
	o = 0
	sb = []
	while o<s:
	    a = self.socket.recv(min(s-o,65536))
	    if len(a)==0:
		raise IOError('EOF')
	    sb.append(a)
	    o += len(a)
	data = "".join(sb)
	if self.options.compress:
	    data = zlib.decompress(data)
	assert len(data)==size
	return data

    def copyData(self,list):
	for name,value in list.iteritems():
	    if value[0]=='-':
		f = open(name,'r+b')
		f.seek(0,2)
		localSize = f.tell()
		offset = 0
		while offset<value[2]:
		    size = min(value[2]-offset,CHUNK_SIZE)
		    self.log('digesting %s at %d'%(name,offset))
		    digest = None
		    if offset+size<=localSize:
			digest = self.doCommand('getDigest',name,offset,size)
			f.seek(offset)
			b = f.read(size)
			assert len(b)==size
			if md5.new(b).digest()==digest:
			    offset += size
			    continue
		    self.log('copying %s at %d'%(name,offset))
		    data = self.getData(name,offset,size)
		    if not self.options.insecure:
			if digest==None:
			    digest = self.doCommand('getDigest',name,offset,size)
			if md5.new(data).digest()!=digest:
			    raise Exception('Digest did not match.')
		    f.seek(offset)
		    f.write(data)
		    offset += size

    def fixPermissions(self,list):
	for name,value in list.iteritems():
	    if value[0]!='L' and value[0]!='l':
		self.log('chmod '+name)
		os.chmod(name,value[1]&01777)

    def run(self):
	self.socket = None
	self.options = self.doCommand('getOptions')
	os.chdir(self.doCommand('getDestDir'))
	list = self.doCommand('getList')
	self.log('list=%s'%str(list))
	self.log('removeJunk')
	self.removeJunk(list)
	self.log('createDirs')
	self.createDirs(list)
	self.log('createFiles')
	self.createFiles(list)
	self.log('linkLinks')
	self.linkLinks(list)
	self.log('copyData')
	self.copyData(list)
	self.log('fixPermissions')
	self.fixPermissions(list)
	self.log('quit')
	self.doCommand('quit',0)

if 'pysyncSource' not in globals():
    RemotePysync().run()
"""

exec(pysyncSource)

class LocalPysync:

    def __init__(self,argv):
	self.options = Options()
	self.options.compress = False
	self.options.verbose = False
	self.options.insecure = False
	self.options.minusn = False
	self.cache = [None]
	self.exclude = set()
	a = argv[1:]
	while a:
	    if a[0]=='-verbose':
		self.options.verbose = True
	    elif a[0]=='-compress':
		self.options.compress = True
	    elif a[0]=='-insecure':
		self.options.insecure = True
	    elif a[0]=='-n':
		self.options.minusn = True
	    elif a[0]=='-x':
		a.pop(0)
		name = a[0]
		if name[0]=='/':
		    raise Exception('Please do not use absolute path with -x.')
		if name[0:2]!='./':
		    name = os.path.join('.',name)
		self.exclude.add(name)
	    else:
		break
	    a.pop(0)
	if len(a)!=2:
	    self.usage(argv)
	sourceDir = a[0]
	dest = a[1]
	i = dest.find(':')
	if i==-1:
	    self.usage(argv)
	os.chdir(sourceDir)
	self.userAndHost = dest[:i]
	self.destDir = dest[i+1:]
	self.connectAddress = None
	self.sendData = None

    def usage(self,argv):
	sys.stderr.write("usage:\n")
	sys.stderr.write("    python "+argv[0]+" [-verbose] [-compress] [-insecure] [-n] [-x exclude_file] sourcedir [user@]host:destdir\n")
	sys.exit(1)

    def readFile(self,filename,offset,size):
	key = (filename,offset,size)
	if self.cache[0]==key:
	    return self.cache[1]
	f = open(filename,'rb')
	f.seek(offset)
	a = f.read(size)
	f.close()
	assert len(a)==size
	self.cache = (key,a)
	return a

    def getList(self):
	list = dict()
	inomap = dict()
	for root,dirs,files in os.walk('.'):
	    for i in dirs+files:
		name = os.path.join(root,i)
		if name in self.exclude:
		    if i in dirs:
			dirs.remove(i)
		    continue
		s = os.lstat(name)
		if s.st_ino in inomap:
		    list[name] = ('L',inomap[s.st_ino])
		    continue
		inomap[s.st_ino] = name
		list[name] = statToTuple(s,name)
	return list

    def doCommand(self,what):
	if what[0]=='connect':
	    self.connectAddress = what[1]
	elif what[0]=='getOptions':
	    return self.options
	elif what[0]=='getDestDir':
	    return self.destDir
	elif what[0]=='getList':
	    return self.getList()
	elif what[0]=='getDigest':
	    return md5.new(self.readFile(what[1],what[2],what[3])).digest()
	elif what[0]=='getData':
	    self.sendData = self.readFile(what[1],what[2],what[3])
	    if self.options.compress:
		self.sendData = zlib.compress(self.sendData,1)
	    return len(self.sendData)
	elif what[0]=='quit':
	    sys.exit(what[1])
	else:
	    assert 0

    def work(self):
	while 1:
	    a = self.p.stdout.readline()
	    if len(a)==0:
		break
	    size = int(a)
	    data = self.p.stdout.read(size)
	    assert len(data)==size
	    answer = cPickle.dumps(self.doCommand(cPickle.loads(data)))
	    self.p.stdin.write("%d\n%s"%(len(answer),answer))
	    self.p.stdin.flush()
	    if self.connectAddress!=None:
		self.socket = socket.socket()
		self.socket.connect(self.connectAddress)
		self.connectAddress = None
	    if self.sendData!=None:
		self.socket.sendall(self.sendData)
		self.sendData = None

    def run(self):
	self.p = subprocess.Popen(["ssh",self.userAndHost,"python -u -c '%s'"%bootstrapSource],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
	try:
	    self.p.stdin.write("%d\n%s"%(len(pysyncSource),pysyncSource))
	    self.work()
	finally:
	    os.kill(self.p.pid,9)

LocalPysync(sys.argv).run()
