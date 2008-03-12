import time

import logger
import message
import utils
from role import Role
from message import Message

TIMEOUT = .5

class Client(Role):

    def __init__(self,messenger,argv):
	Role.__init__(self,messenger.makePort(""))
	self.__counter = 0
	logger.setDisable(True)
	self.__priests = utils.stringToAddresses(messenger,argv[1])
	if len(argv)==2:
	    self.__interactive()
	else:
	    sb = argv[2]
	    for i in argv[3:]:
		sb += ' '+i
	    self.__sendPropose(sb)

    def __sendPropose(self,decree):
	leader = None
	fro = [None]
	decree = "Tag %f-%d "%(time.time(),self.__counter)+decree
	self.__counter += 1
	while 1:
	    if leader==None:
		mm = message.Maker("GetLeader")
		mm.send(self.port,self.__priests)
	    else:
		mm = message.Maker("Propose")
		mm.addDecree(decree)
		mm.send(self.port,leader)
	    messag = self.port.recv(TIMEOUT,fro)
	    if messag==None:
		leader = None
		continue
	    m = Message(messag,fro[0])
	    if m.command=="Redirect":
		leader = m.result
	    elif m.command=="Result":
		if m.decree==decree:
		    print "dn="+str(m.dn)+" result="+m.result
		    return
	    else:
		assert 0

    def interactive(self):
	while 1:
	    s = raw_input("decree: ")
	    if not s:
		return
	    self.__sendPropose(s.strip())
