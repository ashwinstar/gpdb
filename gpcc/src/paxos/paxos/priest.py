from acceptor import Acceptor
from learner import Learner
from message import Message
from pinger import Pinger
from proposer import Proposer
from role import Role

TICK = .1

class Priest(Role):

    def __init__(self,configuration,messenger,dataDir):
	Role.__init__(self,messenger.makePort(configuration.host+":%d"%configuration.port))
	self.configuration = configuration
	self.dataDir = dataDir
	self.pinger = Pinger(self)
	self.acceptor = Acceptor(self)
	self.learner = Learner(self)
	self.proposer = Proposer(self)

    def __handleMessages(self,timeout):
	fro = [None]
	messag = self.port.recv(timeout,fro)
	if not messag:
	    return False
	decreeNumber = self.learner.getDecreeNumber()
	m = Message(messag,fro[0])
	self.pinger.handleMessage(decreeNumber,m)
	self.proposer.handleMessage(decreeNumber,m)
	self.acceptor.handleMessage(decreeNumber,m)
	self.learner.handleMessage(decreeNumber,m)
	return True

    def work(self):
	while self.__handleMessages(0):
	    pass
	self.acceptor.sync()
	self.__handleMessages(TICK)
	decreeNumber = self.learner.getDecreeNumber()
	self.pinger.tick(decreeNumber)
	self.acceptor.tick(decreeNumber)
	self.proposer.tick(decreeNumber)
