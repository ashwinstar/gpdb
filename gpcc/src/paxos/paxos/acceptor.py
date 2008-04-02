import os
import time

import message
import utils
from constants import *
from role import Role

ADD_PRIEST_TIMEOUT = 1

class Data:
    def __init__(self):
	self.dn = 0
	self.nextBal = 0
	self.prevVote = None

class Acceptor(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.__datafile = priest.dataDir+"/acceptor-data%d"
	self.__data = [Data() for i in xrange(ACCEPTOR_HISTORY)]
	self.__maxDecreeNumber = 0
	self.__addPriestTimeout = 0
	self.__queue = []
	for i in self.__data:
	    i.dn = -1
	self.id = self.__init()
	for i in xrange(2):
	    fn = self.__datafile%i
	    if os.path.exists(fn):
		self.__raf = open(fn,'r+b')
		self.__readHistory()
		self.__raf.close()
	self.__rafIndex = self.__maxDecreeNumber>>LOG_ACCEPTOR_HISTORY
	self.__raf = open(self.__datafile%(self.__rafIndex&1),'ab')
	m = self.__data[self.__maxDecreeNumber&ACCEPTOR_HISTORY-1]
	for i in xrange(1-ACCEPTOR_HISTORY,0):
	    dn = self.__maxDecreeNumber+i
	    d = self.__data[dn&ACCEPTOR_HISTORY-1]
	    if d.dn!=dn:
		d.dn = dn
		d.nextBal = m.nextBal
		d.prevVote = m.prevVote

    def __init(self):
	idfile = self.__priest.dataDir+"/acceptor-id"
	if not os.path.exists(idfile):
	    self.__rafIndex = -1
	    self.__raf = None
	    d = Data()
	    d.prevVote = NULL_DECREE
	    self.__writeData(d)
	    self.__raf.flush()
	    os.fsync(self.__raf)
	    self.__raf.close()
	    self.__raf = None
	    tempfile = self.__priest.dataDir+"/acceptor-temp"
	    f = open(tempfile,"wb")
	    utils.writeString(f,self.address+'-%f'%time.time())
	    f.write('\n')
	    f.flush()
	    os.fsync(f)
	    f.close()
	    os.rename(tempfile,idfile)
	f = open(idfile,"rb")
	i = utils.readString(f)
	f.close()
	return i

    def __readHistory(self):
	l = 0
	while 1:
	    try:
		dn = utils.readLong(self.__raf)
		bn = utils.readLong(self.__raf)
		decree = utils.readString(self.__raf)
		l = self.__raf.tell()
		d = self.__data[dn&ACCEPTOR_HISTORY-1]
		if dn>d.dn:
		    d.dn = dn
		    d.nextBal = bn
		    d.prevVote = decree
		    if dn>self.__maxDecreeNumber:
			self.__maxDecreeNumber = dn
	    except EOFError:
		self.__raf.seek(l)
		self.__raf.truncate()
		return

    def __writeData(self,d):
	i = self.__maxDecreeNumber>>LOG_ACCEPTOR_HISTORY
	if i!=self.__rafIndex:
	    if self.__raf:
		self.__raf.flush()
		os.fsync(self.__raf)
		self.__raf.close()
	    tempfile = self.__priest.dataDir+"/acceptor-temp"
	    self.__raf = open(tempfile,'wb')
	utils.writeLong(self.__raf,d.dn)
	utils.writeLong(self.__raf,d.nextBal)
	utils.writeString(self.__raf,d.prevVote)
	self.__raf.write('\n')
	if i!=self.__rafIndex:
	    self.__rafIndex = i
	    self.__raf.flush()
	    os.fsync(self.__raf)
	    self.__raf.close()
	    fn = self.__datafile%(self.__rafIndex&1)
	    os.rename(tempfile,fn)
	    self.__raf = open(fn,'ab')

    def __getData(self,dn):
	d = self.__data[dn&ACCEPTOR_HISTORY-1]
	assert d.dn==dn
	return d

    def __sendNak(self,fro,dn,bn):
	mm = message.Maker("Nak")
	mm.addPriestId(self.id)
	mm.addDecreeNumber(dn)
	mm.addBallotNumber(bn)
	mm.send(self.port,fro)

    def __sendSuccess(self,fro,dn,decree):
	mm = message.Maker("Success")
	mm.addDecreeNumber(dn)
	mm.addDecree(decree)
	mm.send(self.port,fro)

    def __gotNextBallot1(self,fro,dn,bn):
	if dn>=0:
	    a = self.__priest.learner.getLearned(dn)
	    if a!=None:
		self.__sendSuccess(fro,dn,a)
		return
	if dn<0:
	    d = self.__getData(-dn)
	elif dn<self.__maxDecreeNumber:
	    d = self.__getData(dn)
	else:
	    return
	ps = self.__priest.learner.priestSet.get(d.dn)
	if self.id not in ps:
	    return
	if bn<=d.nextBal:
	    self.__sendNak(fro,dn,d.nextBal)
	else:
	    d.nextBal = bn
	    self.__writeData(d)
	    mm = message.Maker("LastVote")
	    mm.addPriestId(self.id)
	    mm.addDecreeNumber(dn)
	    mm.addBallotNumber(bn)
	    mm.addDecree(d.prevVote)
	    self.__queue.append([fro,mm.toString()])

    def __gotNextBallot(self,decreeNumber,fro,dn,bn):
	self.__growData(decreeNumber)
	if dn<0 and dn<ACCEPTOR_HISTORY-self.__maxDecreeNumber:
	    dn = -dn
	    for i in xrange(ALPHA):
		if dn==self.__maxDecreeNumber:
		    self.__gotNextBallot1(fro,-dn,bn)
		    break
		self.__gotNextBallot1(fro,dn,bn)
		dn += 1
	elif dn>=0 and dn>self.__maxDecreeNumber-ACCEPTOR_HISTORY:
	    self.__gotNextBallot1(fro,dn,bn)
	else:
	    mm = message.Maker("DecreeNumberTooOld")
	    mm.addDecreeNumber(dn)
	    mm.send(self.port,fro)

    def __gotBeginBallot(self,decreeNumber,fro,dn,bn,decree):
	a = self.__priest.learner.getLearned(dn)
	if a!=None:
	    self.__sendSuccess(fro,dn,a)
	    return
	self.__growData(decreeNumber)
	if dn<=self.__maxDecreeNumber-ACCEPTOR_HISTORY or dn>=self.__maxDecreeNumber:
	    return
	d = self.__getData(dn)
	if bn<d.nextBal:
	    self.__sendNak(fro,dn,d.nextBal)
	elif bn==d.nextBal:
	    d.prevVote = decree
	    self.__writeData(d)
	    mm = message.Maker("Voted")
	    mm.addPriestId(self.id)
	    mm.addDecreeNumber(dn)
	    mm.addBallotNumber(bn)
	    mm.addDecree(decree)
	    self.__queue.append([fro,mm.toString()])

    def __growData(self,decreeNumber):
	while self.__maxDecreeNumber<decreeNumber:
	    d = self.__data[self.__maxDecreeNumber&ACCEPTOR_HISTORY-1]
	    if self.__maxDecreeNumber<decreeNumber-ACCEPTOR_HISTORY:
		self.__maxDecreeNumber = decreeNumber-ACCEPTOR_HISTORY
	    self.__maxDecreeNumber += 1
	    e = self.__data[self.__maxDecreeNumber&ACCEPTOR_HISTORY-1]
	    e.dn = self.__maxDecreeNumber
	    e.nextBal = d.nextBal
	    e.prevVote = d.prevVote
	    self.__writeData(e)

    def tick(self,decreeNumber):
	now = time.time()
	if now<self.__addPriestTimeout:
	    return
	if self.__priest.pinger.isFallenBehind(decreeNumber):
	    return
	self.__addPriestTimeout = now+ADD_PRIEST_TIMEOUT
	if self.id not in self.__priest.learner.priestSet.get(decreeNumber):
	    self.__priest.learner.priestSet.add(self.id)

    def handleMessage(self,decreeNumber,m):
	if m.command=="NextBallot":
	    self.__gotNextBallot(decreeNumber,m.fro,m.dn,m.bn)
	elif m.command=="BeginBallot":
	    self.__gotBeginBallot(decreeNumber,m.fro,m.dn,m.bn,m.decree)

    def sync(self):
	if not self.__queue:
	    return
	self.__raf.flush()
	os.fsync(self.__raf)
	while self.__queue:
	    a = self.__queue.pop()
	    self.port.send(a[0],a[1])
