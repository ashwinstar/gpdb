import gzip
import os
import threading

import message
import priestset

from constants import *
from priestset import PriestSet
from role import Role
from state import State
from stateuploader import StateUploader
from statedownloader import StateDownloader

LOG_N_SAVE = 2
N_SAVE = 1<<LOG_N_SAVE
LOG_SAVE_PERIOD = LOG_ACCEPTOR_HISTORY-LOG_N_SAVE
DECREE_NUMBER = PAXOS_DIR+"/decreeNumber"

class Data:

    def __init__(self):
	self.clear()

    def clear(self):
	self.map = dict()
	self.decree = None
	self.returnAddress = None
	self.result = None
	self.bn = 0

class Learner(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.__saveDn = N_SAVE*[None]
	self.__saveDnL = threading.Lock()
	self.__data = ALPHA*[None]
	self.__lastPriestSet = set()
	self.highestLearnedDn = 0
	if os.path.exists(priest.dataDir+"/start"):
	    for i in xrange(N_SAVE):
		n = priest.dataDir+"/learner-data%d.gz"%i
		if os.path.exists(n):
		    os.remove(n)
	    self.state = State()
	    self.state.put(DECREE_NUMBER,ACCEPTOR_HISTORY,"")
	    self.state.put(priestset.PRIESTS_DIR+"/initialPriest",0,'+'+priest.acceptor.id)
	    self.__saveData(ACCEPTOR_HISTORY+ALPHA)
	    os.remove(priest.dataDir+"/start")
	else:
	    best = State()
	    best.put(DECREE_NUMBER,0,"")
	    bestDn = 0
	    for i in xrange(N_SAVE):
		f = priest.dataDir+"/learner-data%d.gz"%i
		if os.path.exists(f):
		    s = State()
		    dis = gzip.GzipFile(f,'rb')
		    s.read(dis)
		    dis.close()
		    self.__saveDn[i] = s.get(DECREE_NUMBER).dn
		    if self.__saveDn[i]>bestDn:
			bestDn = self.__saveDn[i]
			best = s
	    self.state = best
	self.priestSet = PriestSet(priest,self.state)
	for i in xrange(ALPHA):
	    self.__data[i] = Data()
	self.__stateUploader = StateUploader(priest,self.__saveDn,self.__saveDnL)
	threading.Thread(None,self.__stateUploader).start()

    def getDecreeNumber(self):
	return self.state.get(DECREE_NUMBER).dn+ALPHA

    def getData(self,decreeNumber,dn):
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    return self.__data[dn&ALPHA-1]
	assert 0

    def __sendResult(self,decreeNumber,dn):
	d = self.getData(decreeNumber,dn)
	mm = message.Maker("Result")
	mm.addDecreeNumber(dn)
	mm.addDecree(d.decree)
	mm.addResult(d.result)
	mm.send(self.port,d.returnAddress)

    def __saveData(self,decreeNumber):
	temp = self.__priest.dataDir+"/learner-temp"
	dos = gzip.GzipFile(temp,'wb')
	self.state.write(dos)
	dos.flush()
	os.fsync(dos)
	dos.close()
	i = decreeNumber-ALPHA>>LOG_SAVE_PERIOD&N_SAVE-1
	self.__saveDnL.acquire()
	try:
	    self.__saveDn[i] = decreeNumber-ALPHA
	    os.rename(temp,self.__priest.dataDir+"/learner-data%d.gz"%i)
	finally:
	    self.__saveDnL.release()

    def __makeRoom(self,decreeNumber):
	while 1:
	    d = self.getData(decreeNumber,decreeNumber-ALPHA)
	    if d.decree==None:
		break
	    d.result = self.state.execute(d.decree,decreeNumber-ALPHA)
	    if d.returnAddress:
		self.__sendResult(decreeNumber,decreeNumber-ALPHA)
	    decreeNumber += 1
	    self.state.put(DECREE_NUMBER,decreeNumber-ALPHA,"")
	    ps = self.priestSet.get(decreeNumber)
	    self.__priest.proposer.shiftData(decreeNumber,ps!=self.__lastPriestSet)
	    self.__lastPriestSet = ps
	    d.clear()
	    self.priestSet.clean(decreeNumber)
	    if (decreeNumber-ALPHA&(1<<LOG_SAVE_PERIOD)-1)==0:
		self.__saveData(decreeNumber)

    def __gotVoted(self,decreeNumber,priestId,dn,bn,decree):
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    ps = self.priestSet.get(dn)
	    assert priestId in ps
	    d = self.getData(decreeNumber,dn)
	    if d.decree!=None or bn<d.bn:
		return
	    if bn>d.bn:
		d.bn = bn
		d.map.clear()
	    if decree not in d.map:
		d.map[decree] = set()
	    s = d.map[decree]
	    s.add(priestId)
	    if len(s)>len(ps)/2:
		mm = message.Maker("Success")
		mm.addDecreeNumber(dn)
		mm.addDecree(decree)
		mm.send(self.port,self.__priest.pinger.getPriestAddresses())

    def __gotSuccess(self,decreeNumber,fro,dn,decree):
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    d = self.getData(decreeNumber,dn)
	    if d.decree!=None:
		return
	    i = decree.find('_')
	    d.returnAddress = decree[0:i]
	    d.decree = decree[i+1:]
	    self.__makeRoom(decreeNumber)
	    if dn>self.highestLearnedDn:
		self.highestLearnedDn = dn

    def __gotGetState(self,fro):
	bestDn = 0
	for i in self.__saveDn:
	    if i>bestDn:
		bestDn = i
	if bestDn:
	    mm = message.Maker("State")
	    mm.addDecreeNumber(bestDn)
	    url = "nftp://%s/%d"%(self.__stateUploader.port.getAddress(),bestDn)
	    mm.addResult(url)
	    mm.send(self.port,fro)

    def __gotDecreeNumberTooOld(self,decreeNumber,dn):
	if decreeNumber>dn+ALPHA:
	    return
	temp = self.__priest.dataDir+"/learner-temp"
	StateDownloader(self.__priest,temp).download()
	dis = gzip.GzipFile(temp,"rb")
	self.state.read(dis);
	dis.close();
	decreeNumber = self.getDecreeNumber()
	i = decreeNumber-ALPHA>>LOG_SAVE_PERIOD&N_SAVE-1
	for d in self.__data:
	    d.clear()
	self.__saveDnL.acquire()
	try:
	    self.__saveDn[i] = decreeNumber-ALPHA
	    name = self.__priest.dataDir+"/learner-data%d.gz"%i
	    os.rename(temp,name)
	finally:
	    self.__saveDnL.release()

    def handleMessage(self,decreeNumber,m):
	if m.command=="Voted":
	    self.__gotVoted(decreeNumber,m.priestId,m.dn,m.bn,m.decree)
	elif m.command=="GetState":
	    self.__gotGetState(m.fro)
	elif m.command=="Success":
	    self.__gotSuccess(decreeNumber,m.fro,m.dn,m.decree)
	elif m.command=="DecreeNumberTooOld":
	    self.__gotDecreeNumberTooOld(decreeNumber,m.dn)
