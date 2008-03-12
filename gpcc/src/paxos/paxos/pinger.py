import time

import message
import utils

from role import Role
from constants import *

PING_TIME = .1
PING_DEAD_TIME = 1
PING_REMOVE_TIME = 10

class Pinger(Role):
    def __init__(self,priest):
	Role.__init__(self,priest.port)
	self.__pingTime = 0
	self.__configSet = utils.stringToAddresses(self.port.getMessenger(),priest.configuration.priests)
	self.__pingMap = dict()
	self.__idMap = dict()
	self.__subtractMap = dict()
	self.__resultMap = dict()
	self.__priest = priest

    def __cleanMap(self,map):
	"""Remove timed out stuff from map."""
	now = time.time()
	for i in map.keys():
	    if map[i][0]<now:
		map.pop(i)
 
    def getLeader(self,dn):
	"""Gets the leader.

	dn: decree number
	if dn<0, returns leader for large decree numbers
	otherwise, returns leader for specified decree number
	"""
	leader = self.address;
	for i in self.__pingMap.iteritems():
	    if dn<0 or i[1][1]<=dn:
		if i[0]<leader:
		    leader = i[0]
	return leader

    def __gotPing(self,fro,id,dn,result):
	"""Handle Ping message.

	fro: from address
	id: id of Priest sending Ping
	dn: Priest has learned all decrees < dn
	result: comma separated list of addresses of priests
	"""
	now = time.time()
	self.__pingMap[fro] = [now+PING_DEAD_TIME,dn]
	self.__idMap[id] = now+PING_REMOVE_TIME
	for i in result.split(','):
	    self.__resultMap[i] = [now+PING_DEAD_TIME]

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
	m[priestId] = [now+PING_DEAD_TIME]
	if len(m)>len(ps)/2:
	    self.__subtractMap.pop(result)
	    self.__priest.learner.priestSet.subtract(result)

    def tick(self,decreeNumber):
	"""Handle timeouts."""
	now = time.time()
	if now<self.__pingTime:
	    return
	for i in self.__subtractMap.keys():
	    m = self.__subtractMap[i]
	    self.__cleanMap(m)
	    if len(m)==0:
		self.__subtractMap.pop(i)
	self.__cleanMap(self.__pingMap)
	self.__cleanMap(self.__resultMap)
	self.__pingTime = now+PING_TIME
	mm = message.Maker("Ping")
	mm.addDecreeNumber(decreeNumber-ALPHA)
	mm.addPriestId(self.__priest.acceptor.id)
	r = set(self.__configSet)
	r |= set(self.__pingMap.keys())
	r.discard(self.address)
	sb = ""
	for s in r:
	    if sb:
		sb += ','
	    sb += s
	mm.addResult(sb)
	mm.send(self.port,self.getPriestAddresses())
	ps = self.__priest.learner.priestSet.get(decreeNumber)
	for i in self.__idMap.keys():
	    if i not in ps:
		self.__idMap.pop(i)
	for id in ps:
	    if id==self.__priest.acceptor.id:
		continue
	    if id not in self.__idMap:
		self.__idMap[id] = now+PING_REMOVE_TIME
	    if self.__idMap[id]<now:
		mm = message.Maker("SubtractPriest")
		mm.addPriestId(self.__priest.acceptor.id)
		mm.addResult(id)
		mm.send(self.port,self.getLeader(-1))
		self.__idMap[id] = now+PING_TIME

    def sendRedirect(self,to):
	"""Send a Redirect message."""
	mm = message.Maker("Redirect")
	mm.addResult(self.getLeader(-1))
	mm.send(self.port,to)

    def getPriestAddresses(self):
	"""Returns set of addresses of priests."""
	s = set(self.__configSet)
	s |= set(self.__resultMap.keys())
	s |= set(self.__pingMap.keys())
	return s

    def handleMessage(self,decreeNumber,m):
	if m.command=="Ping":
	    self.__gotPing(m.fro,m.priestId,m.dn,m.result)
	elif m.command=="GetLeader":
	    self.sendRedirect(m.fro);
	elif m.command=="SubtractPriest":
	    self.__gotSubtractPriest(decreeNumber,m.priestId,m.result)

