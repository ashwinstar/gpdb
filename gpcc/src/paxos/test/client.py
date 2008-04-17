import sys
import time

import logger
import message
import utils
from udpmessenger import UdpMessenger

TIMEOUT = 1

class Client:

    def __init__(self,messenger,argv):
	self.port = messenger.makePort("")
	self.__counter = 0
	self.__priests = utils.stringToAddresses(messenger,argv[1])
	if len(argv)==2:
	    self.__interactive()
	else:
	    self.__sendPropose(" ".join(argv[2:]))

    def __sendPropose(self,decree):
	leader = None
	decree = "Tag %f-%d "%(time.time(),self.__counter)+decree
	self.__counter += 1
	proposeTimeout = 0
	getLeaderTimeout = 0
	while 1:
	    now = time.time()
	    if getLeaderTimeout<now:
		getLeaderTimeout = now+TIMEOUT
		mm = message.Maker("GetLeader")
		mm.send(self.port,self.__priests)
	    if leader and proposeTimeout<now:
		proposeTimeout = now+TIMEOUT
		mm = message.Maker("Propose")
		mm.addDecree(decree)
		mm.send(self.port,leader)
	    m = message.getMessage(self.port.recv(TIMEOUT))
	    if m==None:
		continue
	    if m.command=="Redirect":
		if leader!=m.result:
		    leader = m.result
		    proposeTimeout = 0
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

Client(UdpMessenger(),sys.argv)
