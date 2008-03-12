import os
import time

import message

from message import Message
from role import Role
from stringchopper import StringChopper

TIMEOUT = .5

class StateDownloader(Role):

    def __init__(self,priest,filename):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.__messenger = self.port.getMessenger()
	self.__raf = open(filename,"wb")

    def __getBestUrl(self):
	dn = 0
	best = None
	fro = [None]
	while best==None:
	    mm = message.Maker("GetState")
	    mm.send(self.port,self.__priest.pinger.getPriestAddresses())
	    timeout = time.time()+TIMEOUT
	    while timeout>time.time():
		messag = self.port.recv(TIMEOUT,fro)
		if messag==None:
		    continue
		m = Message(messag,fro[0])
		if m.command!="State":
		    continue
		if m.dn>dn:
		    dn = m.dn
		    best = m.result
	self.log("dn="+str(dn)+" url="+best);
	return best

    def __doNftp(self,url):
	assert url.startswith("nftp://")
	url = url[7:]
	i = url.index('/')
	to = url[:i]
	filename = url[i+1:]
	fro = [None]
	p = self.__messenger.makePort(self.__priest.configuration.host)
	bigtimeout = time.time()+10*TIMEOUT
	timeout = 0
	blockNumber = 0
	self.__raf.seek(0)
	self.__raf.truncate()
	while 1:
	    now = time.time()
	    if bigtimeout<now:
		return False
	    if timeout<now:
		timeout = now+TIMEOUT
		p.send(to,"Get "+str(blockNumber)+' '+filename)
	    s = p.recv(TIMEOUT,fro)
	    if s==None:
		continue
	    sc = StringChopper(s)
	    command = sc.nextToken()
	    if int(sc.nextToken())!=blockNumber:
		continue
	    if sc.nextToken()!=filename:
		continue
	    if command=="Error":
		self.log(s)
		return False
	    if command=="Data":
		t = sc.remainingStuff()
		if len(t)==0:
		    return True
		bigtimeout = now+10*TIMEOUT
		self.__raf.write(t)
		to = fro[0]
		blockNumber += 1

    def download(self):
	while not self.__doNftp(self.__getBestUrl()):
	    pass
	self.__raf.flush()
	os.fsync(self.__raf)
	self.__raf.close()
