import sys
import time

import logger
import message
import utils
from message import Message
from udpmessenger import UdpMessenger

TIMEOUT = .1
PING_TIMEOUT = .1
PROPOSE_TIMEOUT = 1
DEAD_TIMEOUT = 1
DRAW_TIMEOUT = 1

class Data:
    def __init__(self,address):
	self.dn = 0
	self.deadTime = 0
	self.address = address
	self.priestId = ""
	self.leader = ""

class Status:

    def __init__(self,messenger,argv):
	self.port = messenger.makePort("")
	self.__priests = utils.stringToAddresses(messenger,argv[1])

    def run(self):
	fro = [None]
	ps = set()
	pingTimeout = 0
	proposeTimeout = 0
	drawTimeout = 0
	decree = None
	dn = 0
	priests = dict([(i,Data(i)) for i in self.__priests])
	while 1:
	    now = time.time()
	    if drawTimeout<now:
		drawTimeout = now+DRAW_TIMEOUT
		print "\33[H\33[J"+time.ctime(now)
		print
		print "decreeNumber=%d"%dn
		print
		for i in sorted(priests.keys()):
		    d = priests[i]
		    sys.stdout.write("%s:"%i)
		    sys.stdout.write(" dn=%d"%abs(d.dn))
		    sys.stdout.write(d.deadTime<now and " down" or " up")
		    if d.priestId in ps:
			sys.stdout.write(" voting")
		    if d.leader:
			sys.stdout.write(" leader=%s"%d.leader)
		    print
	    if pingTimeout<now:
		pingTimeout = now+PING_TIMEOUT
		mm = message.Maker("Ping")
		mm.send(self.port,priests.keys())
	    if proposeTimeout<now:
		proposeTimeout = now+PROPOSE_TIMEOUT
		mm = message.Maker("Propose")
		decree = "Tag %d GetPriestSet"%now
		mm.addDecree(decree)
		mm.send(self.port,priests.keys())
	    messag = self.port.recv(TIMEOUT,fro)
	    if messag==None:
		continue
	    m = Message(messag,fro[0])
	    d = priests.setdefault(m.fro,Data(m.fro))
	    if m.command=="Ping":
		d.priestId = m.priestId
		d.dn = m.dn
		d.deadTime = time.time()+DEAD_TIMEOUT
		if m.result:
		    for i in m.result.split(','):
			priests.setdefault(i,Data(i))
	    elif m.command=="Redirect":
		d.leader = m.result
	    elif m.command=="Result":
		d.leader = m.fro
		if m.decree==decree:
		    dn = m.dn
		    if len(m.result)<=2:
			ps = set()
		    else:
			ps = set(m.result[2:].split(','))

Status(UdpMessenger(),sys.argv).run()
