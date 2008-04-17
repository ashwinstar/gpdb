import threading
import time

import message

from acceptor import Acceptor
from learner import Learner
from pinger import Pinger
from proposer import Proposer
from role import Role

TICK = .01

class Priest(Role,threading.Thread):

    def __init__(self,configuration,messenger,dataDir):
	Role.__init__(self,messenger.makePort(configuration.host+":%d"%configuration.port))
	threading.Thread.__init__(self)
	self.configuration = configuration
	self.dataDir = dataDir
	self.lock = threading.RLock()
	self.lock.acquire()
	try:
	    self.pinger = Pinger(self)
	    self.acceptor = Acceptor(self)
	    self.learner = Learner(self)
	    self.proposer = Proposer(self)
	finally:
	    self.lock.release()

    def work(self):
	ticktime = 0
	while 1:
	    m = message.getMessage(self.port.recv(TICK))
	    if m:
		self.lock.acquire()
		try:
		    decreeNumber = self.learner.getDecreeNumber()
		    self.pinger.handleMessage(decreeNumber,m)
		    self.proposer.handleMessage(decreeNumber,m)
		    self.acceptor.handleMessage(decreeNumber,m)
		    self.learner.handleMessage(decreeNumber,m)
		finally:
		    self.lock.release()
	    now = time.time()
	    if ticktime<now:
		ticktime = now+TICK
		self.lock.acquire()
		try:
		    self.acceptor.sync()
		    decreeNumber = self.learner.getDecreeNumber()
		    self.pinger.tick(decreeNumber)
		    self.acceptor.tick(decreeNumber)
		    self.proposer.tick(decreeNumber)
		finally:
		    self.lock.release()
