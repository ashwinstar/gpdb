import os
import time

import message
import utils
from constants import *
from role import Role

ADD_PRIEST_TIMEOUT = 1
FILE_SIZE_LIMIT = 1000*ACCEPTOR_HISTORY

class Data:
    def __init__(self):
	self.dn = 0
	self.nextBal = 0
	self.prevVote = None

class Acceptor(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.__datafile0 = priest.dataDir+"/acceptor-data0"
	self.__datafile1 = priest.dataDir+"/acceptor-data1"
	self.__data = [Data() for i in xrange(ACCEPTOR_HISTORY)]
	self.__maxDecreeNumber = 0
	self.__addPriestTimeout = 0
	self.__queue = []
	for i in self.__data:
	    i.dn = -1
	self.id = self.__init()
	self.__raf = open(self.__datafile0,'r+b')
	self.__readHistory()
	self.__raf.close()
	self.__raf = open(self.__datafile1,'r+b')
	self.__readHistory()
	for i in xrange(1-ACCEPTOR_HISTORY,0):
	    dn = self.__maxDecreeNumber+i
	    d = self.__data[dn&ACCEPTOR_HISTORY-1]
	    if d.dn!=dn:
		d.dn = dn;
		d.nextBal = self.__data[self.__maxDecreeNumber].nextBal
		d.prevVote = self.__data[self.__maxDecreeNumber].prevVote

    def __init(self):
	idfile = self.__priest.dataDir+"/acceptor-id"
	if not os.path.exists(idfile):
	    self.__raf = open(self.__datafile0,"wb")
	    d = Data()
	    d.prevVote = NULL_DECREE
	    self.__writeData(d)
	    self.__raf.flush()
	    os.fsync(self.__raf)
	    self.__raf.close()
	    self.__raf = open(self.__datafile1,"wb")
	    self.__raf.close()
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
	utils.writeLong(self.__raf,d.dn)
	utils.writeLong(self.__raf,d.nextBal)
	utils.writeString(self.__raf,d.prevVote)
	self.__raf.write('\n')
	if self.__raf.tell()>FILE_SIZE_LIMIT:
	    for e in self.__data:
		if e.dn!=-1 and e!=d:
		    utils.writeLong(self.__raf,e.dn)
		    utils.writeLong(self.__raf,e.nextBal)
		    utils.writeString(self.__raf,e.prevVote)
		    self.__raf.write('\n')
	    self.__raf.flush()
	    os.fsync(self.__raf)
	    self.__raf.close()
	    os.rename(self.__datafile1,self.__datafile0)
	    self.__raf = open(self.__datafile1,"wb")

    def __getData(self,dn):
	d = self.__data[dn&ACCEPTOR_HISTORY-1]
	assert d.dn==dn
	return d

    def __sendNak(self,fro,dn,bn):
	mm = message.Maker("Nak")
	mm.addDecreeNumber(dn)
	mm.addBallotNumber(bn)
	mm.send(self.port,fro)

    def __gotNextBallot1(self,decreeNumber,fro,dn,bn):
	if dn<0:
	    d = self.__getData(~dn)
	elif dn<decreeNumber:
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
	if dn<0 and ~dn>decreeNumber-ACCEPTOR_HISTORY:
	    dn = ~dn
	    for i in xrange(ALPHA):
		if dn==self.__maxDecreeNumber:
		    self.__gotNextBallot1(decreeNumber,fro,~dn,bn)
		    break
		self.__gotNextBallot1(decreeNumber,fro,dn,bn)
		dn += 1
	elif dn>decreeNumber-ACCEPTOR_HISTORY:
	    self.__gotNextBallot1(decreeNumber,fro,dn,bn);
	else:
	    mm = message.Maker("DecreeNumberTooOld")
	    mm.addDecreeNumber(dn)
	    mm.send(self.port,fro)

    def __gotBeginBallot(self,decreeNumber,fro,dn,bn,decree):
	if dn<=decreeNumber-ACCEPTOR_HISTORY or dn>=decreeNumber:
	    return
	self.__growData(decreeNumber)
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
	if self.__maxDecreeNumber<decreeNumber-ACCEPTOR_HISTORY:
	    self.__maxDecreeNumber = decreeNumber-ACCEPTOR_HISTORY
	while self.__maxDecreeNumber<decreeNumber:
	    d = self.__data[self.__maxDecreeNumber&ACCEPTOR_HISTORY-1]
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
	self.log("len(queue)=%d"%len(self.__queue))
	self.__raf.flush()
	os.fsync(self.__raf)
	while self.__queue:
	    a = self.__queue.pop()
	    self.port.send(a[0],a[1])
