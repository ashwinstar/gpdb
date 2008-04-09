import time

import message
import utils

from role import Role
from constants import *

PING_TIME = .1
PING_DEAD_TIME = 1
PING_REMOVE_TIME = 10

class Data:

    def __init__(self,fro,id,dn,result,now):
	self.time = now+PING_DEAD_TIME
	self.fro = fro
	self.id = id
	self.dn = dn
	self.result = result

class Pinger(Role):

    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__priest = priest
	self.__pingTime = 0
	self.__configSet = utils.stringToAddresses(self.port.getMessenger(),priest.configuration.priests)
	self.__map = dict()
	self.__extra = set()
	self.__subtractTime = dict()
	self.__subtractMap = dict()

    def getLeader(self,dn):
	"""Gets the leader.

	dn: decree number
	if dn<0, returns leader for large decree numbers
	otherwise, returns leader for specified decree number
	"""
	leader = None
	for d in self.__map.itervalues():
	    if dn<0 and d.dn>=0 or dn>=0 and abs(d.dn)<=dn+ALPHA:
		if not leader or d.fro<leader:
		    leader = d.fro
	return leader or self.address

    def isFallenBehind(self,decreeNumber):
	for d in self.__map.itervalues():
	    if decreeNumber+ALPHA<=abs(d.dn):
		return True
	return False

    def __gotPing(self,fro,id,dn,result):
	"""Handle Ping message.

	fro: from address
	id: id of Priest sending Ping
	dn: Priest has learned all decrees < dn-ALPHA
	result: comma separated list of addresses of priests
	"""
	now = time.time()
	if id:
	    self.__map[id] = Data(fro,id,dn,result,now)
	    self.__subtractTime[id] = now+PING_REMOVE_TIME
	else:
	    self.__extra.add(fro)

    def __gotSubtractPriest(self,decreeNumber,priestId,result):
	if result==self.__priest.acceptor.id:
	    return
	ps = self.__priest.learner.priestSet.get(decreeNumber)
	if priestId not in ps or result not in ps:
	    return
	now = time.time()
	if result not in self.__subtractMap:
	    self.__subtractMap[result] = dict()
	m = self.__subtractMap[result]
	m[priestId] = now+PING_DEAD_TIME
	if len(m)>len(ps)/2:
	    self.__subtractMap.pop(result)
	    self.__priest.learner.priestSet.subtract(result)

    def tick(self,decreeNumber):
	"""Handle timeouts."""
	now = time.time()
	if now<self.__pingTime:
	    return
	self.__pingTime = now+PING_TIME
	for i in self.__subtractMap.keys():
	    m = self.__subtractMap[i]
	    for j in m.keys():
		if m[j]<now:
		    m.pop(j)
	    if not m:
		self.__subtractMap.pop(i)
	for i in self.__map.keys():
	    if self.__map[i].time<now:
		self.__map.pop(i)
	mm = message.Maker("Ping")
	if self.isFallenBehind(decreeNumber):
	    mm.addDecreeNumber(-decreeNumber)
	else:
	    mm.addDecreeNumber(decreeNumber)
	mm.addPriestId(self.__priest.acceptor.id)
	r = set(self.__configSet)
	r.discard(self.address)
	mm.addResult(",".join(r))
	mm.send(self.port,self.getAddresses()|self.__extra)
	self.__extra = set()
	ps = self.__priest.learner.priestSet.get(decreeNumber)
	for i in self.__subtractTime.keys():
	    if i not in ps:
		self.__subtractTime.pop(i)
	for id in ps:
	    if id==self.__priest.acceptor.id:
		continue
	    if id not in self.__subtractTime:
		self.__subtractTime[id] = now+PING_DEAD_TIME
	    if self.__subtractTime[id]<now:
		mm = message.Maker("SubtractPriest")
		mm.addPriestId(self.__priest.acceptor.id)
		mm.addResult(id)
		mm.send(self.port,self.getLeader(-1))
		self.__subtractTime[id] = now+PING_TIME

    def sendRedirect(self,fro):
	"""Send a Redirect message."""
	mm = message.Maker("Redirect")
	mm.addResult(self.getLeader(-1))
	mm.send(self.port,fro)

    def getAcceptorAddresses(self,dn):
	"""Returns addresses of acceptor priests."""
	s = set()
	for i in self.__priest.learner.priestSet.get(dn):
	    if i in self.__map:
		s.add(self.__map[i].fro)
	return s

    def getAddresses(self):
	"""Returns set of addresses of priests."""
	s = set(self.__configSet)
	for d in self.__map.itervalues():
	    s.add(d.fro)
	    s.update(d.result.split(','))
	return s

    def handleMessage(self,decreeNumber,m):
	if m.command=="Ping":
	    self.__gotPing(m.fro,m.priestId,m.dn,m.result)
	elif m.command=="GetLeader":
	    self.sendRedirect(m.fro)
	elif m.command=="SubtractPriest":
	    self.__gotSubtractPriest(decreeNumber,m.priestId,m.result)
