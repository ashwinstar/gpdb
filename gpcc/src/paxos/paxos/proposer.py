import time

import message

from constants import *
from role import Role

OLIVE_DAY = "_"
DECREE_TIMEOUT = 1

class Data:

    def __init__(self):
	self.promiseMap = dict()
	self.clearAll(0)

    def clearAll(self,bn):
	self.clear(bn)
	self.tryDecree = None
	self.limitLastTried = bn

    def clear(self,bn):
	self.lastTried = bn
	self.nextBallotTimeout = 0
	self.promiseMap.clear()
	self.promiseDecree = None

class Proposer(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__data = [Data() for i in xrange(ALPHA)]
	self.__etc = Data()
	self.__priest = priest
	self.__ticktimeout = 0

    def __getData(self,decreeNumber,dn):
	if dn==-decreeNumber:
	    return self.__etc
	if decreeNumber-ALPHA<=dn<decreeNumber:
	    return self.__data[dn&ALPHA-1]
	assert 0

    def __sendNextBallot(self,decreeNumber,dn,bn):
	d = self.__getData(decreeNumber,dn)
	d.clear(bn)
	mm = message.Maker("NextBallot")
	mm.addDecreeNumber(dn)
	mm.addBallotNumber(bn)
	mm.send(self.port,self.__priest.pinger.getAddresses())
	d.nextBallotTimeout = time.time()+DECREE_TIMEOUT

    def __tick1(self,decreeNumber,dn):
	if dn>=0 and self.__isLearned(dn):
	    return
	if self.address!=self.__priest.pinger.getLeader(dn):
	    return
	d = self.__getData(decreeNumber,dn)
	if d.nextBallotTimeout<time.time():
	    if self.address!=self.__priest.pinger.getLeader(-1) or NULL_DECREE!=d.promiseDecree:
		self.__sendNextBallot(decreeNumber,dn,d.lastTried+1)

    def tick(self,decreeNumber):
	now = time.time()
	if now<self.__ticktimeout:
	    return
	self.__ticktimeout = now+DECREE_TIMEOUT
	for i in xrange(-ALPHA,0):
	    if decreeNumber+i>=0:
		self.__tick1(decreeNumber,decreeNumber+i)
	self.__tick1(decreeNumber,-decreeNumber)
	if self.__priest.learner.priestSet.isChanging(decreeNumber):
	    self.__gotPropose(decreeNumber,"","")

    def __gotNak1(self,decreeNumber,priestId,dn,bn):
	ps = self.__priest.learner.priestSet.get(abs(dn))
	if priestId not in ps:
	    return
	if self.address!=self.__priest.pinger.getLeader(dn):
	    return
	d = self.__getData(decreeNumber,dn)
	if bn>=d.lastTried:
	    self.__sendNextBallot(decreeNumber,dn,bn+1)

    def __gotNak(self,decreeNumber,priestId,dn,bn):
	if dn<0:
	    dn = -dn
	    if dn<decreeNumber-ALPHA:
		dn = decreeNumber-ALPHA
	    while dn<decreeNumber:
		self.__gotNak1(decreeNumber,priestId,dn,bn)
		dn += 1
	    if dn==decreeNumber:
		self.__gotNak1(decreeNumber,priestId,-dn,bn)
	elif decreeNumber-ALPHA<=dn<decreeNumber:
	    d = self.__getData(decreeNumber,dn)
	    if d.limitLastTried==d.lastTried:
		self.__gotNak(decreeNumber,priestId,-decreeNumber,bn)
	    self.__gotNak1(decreeNumber,priestId,dn,bn)

    def __gotLastVote1(self,decreeNumber,priestId,dn,bn,decree):
	d = self.__getData(decreeNumber,dn)
	if bn!=d.lastTried or d.promiseDecree!=None:
	    return
	ps = self.__priest.learner.priestSet.get(abs(dn))
	if priestId not in ps:
	    return
	if decree not in d.promiseMap:
	    d.promiseMap[decree] = set()
	d.promiseMap[decree].add(priestId)
	biggestDecree = NULL_DECREE
	biggestSize = 0
	for key in d.promiseMap:
	    size = len(d.promiseMap[key])
	    if key!=NULL_DECREE and biggestSize<size:
		biggestSize = size
		biggestDecree = key
	if NULL_DECREE in d.promiseMap:
	    biggestSize += len(d.promiseMap[NULL_DECREE])
	if biggestSize<=len(ps)/2:
	    return
	if biggestDecree==NULL_DECREE and 0<=dn<self.__priest.learner.highestLearnedDn:
	    biggestDecree = OLIVE_DAY
	d.promiseDecree = biggestDecree
	if biggestDecree==NULL_DECREE and d.tryDecree!=None:
	    biggestDecree = d.tryDecree
	if biggestDecree!=NULL_DECREE:
	    mm = message.Maker("BeginBallot")
	    mm.addDecreeNumber(dn)
	    mm.addBallotNumber(bn)
	    mm.addDecree(biggestDecree)
	    mm.send(self.port,self.__priest.pinger.getAcceptorAddresses(dn))

    def __gotLastVote(self,decreeNumber,priestId,dn,bn,decree):
	if dn<0:
	    dn = -dn
	    if dn<decreeNumber-ALPHA:
		dn = decreeNumber-ALPHA
	    while dn<decreeNumber:
		self.__gotLastVote1(decreeNumber,priestId,dn,bn,decree)
		dn += 1
	    if dn==decreeNumber:
		self.__gotLastVote1(decreeNumber,priestId,-dn,bn,decree)
	elif decreeNumber-ALPHA<=dn<decreeNumber:
	    self.__gotLastVote1(decreeNumber,priestId,dn,bn,decree)

    def shiftData(self,decreeNumber,priestSetChanged):
	if priestSetChanged:
	    self.__etc.promiseMap.clear()
	    self.__etc.promiseDecree = None
	d = self.__getData(decreeNumber,decreeNumber-1)
	d.clearAll(self.__etc.lastTried)
	for i in self.__etc.promiseMap.iteritems():
	    d.promiseMap[i[0]] = i[1].copy()
	d.promiseDecree = self.__etc.promiseDecree
	d.nextBallotTimeout = self.__etc.nextBallotTimeout
	self.__tick1(decreeNumber,decreeNumber-1)

    def __sendBeginBallot(self,decreeNumber,fp,dn):
	d = self.__getData(decreeNumber,dn)
	d.promiseDecree = fp
	d.nextBallotTimeout = time.time()+DECREE_TIMEOUT
	mm = message.Maker("BeginBallot")
	mm.addDecreeNumber(dn)
	mm.addBallotNumber(d.lastTried)
	mm.addDecree(fp)
	mm.send(self.port,self.__priest.pinger.getAcceptorAddresses(dn))

    def __gotPropose(self,decreeNumber,fro,decree):
	if self.address!=self.__priest.pinger.getLeader(-1):
	    if fro:
		self.__priest.pinger.sendRedirect(fro)
	    return
	fp = fro+'_'+decree
	for i in xrange(-ALPHA,0):
	    if self.__isLearned(decreeNumber+i):
		continue
	    d = self.__getData(decreeNumber,decreeNumber+i)
	    if NULL_DECREE!=d.promiseDecree:
		continue
	    d.tryDecree = fp
	    self.__sendBeginBallot(decreeNumber,fp,decreeNumber+i)
	    return
	self.log("no room")

    def __isLearned(self,dn):
	return self.__priest.learner.getLearned(dn)!=None

    def handleMessage(self,decreeNumber,m):
	if m.command=="Propose":
	    self.__gotPropose(decreeNumber,m.fro,m.decree)
	elif m.command=="Nak":
	    self.__gotNak(decreeNumber,m.priestId,m.dn,m.bn)
	elif m.command=="LastVote":
	    self.__gotLastVote(decreeNumber,m.priestId,m.dn,m.bn,m.decree)
