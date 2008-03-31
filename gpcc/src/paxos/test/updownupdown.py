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
WAIT_TIME = 100
NPRIEST = 5

ntest = 0
passed = 0

slowness = 0
not_monotone = False
got_timeout = False

host = socket.gethostbyname(socket.gethostname())

messenger = UdpMessenger()
messenger.setDisableLog(True)

priestAddresses = ["%s:%d"%(host,2000+i) for i in xrange(NPRIEST)]

printNeedNl = False

def print_line(s):
    global printNeedNl
    if printNeedNl:
	printNeedNl = False
	print
    print s

def print_dot():
    sys.stdout.write('.')
    global printNeedNl
    printNeedNl = True

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

class UpDownUpDown:

    def __init__(self):
	self.leader = None
	self.counter = 0
	self.port = messenger.makePort("")
	self.dn = 0
	self.priestSet = set()
	os.system("rm -rf data")
	os.makedirs("data")
	self.p = [Priest(i,i==0) for i in xrange(NPRIEST)]

    def waitCatchUp(self,p):
	fro = [None]
	bigtimeout = time.time()+WAIT_TIME
	timeo = 0
	while 1:
	    now = time.time()
	    if bigtimeout<now:
		print_line("ERROR Timed out waiting for %s to catch up."%p.address)
		global got_timeout
		got_timeout = True
		return False
	    if timeo<now:
		timeo = now+TIMEOUT
		mm = message.Maker("Ping")
		mm.send(self.port,p.address)
	    messag = self.port.recv(TIMEOUT,fro)
	    if messag==None:
		continue
	    m = Message(messag,fro[0])
	    if m.command=="Ping" and m.fro==p.address and m.dn>=0:
		print_line("%s has caught up."%p.address)
		return True

    def getStuff(self):
	fro = [None]
	decree = None
	startTime = time.time()
	timeo = 0
	while 1:
	    now = time.time()
	    if now-startTime>=WAIT_TIME:
		print_line("ERROR Timed out")
		global got_timeout
		got_timeout = True
		return False
	    global slowness
	    if now-startTime>slowness:
		slowness = now-startTime
	    if timeo<now:
		timeo = now+TIMEOUT
		if self.leader==None:
		    mm = message.Maker("GetLeader")
		    mm.send(self.port,priestAddresses)
		else:
		    decree = "Tag %d GetPriestSet"%self.counter
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
	    elif m.command=="Result" and m.decree==decree:
		if m.dn<=self.dn:
		    print_line("ERROR DecreeNumber not monotone")
		    not_monotone = True
		self.priestSet = set(m.result[2:].split(','))
		self.dn = m.dn
		return True

    def wait(self,t):
	t += time.time()
	while time.time()<t:
	    time.sleep(1)
	    if self.getStuff():
		print_dot()

    def waitFor2(self,what,f,timeout=WAIT_TIME):
	global ntest
	global passed
	ntest += 1
	start = time.time()
	while 1:
	    now = time.time();
	    if start+timeout<=now:
		break
	    if f(self.leader,self.priestSet):
		print_line(what+" %f"%(time.time()-start))
		passed += 1
		return
	    time.sleep(1)
	    if self.getStuff():
		print_dot()
	print_line("ERROR gave up waiting for: "+what)

    def waitFor(self,what,f,timeout=WAIT_TIME):
	self.waitFor2(what,lambda l,ps:f(ps),timeout)

    def waitForLeader(self,what,f,timeout=WAIT_TIME):
	self.waitFor2(what,lambda l,ps:f(l),timeout)

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
	global ntest
	global passed
	ntest += 1
	start = self.dn
	while 1:
	    if not self.getStuff():
		return
	    i = self.dn-start
	    print_line("decree %d/%d"%(i,n))
	    if i>=n:
		passed += 1
		return

    def run(self):
	global ntest
	global passed
	print_line("Starting the Priests")
	for i in self.p:
	    i.start()
	allPriests = set([i.id for i in self.p])
	self.waitFor("All priests have joined.",lambda x:allPriests<=x)

	self.propose(constants.ALPHA)

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

	print_line("Killing pairs.")
	for i in self.p:
	    i.stop()
	    for j in self.p:
		if j!=i:
		    j.stop()
		    self.propose(1)
		    j.cont()
		    self.wait(3)
		    self.waitCatchUp(j)
		    self.wait(3)
	    i.cont()
	    self.wait(3)
	    self.waitCatchUp(i)
	    self.wait(3)

	print_line("Checking speed.")
	start = time.time()
	self.propose(1000)
	print_line("time1=%f"%(time.time()-start))
	self.stop(self.p[0])
	start = time.time()
	self.propose(1000)
	print_line("time2=%f"%(time.time()-start))
	self.p[0].cont()
	self.waitForLeader("Priest 0 is leader.",lambda l:l==self.p[0].address,100)

	print_line("Kill priest while it is catching up.")
	self.kill(self.p[0])
	self.propose(1000)
	for i in range(2,20,2):
	    self.p[0].start()
	    self.wait(i)
	    self.kill(self.p[0])
	self.p[0].start()
	self.waitForLeader("Priest 0 is leader.",lambda l:l==self.p[0].address,100)

	print_line("Checking if kill, get a bit behind, restart works.")
	self.kill(self.p[0])
	self.propose(constants.ACCEPTOR_HISTORY/8)
	self.stop(self.p[1])
	self.propose(constants.ACCEPTOR_HISTORY/8)
	self.p[1].cont()
	self.waitForLeader("Priest 1 is leader.",lambda l:l==self.p[1].address,constants.ACCEPTOR_HISTORY/8/20)
	self.p[0].start()
	self.waitForLeader("Priest 0 is leader.",lambda l:l==self.p[0].address,constants.ACCEPTOR_HISTORY/4/20)
	self.propose(1)
	self.p[2].stop()
	self.propose(1)
	self.p[2].cont()
	self.wait(3)
	self.waitCatchUp(self.p[2])
	self.wait(3)

	print_line("Checking if kill, get too far behind, restart works.")
	self.kill(self.p[0])
	self.stop(self.p[1])
	self.propose(constants.ACCEPTOR_HISTORY)
	self.p[1].cont()
	self.waitForLeader("Priest 1 is leader.",lambda l:l==self.p[1].address,constants.ACCEPTOR_HISTORY/4/20)
	self.p[0].start()
	self.waitForLeader("Priest 0 is leader.",lambda l:l==self.p[0].address,constants.ACCEPTOR_HISTORY/4/20)
	self.propose(1)
	self.p[2].stop()
	self.propose(1)
	self.p[2].cont()
	self.wait(3)
	self.waitCatchUp(self.p[2])
	self.wait(3)

	ntest += 1
	print_line("slowness=%d"%slowness)
	if slowness>MAX_SLOWNESS:
	    print_line("ERROR too slow")
	else:
	    passed += 1

	print_line("Killing three, then restarting.")
	self.p[0].kill()
	self.p[1].kill()
	self.p[2].kill()
	self.p[0].start()
	self.p[1].start()
	self.p[2].start()
	self.propose(1)

	print_line("Killing all, then restarting.")
	for i in self.p:
	    i.kill()
	for i in self.p:
	    i.start()
	self.propose(1)

	print_line("Killing all Priests.")
	for i in self.p:
	    if i.started:
		i.kill()

	ntest += 1
	if not_monotone:
	    print_line("ERROR not_monotone failed")
	else:
	    passed += 1

	ntest += 1
	if got_timeout:
	    print_line("ERROR got_timeout failed")
	else:
	    passed += 1

UpDownUpDown().run()

print_line("%d/%d passed"%(passed,ntest))
