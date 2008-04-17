import os
import socket
import time

TIMEOUT = 1
CHUNK_SIZE = 4000
DEFAULT_LOCK_TIME = 12
PAXOS_PROTOCOL_VERSION = 0

TIME_RESOLUTION = .1

E_NOENT = 1
E_ISDIR = 2
E_IFFAILED = 3
E_INVAL = 4

E_LOCKEXPIRED = E_IFFAILED
E_TIMEOUT = 101

class Connection:

    def __init__(self,chubby):
	self.__chubby = chubby
	self.__parent = None
	self.__lockFilename = None
	self.__lockDn = 0
	self.__lockId = None
	self.__lockExpireTime = 0

    @staticmethod
    def __fixFilename(s):
	return s.replace(' ','\0')

    @staticmethod
    def __unfixFilename(s):
	return s.replace('\0',' ')

    def __propose(self,decree):
	if self.__parent:
	    return self.__parent.__propose("If %s %d "%(self.__fixFilename(self.__lockFilename),self.__lockDn)+decree)
	return self.__chubby.propose(decree)

    def __get1(self,name):
	dn = 0
	s = []
	length = 0
	while 1:
	    a = self.__propose("Get %s %d %d"%(name,length,CHUNK_SIZE))
	    if a[0]!=0:
		return a
	    b = a[1]
	    i = b.index(' ')
	    newdn = int(b[0:i])
	    b = b[i+1:]
	    i = b.index(' ')
	    size = int(b[0:i])
	    b = b[i+1:]
	    if dn!=newdn:
		dn = newdn
		if length!=0:
		    s = []
		    length = 0
		else:
		    s = [b]
		    length = len(b)
	    else:
		s.append(b)
		length += len(b)
	    if length==size:
		break
	return (0,dn,"".join(s))

    @staticmethod
    def __stat1(propose,name):
	a = propose("Stat "+name)
	if a[0]==E_NOENT:
	    return (0,0,0)
	if a[0]!=0:
	    return a
	b = a[1]
	i = b.index(' ')
	return (0,int(b[:i]),int(b[i+1:]))

    def __stat(self,name):
	return self.__stat1(self.__propose,name)

    def __chubbyStat(self,name):
	return self.__stat1(self.__chubby.propose,name)

    def __remove(self,name):
	a = self.__stat(name)
	if a[0]==0:
	    dn = a[1]
	    if dn==0:
		return E_NOENT
	    a = self.__propose("If %s %d Remove %s"%(name,dn,name))
	    if a[0]==E_NOENT:
		return 0
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(name)
		if a[0]==0 and a[1]==dn:
		    return E_LOCKEXPIRED
	return a[0]

    def __put(self,name,contents):
	a = self.__stat(name)
	if a[0]!=0:
	    return a[0]
	dn = a[1]
	a = self.__propose("If %s %d Put %s 0 "%(name,dn,name)+contents)
	if a[0]==E_IFFAILED:
	    a = self.__chubbyStat(name)
	    if a[0]==0 and a[1]==dn:
		return E_LOCKEXPIRED
	return a[0]

    def __putBigFile(self,name,contents):
	offset = 0
	while 1:
	    a = self.__propose("Put %s %d "%(name,offset)+contents[offset:offset+CHUNK_SIZE])
	    if a[0]!=0:
		return a[0]
	    offset += CHUNK_SIZE
	    if offset>=len(contents):
		return 0

    def get(self,name):
	name = self.__fixFilename(name)
	return self.__get1(name)

    def stat(self,name):
	name = self.__fixFilename(name)
	return self.__stat(name)

    def put(self,name,contents):
	name = self.__fixFilename(name)
	return self.__put(name,contents)

    def putBigFile(self,name,contents):
	name = self.__fixFilename(name)
	return self.__putBigFile(name,contents)

    def remove(self,name):
	name = self.__fixFilename(name)
	return self.__remove(name)

    def list(self,name):
	name = self.__fixFilename(name)
	a = self.__get1(name)
	if a[0]!=0:
	    return a
	return tuple([0,a[1]]+map(self.__unfixFilename,a[2].split(' ')))

    def getLockFilenameDecreeNumberConnection(self,fn,dn):
	c = Connection(self.__chubby)
	c.__parent = self
	c.__lockDn = dn
	c.__lockFilename = fn
	return c

    def getLockStringConnection(self,lockstring):
	i = lockstring.find('-')
	dn = int(lockstring[:i])
	fn = lockstring[i+1:]
	return self.getLockFilenameDecreeNumberConnection(fn,dn)

    def getLockString(self):
	return "%d-%s"%(self.__lockDn,self.__lockFilename)

    def checkLock(self):
	a = self.stat(self.__lockFilename)
	return (a[0]!=0 or a[1]!=self.__lockDn) and E_LOCKEXPIRED or 0

    def __statLock(self,name):
	a = self.get(name+"-info")
	if a[0]==E_NOENT:
	    return (0,0,0,None)
	if a[0]!=0:
	    return a
	i = a[2].index(' ')
	return (0,a[1],int(a[2][:i]),a[2][i+1:])

    def lock(self,name,timeout,lockTime=DEFAULT_LOCK_TIME):
	timedout = time.time()+timeout
	expire = 0
	dn = 0
	di = self.__chubby.getNewId()
	while time.time()<timedout:
	    a = self.__statLock(name)
	    if a[0]!=0:
		return a
	    now = time.time()
	    if a[1]>dn:
		expire = now+a[2]+TIME_RESOLUTION
		dn = a[1]
	    if a[1]==0 or expire<now:
		b = self.put(name,di)
		if b!=0:
		    return (b,)
		b = self.get(name)
		if b[0]!=0:
		    return b
		if b[2]==di:
		    c = Connection(self.__chubby)
		    c.__parent = self
		    c.__lockDn = b[1]
		    c.__lockFilename = name
		    c.__lockId = di
		    if c.renew(lockTime)==0:
			return (0,c)
	    time.sleep(1)
	return (E_TIMEOUT,)

    def renew(self,lockTime=DEFAULT_LOCK_TIME):
	if self.__lockId==None:
	    return E_INVAL
	s = "%d %s"%(lockTime,self.__lockId)
	info = self.__lockFilename+"-info"
	now = time.time()
	e = self.put(info,s)
	if e==0:
	    a = self.get(info)
	    if a[0]:
		e = a[0]
	    elif a[2]==s:
		self.__lockExpireTime = now+lockTime
	    else:
		e = E_LOCKEXPIRED
	return e

    def getLockExpireTime(self):
	return (0,self.__lockExpireTime)

    def giveUpLock(self):
	self.renew(0)

class Chubby:

    def __init__(self,addresses):
	self.__addresses = addresses
	self.__id = socket.gethostname()+"-%d-%f-%d"%(os.getpid(),time.time(),id(self))
	self.__count = 0
	self.__leader = None

    def makeConnection(self):
	return Connection(self)

    def getNewId(self):
	self.__count += 1
	return "%s-%d"%(self.__id,self.__count)

    def propose(self,decree):
	decree = "Tag %s "%self.getNewId()+decree
	fd = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
	fd.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
	getLeaderTimeout = 0
	proposeTimeout = 0
	try:
	    fd.settimeout(TIMEOUT)
	    while 1:
		now = time.time()
		if getLeaderTimeout<now:
		    getLeaderTimeout = now+TIMEOUT
		    for a in self.__addresses.split(','):
			if a:
			    self.__sendto(fd,"GetLeader version=%d"%PAXOS_PROTOCOL_VERSION,a)
		if self.__leader and proposeTimeout<now:
		    proposeTimeout = now+TIMEOUT
		    self.__sendto(fd,"Propose version=%d decree=%s"%(PAXOS_PROTOCOL_VERSION,self.__mangle(decree)),self.__leader)
		try:
		    p = fd.recv(65536)
		except socket.error:
		    continue
		p = p.split(' ')
		pversion = None
		presult = None
		pdecree = None
		pcommand = p[0]
		for i in p[1:]:
		    j = i.index('=')
		    key = i[:j]
		    value = i[j+1:]
		    if key=="version":
			pversion = int(value)
		    elif key=="result":
			presult = self.__unmangle(value)
		    elif key=="decree":
			pdecree = self.__unmangle(value)
		if pversion!=PAXOS_PROTOCOL_VERSION:
		    continue
		if pcommand=="Redirect":
		    if self.__leader!=presult:
			self.__leader = presult
			proposeTimeout = 0
		elif pcommand=="Result":
		    if pdecree==decree:
			if presult.startswith("error "):
			    a = presult[6:]
			    j = a.find(' ')
			    if j!=-1:
				a = a[:j]
			    return (int(a),)
			return (0,presult)
		else:
		    assert 0
	finally:
	    fd.close()

    @staticmethod
    def __sendto(fd,what,address):
	i = address.index(':')
	port = int(address[i+1:])
	host = address[0:i]
	fd.sendto(what,(socket.gethostbyname(host),port))

    @staticmethod
    def __mangle(s):
	sb = []
	for c in s:
	    if c==' ' or c=='!':
		sb.append('!')
		c = chr(ord(c)+1)
	    sb.append(c)
	return "".join(sb)

    @staticmethod
    def __unmangle(s):
	sb = []
	escape = False
	for c in s:
	    if escape:
		sb.append(chr(ord(c)-1))
		escape = False
	    elif c=='!':
		escape = True
	    else:
		sb.append(c)
	return "".join(sb)
