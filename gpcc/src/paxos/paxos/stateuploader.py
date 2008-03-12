import gzip
import os

from role import Role
from stringchopper import StringChopper

class StateUploader(Role):

    def __init__(self,priest,saveDn,saveDnL):
	self.__priest = priest
	self.__saveDn = saveDn
	self.__saveDnL = saveDnL
	self.__port = priest.port.getMessenger().makePort(priest.configuration.host)
	Role.__init__(self,self.__port)

    def work(self):
	fro = [None]
	sc = StringChopper(self.__port.recv(None,fro))
	command = sc.nextToken()
	if command!="Get":
	    return
	blockNumber = int(sc.nextToken())
	dn = int(sc.nextToken())
	self.__saveDnL.acquire()
	try:
	    if dn not in self.__saveDn:
		self.__port.send(fro[0],"Error %d %d File not found"%(blockNumber,dn))
		return
	    name = self.__priest.dataDir+"/learner-data%d.gz"%self.__saveDn.index(dn)
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
	    self.__saveDnL.release()
	self.__port.send(fro[0],"Data %d %d "%(blockNumber,dn)+c)
