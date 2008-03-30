import fcntl
import os
import signal
import socket
import sys
import threading
import time

import constants
import message
import utils
from message import Message
from udpmessenger import UdpMessenger

TIMEOUT = 1
MAX_SLOWNESS = 10
WAIT_TIME = 60
NPRIEST = 5

ntest = 0
passed = 0

slowness = 0
not_monotone = False

host = socket.gethostbyname(socket.gethostname())

messenger = UdpMessenger()
messenger.setDisableLog(True)

lock = threading.Lock()

priestAddresses = ["%s:%d"%(host,2000+i) for i in xrange(NPRIEST)]

priestSet = set()
priestSetDn = 0

printNeedNl = False

def print_line(s):
    global printNeedNl
    lock.acquire()
    try:
	if printNeedNl:
	    printNeedNl = False
	    print
	print s
    finally:
	lock.release()

def print_dot():
    global printNeedNl
    lock.acquire()
    try:
	sys.stdout.write('.')
	printNeedNl = True
    finally:
	lock.release()

class Super:

    def __init__(self):
	self.leader = None
	self.counter = 0
	self.port = messenger.makePort("")
	self.dn = 0

    def getStuff(self,wantDn,wantPs):
	fro = [None]
	decree = None
	startTime = time.time()
	timeo = 0
	while wantDn or wantPs:
	    now = time.time()
	    lock.acquire()
	    try:
		global slowness
		if now-startTime>slowness:
		    slowness = now-startTime
	    finally:
		lock.release()
	    if timeo<now:
		timeo = now+TIMEOUT
		if self.leader==None:
		    mm = message.Maker("GetLeader")
		    mm.send(self.port,priestAddresses)
		else:
		    if wantPs:
			mm = message.Maker("GetPriestSet")
			mm.send(self.port,self.leader)
		    if wantDn:
			decree = "Tag %d Stat paxos/decreeNumber"%self.counter
			self.counter += 1
			mm = message.Maker("Propose")
			mm.addDecree(decree)
			mm.send(self.port,self.leader)
	    messag = self.port.recv(TIMEOUT,fro)
	    if messag==None:
		self.leader = None
		continue
	    m = Message(messag,fro[0])
	    if m.command=="Redirect":
		self.leader = m.result
		timeo = 0
	    elif wantPs and m.command=="PriestSet":
		wantPs = False
		lock.acquire()
		try:
		    global priestSet
		    global priestSetDn
		    if priestSetDn<m.dn:
			priestSet = set(m.result.split(','))
			priestSetDn = m.dn
		finally:
		    lock.release()
	    elif wantDn and m.command=="Result" and m.decree==decree:
		wantDn = False
		i = int(m.result[:m.result.index(' ')])
		if i<=self.dn:
		    print_line("ERROR DecreeNumber not monotone")
		    lock.acquire()
		    try:
			not_monotone = True
		    finally:
			lock.release()
		self.dn = i

class ShootShootShoot(threading.Thread,Super):

    def __init__(self):
	threading.Thread.__init__(self)
	Super.__init__(self)
	self.done = False

    def run(self):
	while 1:
	    lock.acquire()
	    try:
		if self.done:
		    break
	    finally:
		lock.release()
	    self.getStuff(True,True)
	    print_dot()
	    time.sleep(1)

shoot = ShootShootShoot()
shoot.start()

class Priest():

    def __init__(self,index,start):
	self.index = index
	self.dir = "data/Priest-%d"%index
	os.mkdir(self.dir)
	self.address = priestAddresses[index]
	self.port = int(self.address[self.address.index(':')+1:])
	f = open(self.dir+"/configuration","w")
	f.write("host=%s\n"%host)
	f.write("port=%d\n"%self.port)
	f.write("priests=%s\n"%",".join(priestAddresses))
	f.close()
	os.mkdir(self.dir+"/data")
	if start:
	    open(self.dir+"/data/start","w").close()
	self.id = None
	self.started = False
	self.stopped = False
	self.pid = None

    def start(self):
	print_line("starting Priest %d"%self.index)
	assert not self.started
	assert not self.stopped
	self.started = True
	os.system("cd %s;python ../../bin/main.pyc Priest&"%self.dir)
	if not self.id:
	    while 1:
		try:
		    f = open(self.dir+"/data/acceptor-id","r")
		    i = f.read()
		    f.close()
		    if '\n' in i:
			self.id = i.strip('\r\n"')
			break
		except IOError:
		    pass
	while 1:
	    try:
		f = open(self.dir+"/pid","r")
		i = f.read()
		f.close()
		if '\n' in i:
		    self.pid = int(i)
		    break
	    except IOError:
		pass

    def kill(self):
	print_line("SIGKILL Priest %d"%self.index)
	assert self.started
	self.started = False
	self.stopped = False
	os.kill(self.pid,signal.SIGKILL)
	self.pid = None
	f = open(self.dir+"/pid","r+")
	fcntl.lockf(f,fcntl.LOCK_EX)
	f.close()
	os.remove(self.dir+"/pid")

    def stop(self):
	print_line("SIGSTOP Priest %d"%self.index)
	assert self.started
	assert not self.stopped
	self.stopped = True
	os.kill(self.pid,signal.SIGSTOP)

    def cont(self):
	print_line("SIGCONT Priest %d"%self.index)
	assert self.started
	assert self.stopped
	self.stopped = False
	os.kill(self.pid,signal.SIGCONT)

class UpDownUpDown(Super):

    def __init__(self):
	Super.__init__(self)
	os.system("rm -rf data")
	os.makedirs("data")
	self.p = [Priest(i,i==0) for i in xrange(NPRIEST)]

    def waitFor(self,what,f):
	global ntest
	global passed
	ntest += 1
	timeout = time.time()+WAIT_TIME
	while time.time()<timeout:
	    lock.acquire()
	    try:
		p = priestSet
	    finally:
		lock.release()
	    if f(p):
		print_line(what)
		passed += 1
		return
	    time.sleep(1)
	print_line("ERROR gave up waiting for: "+what)

    def kill(self,p):
	p.kill()
	self.waitFor("Priest %d is gone."%p.index,lambda x:p.id not in x)

    def stop(self,p):
	p.stop()
	self.waitFor("Priest %d is gone."%p.index,lambda x:p.id not in x)

    def start(self,p):
	p.start()
	self.waitFor("Priest %d has joined."%p.index,lambda x:p.id in x)

    def cont(self,p):
	p.cont()
	self.waitFor("Priest %d has joined."%p.index,lambda x:p.id in x)

    def propose(self,n):
	self.getStuff(True,False)
	start = self.dn
	while 1:
	    self.getStuff(True,False)
	    i = self.dn-start
	    print_line("decree %d/%d"%(i,n))
	    if i>=n:
		break

    def run(self):
	print_line("Starting the Priests")
	for i in self.p:
	    i.start()
	allPriests = set([i.id for i in self.p])
	self.waitFor("All priests have joined.",lambda x:allPriests<=x)

	print_line("Checking if kill and restart works.")
	self.kill(self.p[0])
	self.start(self.p[0])

	print_line("Checking if stop and continue works.")
	self.stop(self.p[0])
	self.cont(self.p[0])

	print_line("Killing two at the same time.")
	self.p[0].kill()
	self.p[1].kill()
	self.waitFor("Priest 0 and 1 are gone.",lambda x:self.p[0].id not in x and self.p[1].id not in x)
	self.p[0].start()
	self.p[1].start()
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)

	print_line("Killing two, then one.")
	self.p[0].kill()
	self.p[1].kill()
	self.waitFor("Priest 0 and 1 are gone.",lambda x:self.p[0].id not in x and self.p[1].id not in x)
	self.propose(constants.ALPHA)
	self.p[2].kill()
	self.propose(1)
	self.p[0].start()
	self.p[1].start()
	self.p[2].start()
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)

	print_line("Checking if kill, get too far behind, restart works.")
	self.kill(self.p[0])
	self.stop(self.p[1])
	self.propose(constants.ACCEPTOR_HISTORY)
	self.cont(self.p[1])
	self.start(self.p[0])

	lock.acquire()
	shoot.done = True
	lock.release()
	shoot.join()

	print_line("Killing all Priests.")
	for i in self.p:
	    if i.started:
		i.kill()

UpDownUpDown().run()

ntest += 1
print_line("slowness=%d"%slowness)
if slowness>MAX_SLOWNESS:
    print_line("too slow")
else:
    passed += 1
ntest += 1
if not_monotone:
    print_line("not_monotone failed")
else:
    passed += 1

print_line("%d/%d passed"%(passed,ntest))

# Python has trouble exiting, so we kill ourself.
os.kill(os.getpid(),signal.SIGKILL)
