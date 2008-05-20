import os
import socket
import sys
import time

import chubby
from chubby import Chubby

ntest = 0
passed = 0

CHUNK_SIZE = 30000

x=0

def check(__a,__b):
    global x
    global ntest
    global passed
    ntest += 1
    print "%s\t%s"%(prefix,__a)
    x = eval(__a)
    if eval(__b):
	print "%s\t\t%s"%(prefix,str(x))
	passed += 1
    else:
	print "%s\t\tBAD %s good %s"%(prefix,str(x),__b)

def test1():
    global global_l
    global_l = c.getLease("test/lock2",20,10)
    check("global_l","global_l[0]==0")
    if global_l[0]==0:
	global_l = global_l[1]
	for i in xrange(10):
	    check("global_l.renew(10)","True")
	    check("global_l.getLeaseExpireTime()","x[0]==0 and time.time()+6<x[1]<time.time()+10")
	    time.sleep(6)
    time.sleep(10)
    c.getLease("test/lock3",20,10)
    c.put("test/test1result","%d/%d"%(passed,ntest))

def test2(fn,dn):
    global global_l
    global_l = c.getLeaseFilenameDecreeNumberConnection(fn,dn)
    check("global_l.list('/')","x[0]==0")
    time.sleep(20)
    check("global_l.list('/')","x[0]==chubby.E_LEASEEXPIRED")
    c.put("test/test2result","%d/%d"%(passed,ntest))

def test0():
    global global_a
    global global_l
    check("c.put('/test/file','contents')","x==0")
    check("c.get('/test/file')","x[0]==0 and x[2]=='contents'")
    check("c.move('/test/file','/test/tofile')","x==0")
    check("c.stat('test/file')","x[0]==chubby.E_NOENT")
    check("c.get('/test/tofile')","x[0]==0 and x[2]=='contents'")
    check("c.list('/')","x[0]==0 and 'paxos' in x[2:] and 'test' in x[2:]")
    check("c.list('noentfile')","x==(chubby.E_NOENT,)")
    check("c.stat('test/tofile')","x[0]==0")
    check("c.remove('//test//1/1')","x==0 or x==chubby.E_NOENT")
    check("c.put('test///1/1','')","x==0")
    check("c.stat('test/1//1')","x[0]==0")
    dn = x[1]
    check("c.stat('test/1')","x[0:2]==(0,%d)"%dn)
    check("c.stat('test')","x[0:2]==(0,%d)"%dn)
    check("c.remove('//test//1/1')","x==0")
    check("c.stat('test')","x[0]==0 and x[1]>%d"%dn)
    check("c.list('/test')","'1' not in x")
    global_a = CHUNK_SIZE*'!'+"".join(map(str,xrange(0,CHUNK_SIZE)))
    check("c.putBigFile('///test/bi g',global_a)","x==0")
    check("c.get('/test//bi g')","x[0]==0 and x[2]==global_a")
    check("c.remove('/test/bi g')","x==0")
    check("c.list('/test')","'bi g' not in x")
    check("c.remove('/test/lock')","x==0 or x==chubby.E_NOENT")
    check("c.remove('/test/lock-info')","x==0 or x==chubby.E_NOENT")
    check("c.remove('/test/lock2')","x==0 or x==chubby.E_NOENT")
    check("c.remove('/test/lock2-info')","x==0 or x==chubby.E_NOENT")
    check("c.remove('/test/lock3')","x==0 or x==chubby.E_NOENT")
    check("c.remove('/test/lock3-info')","x==0 or x==chubby.E_NOENT")
    check("c.put('/test/test1result','no')","x==0")
    check("c.put('/test/test2result','no')","x==0")
    global_l = c.getLease('/test//lock',20,10)
    check("global_l","global_l[0]==0")
    if global_l[0]==0:
	global_l = global_l[1]
	global_l = global_l.clone()
	check("global_l","global_l[0]==0")
	if global_l[0]==0:
	    global_l = global_l[1]
	    check("global_l.getLeaseFilenameAndDecreeNumber()","1")
	    check("global_l.checkLease()","x==0")
	    check("global_l.list('/')","x[0]==0")
	    global_l.giveUpLease()
    global_l = c.getLease('/test//lock',20,10)
    check("global_l","global_l[0]==0")
    if global_l[0]==0:
	global_l = global_l[1]
	check("global_l.getLeaseFilenameAndDecreeNumber()","1")
	check("global_l.checkLease()","x==0")
	check("global_l.list('/')","x[0]==0")
    check("c.getLease('test/lock',2,10)","x==(chubby.E_TIMEOUT,)")
    os.system("python -u %s test1&"%sys.argv[0])
    time.sleep(10)
    check("c.getLease('test/lock2',20,10)","x==(chubby.E_TIMEOUT,)")
    check("c.getLease('test/lock2',60,10)","x[0]==0")
    global_l = c.getLease('test/lock3',20,10)
    check("global_l","global_l[0]==0")
    if global_l[0]==0:
	global_l = global_l[1]
	check("global_l.checkLease()","x==0")
	check("global_l.list('/')","x[0]==0")
	time.sleep(20)
	check("global_l.checkLease()","x==chubby.E_LEASEEXPIRED")
	check("global_l.list('/')","x==(chubby.E_LEASEEXPIRED,)")
    global_l = c.getLease('test/lock',20,10)
    check("global_l","global_l[0]==0")
    if global_l[0]==0:
	global_l = global_l[1]
	rc,fn,dn = global_l.getLeaseFilenameAndDecreeNumber()
	os.system("python -u %s test2 %s %d&"%(sys.argv[0],fn,dn))
    check("c.getLease('test/lock',20,10)","x[0]==0")
    check("c.get('/test/test1result')","x[0]==0 and x[2]=='21/21'")
    time.sleep(20)
    check("c.get('/test/test2result')","x[0]==0 and x[2]=='2/2'")

hostname = socket.gethostbyname(socket.gethostname())
chu = Chubby(hostname+":2000,"+hostname+":2001,"+hostname+":2002")
c = chu.makeConnection()

if len(sys.argv)>1 and sys.argv[1]=="test1":
    prefix = "test1"
    test1()
elif len(sys.argv)>3 and sys.argv[1]=="test2":
    prefix = "test2"
    test2(sys.argv[2],int(sys.argv[3]))
else:
    prefix = ""
    test0()
    print "%d/%d passed"%(passed,ntest)
