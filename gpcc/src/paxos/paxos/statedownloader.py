import gzip
import os
import threading
import time

import message

from role import Role
from state import State
from stringchopper import StringChopper

TIMEOUT = 1

class StateDownloader(Role,threading.Thread):

    def __init__(self,priest,filename):
	Role.__init__(self,priest.port)
	threading.Thread.__init__(self)
	self.__priest = priest
	self.__raf = open(filename,"wb")
	self.filename = filename

    def __getBestUrl(self):
	dn = 0
	best = None
	while best==None:
	    mm = message.Maker("GetState")
	    self.__priest.lock.acquire()
	    try:
		a = self.__priest.pinger.getAddresses()
	    finally:
		self.__priest.lock.release()
	    mm.send(self.port,a)
	    timeout = time.time()+TIMEOUT
	    while timeout>time.time():
		m = message.getMessage(self.port.recv(TIMEOUT))
		if m==None:
		    continue
		if m.command!="State":
		    continue
		if m.dn>dn:
		    dn = m.dn
		    best = m.result
	self.log("dn="+str(dn)+" url="+best)
	return best

    def __doNftp(self,url):
	assert url.startswith("nftp://")
	url = url[7:]
	i = url.index('/')
	to = url[:i]
	filename = url[i+1:]
	p = self.port.getMessenger().makePort(self.__priest.configuration.host)
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
	    s,fro = p.recv(TIMEOUT)
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
		to = fro
		blockNumber += 1

    def work(self):
	while not self.__doNftp(self.__getBestUrl()):
	    pass
	self.__raf.flush()
	os.fsync(self.__raf)
	self.__raf.close()
	dis = gzip.GzipFile(self.filename,"rb")
	self.state = State()
	self.state.read(dis)
	dis.close()
