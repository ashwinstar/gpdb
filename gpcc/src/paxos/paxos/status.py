import time

import logger
import message
import utils
from role import Role
from message import Message

TIMEOUT = 1

class Status(Role):

    def __init__(self,messenger,argv):
	Role.__init__(self,messenger.makePort(""))
	messenger.setDisableLog(True)
	self.__priests = utils.stringToAddresses(messenger,argv[1])
	self.__printStatus()

    def __printStatus(self):
	leader1 = leader = None
	fro = [None]
	timeo = 0
	while 1:
	    if leader==None:
		mm = message.Maker("GetLeader")
		mm.send(self.port,self.__priests)
	    elif timeo<time.time():
		mm = message.Maker("GetPriestSet")
		mm.send(self.port,leader)
		timeo = time.time()+TIMEOUT
	    messag = self.port.recv(TIMEOUT,fro)
	    if messag==None:
		leader = None
		print "\33[H\33[J"+time.ctime(time.time())
		continue
	    m = Message(messag,fro[0])
	    if m.command=="Redirect":
		leader1 = leader = m.result
	    elif m.command=="PriestSet":
		print "\33[H\33[J"+time.ctime(time.time())
		print
		print "decreeNumber=%d"%m.dn
		print
		print "leader="+leader
		print
		for i in m.result.split(','):
		    print "priest="+i
		return
