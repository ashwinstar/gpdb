import random
import threading
import time

import message

from role import Role

class Tester(Role,threading.Thread):
    def __init__(self,priests,messenger):
	Role.__init__(self,messenger.makePort(""))
	threading.Thread.__init__(self)
	self.__priests = priests
	self.__learnedList = []

    def __checkResult(self,dn,decree,result):
	s = decree+' '+result
	self.log("dn="+str(dn)+" decree="+decree+" result="+result)
	while len(self.__learnedList)<=dn:
	    self.__learnedList += [None]
	if self.__learnedList[dn]==None:
	    self.__learnedList[dn] = s
	assert self.__learnedList[dn]==s

    def __readMessages(self):
	while 1:
	    m = message.getMessage(self.port.recv(0))
	    if m==None:
		break
	    if m.command=="Result":
		self.__checkResult(m.dn,m.decree,m.result)
	    else:
		assert m.command=="Redirect"

    def work(self):
	while 1:
	    self.__readMessages()
	    i = random.randint(0,999)
	    j = random.randint(0,999)
	    k = random.randint(0,999)
	    mm = message.Maker("Propose")
	    mm.addDecree("Put /test/%d/%d %d"%(i,j,k))
	    mm.send(self.port,self.__priests)
	    time.sleep(random.random()*1)
