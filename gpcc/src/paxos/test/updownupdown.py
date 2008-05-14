import fcntl
import os
import signal
import socket
import subprocess
import sys
import threading
import time

import constants
import message
import utils
from udpmessenger import UdpMessenger

TIMEOUT = 1
MAX_SLOWNESS = 10
WAIT_TIME = 300
NPRIEST = 5
CATCH_UP_SPEED = 20
SHOOT_INTERVAL = .05

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

class ShootShootShoot(threading.Thread):

    def __init__(self):
	threading.Thread.__init__(self)
	self.lock = threading.RLock()
	self.finished = False

    def run(self):
	port = messenger.makePort("")
	decree = None
	timeout = 0
	counter = 0
	while 1:
	    self.lock.acquire()
	    try:
		if self.finished:
		    break
	    finally:
		self.lock.release()
	    now = time.time()
	    if timeout<now:
		timeout = now+TIMEOUT
		decree = "Tag %d Stat /"%counter
		counter += 1
		mm = message.Maker("Propose")
		mm.addDecree(decree)
		mm.send(port,priestAddresses)
	    m = message.getMessage(port.recv(TIMEOUT))
	    if m:
		if m.command=="Result" and m.decree==decree:
		    timeout = now+SHOOT_INTERVAL

    def unstart(self):
	self.lock.acquire()
	try:
	    self.finished = True
	finally:
	    self.lock.release()
	self.join()

class Priest:

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
	self.killedTime = 0

    def start(self):
	now = time.time()
	if self.killedTime+1>now:
	    time.sleep(self.killedTime+1-now)
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
		time.sleep(.1)
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
	    time.sleep(.1)

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
	self.killedTime = time.time()

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

    def getStuff(self,checkSlowness):
	decree = None
	startTime = time.time()
	getLeaderTimeout = 0
	proposeTimeout = 0
	try:
	    while 1:
		now = time.time()
		if now-startTime>=WAIT_TIME:
		    print_line("ERROR Timed out")
		    global got_timeout
		    got_timeout = True
		    return False
		if getLeaderTimeout<now:
		    getLeaderTimeout = now+TIMEOUT
		    mm = message.Maker("GetLeader")
		    mm.send(self.port,priestAddresses)
		if self.leader and proposeTimeout<now:
		    proposeTimeout = now+TIMEOUT
		    decree = "Tag %d GetPriestSet"%self.counter
		    self.counter += 1
		    mm = message.Maker("Propose")
		    mm.addDecree(decree)
		    mm.send(self.port,self.leader)
		m = message.getMessage(self.port.recv(TIMEOUT))
		if m==None:
		    continue
		if m.command=="Redirect":
		    if self.leader!=m.result:
			self.leader = m.result
			proposeTimeout = 0
		elif m.command=="Result" and m.decree==decree:
		    if m.dn<=self.dn:
			print_line("ERROR DecreeNumber not monotone")
			not_monotone = True
		    self.priestSet = set(m.result[2:].split(','))
		    self.dn = m.dn
		    return True
	finally:
	    if checkSlowness:
		i = time.time()-startTime
		if i>MAX_SLOWNESS:
		    print_line("slowness=%f"%i)
		global slowness
		if i>slowness:
		    slowness = i

    def waitCatchUp1(self,index,timeout=WAIT_TIME):
	p = self.p[index]
	start = time.time()
	timeo = 0
	n = 0
	while 1:
	    now = time.time()
	    if start+timeout<now:
		return False
	    if timeo<now:
		timeo = now+TIMEOUT
		if self.getStuff(True):
		    print_dot()
		mm = message.Maker("Ping")
		mm.send(self.port,p.address)
	    m = message.getMessage(self.port.recv(TIMEOUT))
	    if m==None:
		continue
	    if m.command=="Ping" and m.fro==p.address:
		if m.dn<0:
		    n = 0
		if m.dn>=0:
		    n += 1
		    if n>=5:
			print_line("Priest %d has caught up. %f"%(p.index,now-start))
			return True

    def waitCatchUp(self,index,timeout=WAIT_TIME):
	if self.waitCatchUp1(index,timeout):
	    return True
	print_line("ERROR Timed out waiting for Priest %d to catch up."%index)
	global got_timeout
	got_timeout = True
	return False

    def wait(self,t):
	t += time.time()
	while time.time()<t:
	    time.sleep(1)
	    if self.getStuff(True):
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
	    if self.getStuff(True):
		print_dot()
	print_line("ERROR gave up waiting for: "+what)

    def waitFor(self,what,f,timeout=WAIT_TIME):
	self.waitFor2(what,lambda l,ps:f(ps),timeout)

    def waitForLeader(self,what,f,timeout=WAIT_TIME):
	self.waitFor2(what,lambda l,ps:f(l),timeout)

    def kill(self,index,timeout=WAIT_TIME):
	p = self.p[index]
	p.kill()
	self.waitFor("Priest %d is gone."%index,lambda x:p.id not in x,timeout)

    def stop(self,index,timeout=WAIT_TIME):
	p = self.p[index]
	p.stop()
	self.waitFor("Priest %d is gone."%index,lambda x:p.id not in x,timeout)

    def start(self,index,timeout=WAIT_TIME):
	p = self.p[index]
	p.start()
	self.waitFor("Priest %d has joined."%index,lambda x:p.id in x,timeout)

    def cont(self,index,timeout=WAIT_TIME):
	p = self.p[index]
	p.cont()
	self.waitFor("Priest %d has joined."%index,lambda x:p.id in x,timeout)

    def propose(self,n,checkSlowness=True):
	global ntest
	global passed
	ntest += 1
	start = self.dn
	while 1:
	    if not self.getStuff(checkSlowness):
		return
	    i = self.dn-start
	    print_line("decree %d/%d"%(i,n))
	    if i>=n:
		passed += 1
		return

    def __run1(self):
	global ntest
	global passed
	print_line("Starting the Priests")

	for i in self.p:
	    i.start()
	self.propose(1,False)

	allPriests = set([i.id for i in self.p])
	self.waitFor("All priests have joined.",lambda x:allPriests<=x)

	self.propose(constants.ALPHA)

	print_line("Checking if kill and restart works.")
	self.kill(0)
	self.start(0)

	print_line("Checking if stop and continue works.")
	self.stop(0)
	self.cont(0)

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
		    self.cont(j.index)
		    self.waitCatchUp(j.index)
	    self.cont(i.index)
	    self.waitCatchUp(i.index)

	print_line("Checking speed.")
	start = time.time()
	self.propose(1000)
	print_line("time1=%f"%(time.time()-start))
	self.stop(0)
	start = time.time()
	self.propose(1000)
	print_line("time2=%f"%(time.time()-start))
	self.p[0].cont()
	self.waitCatchUp(0,WAIT_TIME+1000/CATCH_UP_SPEED)
	self.waitFor("Priests 0 has joined.",lambda x:self.p[0].id in x)

	print_line("Kill priest while it is catching up.")
	self.kill(0)
	self.propose(1000)
	i = 1
	ntest += 1
	while i<WAIT_TIME+1000/CATCH_UP_SPEED:
	    self.p[0].start()
	    if self.waitCatchUp1(0,i):
		passed += 1
		break
	    self.p[0].kill()
	    self.wait(1)
	    i = (i+1)*1.2
	else:
	    print_line("ERROR Priest 0 taking too long to catch up.")
	self.waitFor("Priests 0 has joined.",lambda x:self.p[0].id in x)

	print_line("Checking if kill, get a bit behind, restart works.")
	self.kill(0)
	self.propose(constants.ACCEPTOR_HISTORY/8)
	self.stop(1)
	self.propose(constants.ACCEPTOR_HISTORY/8)
	self.p[1].cont()
	self.waitCatchUp(1,WAIT_TIME+constants.ACCEPTOR_HISTORY/8/CATCH_UP_SPEED)
	self.p[0].start()
	self.waitCatchUp(0,WAIT_TIME+constants.ACCEPTOR_HISTORY/4/CATCH_UP_SPEED)
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)
	self.propose(1)
	self.p[2].stop()
	self.p[3].stop()
	self.propose(1)
	self.p[2].cont()
	self.p[3].cont()
	self.waitCatchUp(2)
	self.waitCatchUp(3)
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)

	print_line("Checking if kill, get too far behind, restart works.")
	self.p[0].kill()
	self.p[1].stop()
	self.waitFor("Priest 0 and 1 are gone.",lambda x:self.p[0].id not in x and self.p[1].id not in x)
	self.propose(constants.ACCEPTOR_HISTORY+1)
	self.p[1].cont()
	self.waitCatchUp(1,WAIT_TIME+constants.ACCEPTOR_HISTORY/4/CATCH_UP_SPEED)
	self.p[0].start()
	self.waitCatchUp(0,WAIT_TIME+constants.ACCEPTOR_HISTORY/4/CATCH_UP_SPEED)
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)
	self.propose(1)
	self.p[2].stop()
	self.p[3].stop()
	self.propose(1)
	self.p[2].cont()
	self.p[3].cont()
	self.waitCatchUp(2)
	self.waitCatchUp(3)
	self.waitFor("All Priests have joined.",lambda x:allPriests<=x)

	print_line("Killing three, then restarting.")
	self.p[0].kill()
	self.p[1].kill()
	self.p[2].kill()
	self.p[0].start()
	self.p[1].start()
	self.p[2].start()
	self.propose(1,False)

	print_line("Killing all, then restarting.")
	for i in self.p:
	    i.kill()
	for i in self.p:
	    i.start()
	self.propose(1,False)

	print_line("Killing all Priests.")
	for i in self.p:
	    if i.started:
		i.kill()

    def run(self,n):
	global ntest
	global passed
	global slowness
	
	for i in xrange(n):
	    self.__run1()

	ntest += 1
	if not subprocess.call(['grep','Traceback','data/log']):
	    print_line("ERROR log has Traceback")
	else:
	    passed += 1

	ntest += 1
	if not subprocess.call(['sh','-c',"grep crc= data/log|cut -d ' ' -f 3-|sort|uniq|cut -d ' ' -f 1|uniq -d|grep ."]):
	    print_line("ERROR bad crc")
	else:
	    passed += 1

	ntest += 1
	print_line("slowness=%f"%slowness)
	if slowness>MAX_SLOWNESS:
	    print_line("ERROR too slow")
	else:
	    passed += 1

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

starttime = time.time()

shootshootshoot = ShootShootShoot()

shootshootshoot.start()

try:
    UpDownUpDown().run(len(sys.argv)<2 and 1 or int(sys.argv[1]))
except KeyboardInterrupt:
    utils.killPython()

shootshootshoot.unstart()

print_line("%d/%d passed %f"%(passed,ntest,time.time()-starttime))
