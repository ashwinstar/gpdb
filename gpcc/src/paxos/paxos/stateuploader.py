import gzip
import os
import threading

from constants import *
from role import Role
from stringchopper import StringChopper

class StateUploader(Role,threading.Thread):

    def __init__(self,priest):
	self.__priest = priest
	port = priest.port.getMessenger().makePort(priest.configuration.host)
	Role.__init__(self,port)
	threading.Thread.__init__(self)

    def __work1(self):
	m,fro = self.port.recv(None)
	sc = StringChopper(m)
	command = sc.nextToken()
	if command!="Get":
	    return
	blockNumber = int(sc.nextToken())
	dn = int(sc.nextToken())
	self.__priest.lock.acquire()
	try:
	    saveDn = self.__priest.learner.saveDn
	    if dn not in saveDn:
		self.port.send(fro,"Error %d %d File not found"%(blockNumber,dn))
		return
	    name = self.__priest.dataDir+"/learner-data%d.gz"%saveDn.index(dn)
	    raf = open(name,"rb")
	    raf.seek(0,2)
	    length = raf.tell()
	    raf.seek(blockNumber*1024)
	    length -= blockNumber*1024
	    if length>1024:
		length = 1024
	    if length<0:
		length = 0
	    c = raf.read(length)
	    raf.close()
	finally:
	    self.__priest.lock.release()
	self.port.send(fro,"Data %d %d "%(blockNumber,dn)+c)

    def work(self):
	while 1:
	    self.__work1()
