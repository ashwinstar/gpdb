import time

import message

from constants import *

PRIESTS_DIR = PAXOS_DIR+"/priests"

class PriestSet:

    def __init__(self,priest,state):
	self.__priest = priest
	self.__port = priest.port
	self.__counter = 0

    def __getFilename(self):
	self.__counter += 1
	return PRIESTS_DIR+'/'+self.__port.getAddress()+"-%f-%d"%(time.time(),self.__counter)

    def __addOrSubtract(self,i,addOrSubtract):
	a = self.__priest.learner.state.get(PRIESTS_DIR)
	if a:
	    mm = message.Maker("Propose")
	    mm.addDecree("If %s %d Put %s 0 %s%s"%(PRIESTS_DIR,a.dn,self.__getFilename(),addOrSubtract,i))
	    mm.send(self.__port,self.__priest.pinger.getLeader(-1))

    def add(self,i):
	self.__addOrSubtract(i,'+')

    def subtract(self,i):
	self.__addOrSubtract(i,'-')

    def isChanging(self,dn):
	state = self.__priest.learner.state
	for i in state.list(PRIESTS_DIR):
	    v = state.get(PRIESTS_DIR+'/'+i)
	    if v.dn>dn-2*ALPHA:
		return True
	return False

    def __get1(self,dn):
	state = self.__priest.learner.state
	map = dict()
	for i in state.list(PRIESTS_DIR):
	    v = state.get(PRIESTS_DIR+'/'+i)
	    if dn>=0 and v.dn>dn-ALPHA:
		continue
	    i = v.string[1:]
	    if i not in map or v.dn>abs(map[i]):
		if v.string[0]=='-':
		    map[i] = -v.dn
		else:
		    map[i] = v.dn
	return map

    def clean(self,dn):
	state = self.__priest.learner.state
	map = self.__get1(dn)
	for i in state.list(PRIESTS_DIR):
	    v = state.get(PRIESTS_DIR+'/'+i)
	    if v.dn>dn-ALPHA:
		continue
	    l = map[v.string[1:]]
	    if abs(l)<dn-ACCEPTOR_HISTORY:
		if v.dn!=l:
		    state.remove(dn,PRIESTS_DIR+'/'+i)

    def get(self,dn):
	map = self.__get1(dn)
	s = set()
	for i in map.keys():
	    if map[i]>=0:
		s.add(i)
	return s
