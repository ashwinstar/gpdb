import md5
import os
import random
import socket
import threading
import time

from logger import Logger

DROP_CHANCE = .0

def socketAddressToString(sa):
    return sa[0]+":%d"%sa[1]

class Port:
    def __init__(self,messenger,address):
	self.__messenger = messenger
	self.__socket = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
	self.__socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
	self.__socket.bind(messenger.getSocketAddress(address))
	self.__address = socketAddressToString(self.__socket.getsockname())
	self.__key = messenger.key

    def log(self,s):
	self.__messenger.log(s)

    def getAddress(self):
	return self.__address

    def getMessenger(self):
	return self.__messenger

    def send(self,to,messag):
	self.log("from="+self.__address+" to="+to+" send "+messag)
	m = md5.new()
	m.update(self.__key)
	m.update(to)
	m.update(' ')
	m.update(messag)
	messag = m.hexdigest()+" "+messag
	if random.random()>=DROP_CHANCE:
	    try:
		self.__socket.sendto(messag,self.__messenger.getSocketAddress(to))
	    except:
		self.log("exception with "+messag)
		raise

    def recv(self,timeout):
	self.__socket.settimeout(timeout)
	while 1:
	    try:
		p,fro = self.__socket.recvfrom(65536)
	    except socket.error:
		return (None,None)
	    i = p.find(' ')
	    if i==-1:
		continue
	    fro = socketAddressToString(fro)
	    m = md5.new()
	    m.update(self.__key)
	    m.update(self.__address)
	    m.update(' ')
	    m.update(p[i+1:])
	    if m.hexdigest()!=p[:i]:
		continue
	    p = p[i+1:]
	    if random.random()>=DROP_CHANCE:
		break
	self.log("from="+fro+" to="+self.__address+" recv "+p)
	return (p,fro)

    def close(self):
	self.__socket.close()

class UdpMessenger(Logger):

    def getSocketAddress(self,address):
	port = 0
	i = address.find(':')
	if i!=-1:
	    port = int(address[i+1:])
	    address = address[0:i]
	if not address:
	    address = socket.gethostname()
	try:
	    a = socket.gethostbyname(address)
	except:
	    self.log("address="+address)
	    raise
	return (a,port)

    def __init__(self):
	Logger.__init__(self,"Udp")
	keyfile = os.environ["HOME"]+"/.paxos_key"
	if not os.path.exists(keyfile):
	    idrsa = os.environ["HOME"]+"/.ssh/id_rsa"
	    m = md5.new()
	    if os.path.exists(idrsa):
	    	self.log("hashing %s into %s"%(idrsa,keyfile))
		f = open(idrsa,"r")
		for i in f.read():
		    if i>' ' and i<='~':
		        m.update(i)
		f.close()
	    else:
	    	self.log("No %s or %s.  Creating random key."%(idrsa,keyfile))
		m.update(os.urandom(16))
	    oldumask = os.umask(077)
	    f = open(keyfile,"w")
	    os.umask(oldumask)
	    f.write(m.hexdigest())
	    f.write('\n')
	    f.close()
	key = []
	f = open(keyfile,"r")
	for i in f.read():
	    if i>' ' and i<='~':
		key.append(i)
	f.close()
	self.key = "".join(key)
	self.setDisableLog(True)

    def makePort(self,address):
	return Port(self,address)

    def normalize(self,address):
	return socketAddressToString(self.getSocketAddress(address))
