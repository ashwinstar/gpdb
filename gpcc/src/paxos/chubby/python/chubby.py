import os
import socket
import time

import message
from udpmessenger import UdpMessenger

TIMEOUT = 1
CHUNK_SIZE = 4000
DEFAULT_LEASE_TIME = 12
PAXOS_PROTOCOL_VERSION = 0

TIME_RESOLUTION = .1

E_NOENT = 1
E_ISDIR = 2
E_IFFAILED = 3
E_INVAL = 4

E_LEASEEXPIRED = E_IFFAILED
E_TIMEOUT = 101

def dirname(filename):
    return "/".join(filter(None,filename.split("/"))[:-1])

def fixFilename(s):
    return "/".join(filter(None,s.replace(' ','\0').split("/")))

def unfixFilename(s):
    return s.replace('\0',' ')

def stat1(propose,name):
    a = propose("Stat "+name)
    if a[0]:
	return a
    b = map(int,a[1].split(' '))
    return (0,b[0],b[1],b[2])

class Connection:

    def __init__(self,chubby):
	self.__chubby = chubby
	self.__parent = None
	self.__leaseFilename = None
	self.__leaseDn = 0
	self.__leaseId = None
	self.__leaseExpireTime = 0

    def __propose(self,decree):
	if self.__parent:
	    return self.__parent.__propose("If %s %d "%(fixFilename(self.__leaseFilename),self.__leaseDn)+decree)
	return self.__chubby.propose(decree)

    def __get1(self,name,maxsize):
	dn = 0
	s = []
	length = 0
	while 1:
	    i = CHUNK_SIZE
	    if maxsize!=None and maxsize-length<i:
	        i = maxsize-length
	    a = self.__propose("Get %s %d %d"%(name,length,i))
	    if a[0]:
		return a
	    b = a[1]
	    i = b.index(' ')
	    newdn = int(b[0:i])
	    b = b[i+1:]
	    i = b.index(' ')
	    size = int(b[0:i])
	    b = b[i+1:]
	    i = b.index(' ')
	    parentDn = int(b[0:i])
	    b = b[i+1:]
	    if dn!=newdn:
		dn = newdn
		if length:
		    s = []
		    length = 0
		else:
		    s = [b]
		    length = len(b)
	    else:
		s.append(b)
		length += len(b)
	    if length==size or maxsize!=None and size>=maxsize:
		break
	return (0,dn,"".join(s),parentDn)

    def __stat(self,name):
	return stat1(self.__propose,name)

    def __chubbyStat(self,name):
	return stat1(self.__chubby.propose,name)

    def __rmdir(self,name):
	a = self.__stat(name)
	if a[0]==0:
	    dn = a[1]
	    if not dn:
		return E_NOENT
	    a = self.__propose("If %s %d Rmdir %s"%(name,dn,name))
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(name)
		if a[0]==0 and a[1]==dn:
		    return E_LEASEEXPIRED
	return a[0]

    def __remove(self,name):
	a = self.__stat(name)
	if a[0]==0:
	    dn = a[1]
	    if not dn:
		return E_NOENT
	    a = self.__propose("If %s %d Remove %s"%(name,dn,name))
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(name)
		if a[0]==0 and a[1]==dn:
		    return E_LEASEEXPIRED
	return a[0]

    def __move(self,name,toname):
	a = self.__stat(name)
	if a[0]:
	    return a[0]
	dn = a[1]
	if not dn:
	    return E_NOENT
	a = self.__propose("If %s %d Move %s %s"%(name,dn,name,toname))
	if a[0]==E_IFFAILED:
	    a = self.__chubbyStat(name)
	    if a[0]==0 and a[1]==dn:
		return E_LEASEEXPIRED
	return a[0]

    def __rmdir(self,name):
	a = self.__stat(name)
	if a[0]==0:
	    dn = a[1]
	    if not dn:
		return E_NOENT
	    a = self.__propose("If %s %d Rmdir %s"%(name,dn,name))
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(name)
		if a[0]==0 and a[1]==dn:
		    return E_LEASEEXPIRED
	return a[0]

    def __mkdir(self,name):
	while 1:
	    a = self.__stat(name)
	    if a[0]:
		return a[0]
	    if a[1]:
		return E_INVAL
	    ifname = dirname(name)
	    dn = a[3]
	    if not dn:
		return E_NOENT
	    a = self.__propose("If %s %d Mkdir %s"%(ifname,dn,name))
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(ifname)
		if a[0]:
		    return a[0]
		if a[1]==dn:
		    return E_LEASEEXPIRED
		continue
	    return a[0]

    def __put1(self,name,offset,contents):
	while 1:
	    a = self.__stat(name)
	    if a[0]:
		return a[0]
	    if not a[1]:
		ifname = dirname(name)
		dn = a[3]
		if not dn:
		    return E_NOENT
	    else:
		ifname = name
		dn = a[1]
	    a = self.__propose("If %s %d Put %s %d "%(ifname,dn,name,offset)+contents)
	    if a[0]==E_IFFAILED:
		a = self.__chubbyStat(ifname)
		if a[0]:
		    return a[0]
		if a[1]==dn:
		    return E_LEASEEXPIRED
		if ifname!=name:
		    continue
	    return a[0]

    def __put(self,name,contents):
	return self.__put1(name,0,contents)

    def __putBigFile(self,name,contents):
	offset = 0
	while 1:
	    a = self.__put1(name,offset,contents[offset:offset+CHUNK_SIZE])
	    if a:
		return a
	    offset += CHUNK_SIZE
	    if offset>=len(contents):
		return 0

    def move(self,name,toname):
	"Moves name to toname.  Can wipe out a directory."
	name = fixFilename(name)
	toname = fixFilename(toname)
	return self.__move(name,toname)

    def get(self,name,maxsize=None):
	name = fixFilename(name)
	return self.__get1(name,maxsize)

    def stat(self,name):
	name = fixFilename(name)
	return self.__stat(name)

    def put(self,name,contents):
	name = fixFilename(name)
	return self.__put(name,contents)

    def putBigFile(self,name,contents):
	name = fixFilename(name)
	return self.__putBigFile(name,contents)

    def mkdir(self,name):
	name = fixFilename(name)
	return self.__mkdir(name)

    def rmdir(self,name):
	name = fixFilename(name)
	return self.__rmdir(name)

    def remove(self,name):
	name = fixFilename(name)
	return self.__remove(name)

    def list(self,name):
	name = fixFilename(name)
	a = self.__get1(name,None)
	if a[0]:
	    return a
	return tuple([0,a[1]]+map(unfixFilename,a[2].split(' ')))

    def getLeaseFilenameDecreeNumberConnection(self,fn,dn):
	c = Connection(self.__chubby)
	c.__parent = self
	c.__leaseDn = dn
	c.__leaseFilename = fn
	return c

    def getLeaseFilenameAndDecreeNumber(self):
	if not self.__leaseDn:
	    return (E_INVAL,)
	return (0,self.__leaseFilename,self.__leaseDn)

    def checkLease(self):
	if not self.__leaseDn:
	    return (E_INVAL,)
	a = self.stat(self.__leaseFilename)
	return (a[0] or a[1]!=self.__leaseDn) and E_LEASEEXPIRED or 0

    def __statLease(self,name):
	a = self.get(name+"-info")
	if a[0]==E_NOENT:
	    return (0,0,0,None)
	if a[0]:
	    return a
	i = a[2].index(' ')
	return (0,a[1],int(a[2][:i]),a[2][i+1:])

    def clone(self):
	c = Connection(self.__chubby)
	c.__parent = self.__parent
	c.__leaseDn = self.__leaseDn
	c.__leaseFilename = self.__leaseFilename
	c.__leaseId = self.__leaseId
	c.__leaseExpireTime = self.__leaseExpireTime
	return (0,c)

    def getLease(self,name,timeout,leaseTime=DEFAULT_LEASE_TIME):
	if timeout!=None:
	    timeout += time.time()
	expire = 0
	dn = 0
	di = self.__chubby.getNewId()
	while 1:
	    a = self.__statLease(name)
	    if a[0]:
		return a
	    now = time.time()
	    if a[1]>dn:
		expire = now+a[2]+TIME_RESOLUTION
		dn = a[1]
	    if a[1]==0 or expire<now:
		b = self.put(name,di)
		if b:
		    return (b,)
		b = self.get(name)
		if b[0]:
		    return b
		if b[2]==di:
		    c = Connection(self.__chubby)
		    c.__parent = self
		    c.__leaseDn = b[1]
		    c.__leaseFilename = name
		    c.__leaseId = di
		    if c.renew(leaseTime)==0:
			return (0,c)
	    time.sleep(1)
	    if timeout!=None and time.time()>=timeout:
		return (E_TIMEOUT,)

    def renew(self,leaseTime=DEFAULT_LEASE_TIME):
	if self.__leaseId==None:
	    return E_INVAL
	s = "%d %s"%(leaseTime,self.__leaseId)
	info = self.__leaseFilename+"-info"
	now = time.time()
	e = self.put(info,s)
	if e==0:
	    a = self.get(info)
	    if a[0]:
		e = a[0]
	    elif a[2]==s:
		self.__leaseExpireTime = now+leaseTime
	    else:
		e = E_LEASEEXPIRED
	return e

    def getLeaseExpireTime(self):
	if not self.__leaseDn:
	    return (E_INVAL,)
	return (0,self.__leaseExpireTime)

    def giveUpLease(self):
	self.renew(0)

class Chubby:

    def __init__(self,addresses,timeout=None):
	self.__addresses = addresses.split(',')
	self.__id = socket.gethostname()+"-%d-%f-%d"%(os.getpid(),time.time(),id(self))
	self.__count = 0
	self.__leader = None
	self.__messenger = UdpMessenger()
	self.__timeout = timeout

    def makeConnection(self):
	return Connection(self)

    def getNewId(self):
	self.__count += 1
	return "%s-%d"%(self.__id,self.__count)

    def propose(self,decree):
	decree = "Tag %s "%self.getNewId()+decree
	port = self.__messenger.makePort("")
	getLeaderTimeout = 0
	proposeTimeout = 0
	if self.__timeout!=None:
	    bigTimeout = time.time()+self.__timeout
	try:
	    while 1:
		now = time.time()
		if self.__timeout!=None and bigTimeout<now:
		    return (E_TIMEOUT,)
		if getLeaderTimeout<now:
		    getLeaderTimeout = now+TIMEOUT
		    message.Maker("GetLeader").send(port,self.__addresses)
		if self.__leader and proposeTimeout<now:
		    proposeTimeout = now+TIMEOUT
		    mm = message.Maker("Propose")
		    mm.addDecree(decree)
		    mm.send(port,self.__leader)
		m = message.getMessage(port.recv(TIMEOUT))
		if m==None:
		    continue
		if m.command=="Redirect":
		    if self.__leader!=m.result:
			self.__leader = m.result
			proposeTimeout = 0
		elif m.command=="Result":
		    if m.decree==decree:
			if m.result.startswith("error "):
			    a = m.result[6:]
			    j = a.find(' ')
			    if j!=-1:
				a = a[:j]
			    return (int(a),)
			return (0,m.result)
		else:
		    assert 0
	finally:
	    port.close()
