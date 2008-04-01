import gzip
import os
import threading

import message
import priestset
import utils

from constants import *
from priestset import PriestSet
from role import Role
from state import State
from statedownloader import StateDownloader
from statesaver import StateSaver
from stateuploader import StateUploader

LOG_N_SAVE = 2
N_SAVE = 1<<LOG_N_SAVE
LOG_SAVE_PERIOD = LOG_ACCEPTOR_HISTORY-LOG_N_SAVE
DECREE_NUMBER = PAXOS_DIR+"/decreeNumber"

class Data:

    def __init__(self):
	self.clear(-1)

    def clear(self,dn):
	self.dn = dn
	self.map = dict()
	self.decree = None
	self.alreadyInLog = False
	self.returnAddress = None
	self.result = None
	self.bn = 0
	self.done = False

class Learner(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.saveDn = N_SAVE*[None]
	self.__data = ACCEPTOR_HISTORY*[None]
	self.__lastPriestSet = set()
	self.highestLearnedDn = 0
	best = State()
	best.put(0,DECREE_NUMBER,"")
	if os.path.exists(priest.dataDir+"/start"):
	    best.put(ACCEPTOR_HISTORY,DECREE_NUMBER,"")
	    best.put(0,priestset.PRIESTS_DIR+"/initialPriest",'+'+priest.acceptor.id)
	bestDn = 0
	for i in xrange(N_SAVE):
	    f = priest.dataDir+"/learner-data%d.gz"%i
	    if os.path.exists(f):
		s = State()
		dis = gzip.GzipFile(f,'rb')
		s.read(dis)
		dis.close()
		self.saveDn[i] = s.get(DECREE_NUMBER).dn
		if self.saveDn[i]>bestDn:
		    bestDn = self.saveDn[i]
		    best = s
	self.state = best
	self.priestSet = PriestSet(priest,self.state)
	for i in xrange(ACCEPTOR_HISTORY):
	    self.__data[i] = Data()
	self.__stateUploader = StateUploader(priest)
	self.__stateUploader.start()
	self.__stateDownloader = None
	self.__stateSaver = None
	if bestDn==0:
	    self.__saveState()
	    self.__finishSaveState()
	for i in xrange(N_SAVE):
	    f = priest.dataDir+"/learner-log%d"%i
	    if os.path.exists(f):
		raf = open(f,'r+b')
		l = 0
		try:
		    while 1:
			dn = utils.readLong(raf)
			decree = utils.readString(raf)
			l = raf.tell()
			d = self.__data[dn&ACCEPTOR_HISTORY-1]
			if d.dn<dn:
			    d.clear(dn)
			    d.decree = decree
			    d.alreadyInLog = True
		except EOFError:
		    raf.seek(l)
		    raf.truncate()
		raf.close()
	i = bestDn>>LOG_SAVE_PERIOD&N_SAVE-1
	f = priest.dataDir+"/learner-log%d"%i
	self.__logfile = open(f,"a+b" if os.path.exists(f) else "wb")

    def getDecreeNumber(self):
	return self.state.get(DECREE_NUMBER).dn+ALPHA

    def getLearned(self,dn):
	d = self.__data[dn&ACCEPTOR_HISTORY-1]
	return d.decree if d.dn==dn else None

    def __getData(self,dn):
	d = self.__data[dn&ACCEPTOR_HISTORY-1]
	if d.dn<dn:
	    d.clear(dn)
	assert d.dn==dn
	return d

    def __sendResult(self,dn):
	d = self.__getData(dn)
	mm = message.Maker("Result")
	mm.addDecreeNumber(dn)
	mm.addDecree(d.decree)
	mm.addResult(d.result)
	mm.send(self.port,d.returnAddress)

    def __makeRoom(self,decreeNumber):
	while 1:
	    dn = decreeNumber-ALPHA
	    d = self.__getData(dn)
	    if d.decree==None:
		break
	    if not d.alreadyInLog:
		utils.writeLong(self.__logfile,dn)
		utils.writeString(self.__logfile,d.decree)
		self.__logfile.write('\n')
		self.__logfile.flush()
		os.fsync(self.__logfile)
	    d.result = self.state.execute(d.decree,dn,self.priestSet.get(dn))
	    if d.returnAddress and d.done:
		self.__sendResult(dn)
	    decreeNumber += 1
	    self.state.put(decreeNumber-ALPHA,DECREE_NUMBER,"")
	    ps = self.priestSet.get(decreeNumber)
	    self.__priest.proposer.shiftData(decreeNumber,ps!=self.__lastPriestSet)
	    self.__lastPriestSet = ps
	    self.priestSet.clean(decreeNumber)
	    if (decreeNumber-ALPHA&(1<<LOG_SAVE_PERIOD)-1)==0:
		self.__saveState()
		i = decreeNumber-ALPHA>>LOG_SAVE_PERIOD&N_SAVE-1
		self.__logfile.close()
		self.__logfile = open(self.__priest.dataDir+"/learner-log%d"%i,"wb")

    def __sendSuccess(self,dn,decree):
	mm = message.Maker("Success")
	mm.addDecreeNumber(dn)
	mm.addDecree(decree)
	mm.send(self.port,self.__priest.pinger.getAddresses())

    def __gotVoted(self,decreeNumber,priestId,dn,bn,decree):
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    ps = self.priestSet.get(dn)
	    if priestId not in ps:
		assert 0
		return
	    d = self.__getData(dn)
	    if d.decree!=None or bn<d.bn:
		return
	    if bn>d.bn:
		d.bn = bn
		d.map.clear()
	    if decree not in d.map:
		d.map[decree] = set()
	    s = d.map[decree]
	    s.add(priestId)
	    if len(s)>len(ps)/2 and not d.done:
		d.done = True
		self.__sendSuccess(dn,decree)

    def __gotSuccess(self,decreeNumber,fro,dn,decree):
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    d = self.__getData(dn)
	    if d.decree!=None:
		return
	    i = decree.find('_')
	    d.returnAddress = decree[0:i]
	    d.decree = decree[i+1:]
	    if dn>self.highestLearnedDn:
		self.highestLearnedDn = dn

    def __gotGetState(self,fro):
	bestDn = 0
	for i in self.saveDn:
	    if i>bestDn:
		bestDn = i
	if bestDn:
	    mm = message.Maker("State")
	    mm.addDecreeNumber(bestDn)
	    url = "nftp://%s/%d"%(self.__stateUploader.port.getAddress(),bestDn)
	    mm.addResult(url)
	    mm.send(self.port,fro)

    def __gotDecreeNumberTooOld(self,decreeNumber,dn):
	if decreeNumber>dn+ALPHA or self.__stateDownloader!=None:
	    return
	temp = self.__priest.dataDir+"/learner-download"
	self.__stateDownloader = StateDownloader(self.__priest,temp)
	self.__stateDownloader.start()

    def __finishDownload(self):
	self.__stateDownloader.join()
	self.state = self.__stateDownloader.state
	temp = self.__stateDownloader.filename
	self.__stateDownloader = None
	decreeNumber = self.getDecreeNumber()
	i = decreeNumber-ALPHA>>LOG_SAVE_PERIOD&N_SAVE-1
	for d in self.__data:
	    d.clear(-1)
	self.saveDn[i] = decreeNumber-ALPHA
	name = self.__priest.dataDir+"/learner-data%d.gz"%i
	os.rename(temp,name)

    def __saveState(self):
	if self.__stateSaver!=None:
	    self.__finishSaveState()
	temp = self.__priest.dataDir+"/learner-save"
	dn = self.getDecreeNumber()
	root = self.state.getFixedRoot(dn)
	self.__stateSaver = StateSaver(self.__priest,temp,root,dn)
	self.__stateSaver.start()

    def __finishSaveState(self):
	self.__stateSaver.join()
	dn = self.__stateSaver.dn
	temp = self.__stateSaver.filename
	self.__stateSaver = None
	i = dn-ALPHA>>LOG_SAVE_PERIOD&N_SAVE-1
	self.saveDn[i] = dn-ALPHA
	os.rename(temp,self.__priest.dataDir+"/learner-data%d.gz"%i)

    def handleMessage(self,decreeNumber,m):
	if self.__stateDownloader:
	    if not self.__stateDownloader.isAlive():
		self.__finishDownload()
	    return
	if self.__stateSaver and not self.__stateSaver.isAlive():
	    self.__finishSaveState()
	if m.command=="Voted":
	    self.__gotVoted(decreeNumber,m.priestId,m.dn,m.bn,m.decree)
	elif m.command=="GetState":
	    self.__gotGetState(m.fro)
	elif m.command=="Success":
	    self.__gotSuccess(decreeNumber,m.fro,m.dn,m.decree)
	elif m.command=="DecreeNumberTooOld":
	    self.__gotDecreeNumberTooOld(decreeNumber,m.dn)
	self.__makeRoom(decreeNumber)
