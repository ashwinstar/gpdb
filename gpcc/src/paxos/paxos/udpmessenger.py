import random
import socket
import threading
import time

from logger import Logger

DROP_CHANCE = .0

def getSocketAddress(address):
    port = 0
    i = address.find(':')
    if i!=-1:
	port = int(address[i+1:])
	address = address[0:i]
    if not address:
	address = socket.gethostname()
    return (socket.gethostbyname(address),port)

def socketAddressToString(sa):
    return sa[0]+":%d"%sa[1]

class Port:
    def __init__(self,messenger,address):
	self.__messenger = messenger
	self.__socket = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
	self.__socket.bind(getSocketAddress(address))
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
	    self.__socket.sendto(messag,getSocketAddress(to))

    def recv(self,timeout,fro):
	self.__socket.settimeout(timeout)
	while 1:
	    try:
		start=time.time()
		p = self.__socket.recvfrom(65536)
	    except socket.error,e:
		if e.args[0]==11 or e.args[0]=="timed out":
		    return None
		raise
	    if random.random()>=DROP_CHANCE:
		break
	fro[0] = socketAddressToString(p[1])
	self.log("from="+fro[0]+" to="+self.__address+" recv "+p[0]);
	return p[0]

    def close(self):
	self.__socket.close()

class UdpMessenger(Logger):

    def __init__(self):
	Logger.__init__(self,"Udp")

    def makePort(self,address):
	return Port(self,address)

    def normalize(self,address):
	return socketAddressToString(getSocketAddress(address))
