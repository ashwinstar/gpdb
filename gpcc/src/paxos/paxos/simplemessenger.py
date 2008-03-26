import random
import threading
import time
from logger import Logger

DROP_CHANCE = 0.1
MAX_QUEUE_SIZE = 100
PORT_DOWN_CHANCE = .1
PORT_UPDOWN_TIME = 10

class Port:
    def __init__(self,messenger,address):
	self.__queueCv = threading.Condition()
	self.__messenger = messenger
	self.__address = address
	self.__queue = []
	self.__timeout = 0

    def log(self,s):
	self.__messenger.log(s)

    def getAddress(self):
	return self.__address

    def getMessenger(self):
	return self.__messenger

    def __checkDrop(self):
	now = time.time()
	if self.__timeout<now:
	    self.__up = random.random()>=PORT_DOWN_CHANCE
	    self.__timeout = now+random.random()*PORT_UPDOWN_TIME
	return not self.__up or random.random()<DROP_CHANCE

    def send(self,to,messag):
	self.log("from="+self.__address+" to="+to+" send "+messag)
	if not self.__checkDrop():
	    self.__messenger.send(self,to,messag)

    def recv(self,timeout,fro):
	self.__queueCv.acquire()
	try:
	    while len(self.__queue)==0:
		self.__queueCv.wait(timeout)
		if len(self.__queue)==0:
		    return None
	    o = self.__queue.pop()
	finally:
	    self.__queueCv.release()
	self.log("from="+o[1]+" to="+self.__address+" recv "+o[0])
	fro[0] = o[1]
	return o[0]

    def add(self,fro,messag):
	if not self.__checkDrop():
	    self.__queueCv.acquire()
	    try:
		if len(self.__queue)<MAX_QUEUE_SIZE:
		    self.__queue.append([messag,fro])
		    self.__queueCv.notify()
	    finally:
		self.__queueCv.release()

    def close(self):
	self.__messenger.remove(self)

class SimpleMessenger(Logger):

    def __init__(self):
	Logger.__init__(self,"Simple")
	self.__portsL = threading.Lock()
	self.__ports = dict()
	self.__mapL = threading.Lock()
	self.__map = dict()
	self.__counter = 0

    def remove(self,p):
	self.__portsL.acquire()
	try:
	    self.__ports.pop(p)
	finally:
	    self.__portsL.release()

    def makePort(self,address):
	p = Port(self,self.normalize(address))
	self.__portsL.acquire()
	try:
	    self.__ports[p.getAddress()] = p
	finally:
	    self.__portsL.release()
	return p

    def send(self,fro,to,messag):
	self.__portsL.acquire()
	try:
	    if to in self.__ports:
		self.__ports[to].add(fro.getAddress(),messag)
	finally:
	    self.__portsL.release()

    def normalize(self,address):
	self.__mapL.acquire()
	try:
	    if ':' not in address:
		address = "junk"+str(self.__counter)
	    if address not in self.__map:
		self.__map[address] = "Port"+str(self.__counter)
		self.__counter += 1
	    return self.__map[address]
	finally:
	    self.__mapL.release()
