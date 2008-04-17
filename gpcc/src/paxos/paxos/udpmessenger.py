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

    def log(self,s):
	self.__messenger.log(s)

    def getAddress(self):
	return self.__address

    def getMessenger(self):
	return self.__messenger

    def send(self,to,messag):
	self.log("from="+self.__address+" to="+to+" send "+messag)
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
		start=time.time()
		p = self.__socket.recvfrom(65536)
	    except socket.error:
		return (None,None)
	    if random.random()>=DROP_CHANCE:
		break
	fro = socketAddressToString(p[1])
	self.log("from="+fro+" to="+self.__address+" recv "+p[0])
	return (p[0],fro)

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
	    a=socket.gethostbyname(address)
	except:
	    self.log("address="+address)
	    raise
	return (a,port)

    def __init__(self):
	Logger.__init__(self,"Udp")
	self.setDisableLog(True)

    def makePort(self,address):
	return Port(self,address)

    def normalize(self,address):
	return socketAddressToString(self.getSocketAddress(address))
