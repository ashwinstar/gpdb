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
    print __a
    x = eval(__a)
    if eval(__b):
	print '\t',x
	passed += 1
    else:
	print '\tBAD',x,'good',__b

hostname = socket.gethostbyname(socket.gethostname())
chu = Chubby(hostname+":2000,"+hostname+":2001,"+hostname+":2002")
c = chu.makeConnection()

if len(sys.argv)>1 and sys.argv[1]=="test1":
    test1_l = c.lock("test/lock2",10,5)
    check("test1_l","test1_l[0]==0")
    test1_l = test1_l[1]
    for i in xrange(10):
	check("test1_l.renew(5)","True")
	check("test1_l.getLockExpireTime()","time.time()+3<x<time.time()+5")
	time.sleep(3)
    time.sleep(5)
    test1_l = c.lock("test/lock3",10,5)
    c.put("test/test1result","%d/%d"%(passed,ntest))
    sys.exit(1)

if len(sys.argv)>2 and sys.argv[1]=="test2":
    test2_l = c.getLockStringConnection(sys.argv[2])
    check("test2_l.list('/')","x[0]==0")
    time.sleep(10)
    check("test2_l.list('/')","x[0]==chubby.E_LOCKEXPIRED")
    c.put("test/test2result","%d/%d"%(passed,ntest))
    sys.exit(1)

check("c.put('/test/file','contents')","x==0")
check("c.get('/test/file')","x[0]==0 and x[2]=='contents'")
check("c.list('/')","x[0]==0 and x[2:]==('paxos','test')")
check("c.list('noentfile')","x==(chubby.E_NOENT,)")
check("c.stat('test/file')","x[0]==0")
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
l = c.lock('/test//lock',10,5)
check("l","l[0]==0")
l = l[1]
check("l.getLockString()","1")
check("l.isLockValid()","True")
check("l.list('/')","x[0]==0")
check("c.lock('test/lock',1,5)","x==(chubby.E_TIMEOUT,)")
os.system("python %s test1&"%sys.argv[0])
time.sleep(5)
check("c.lock('test/lock2',10,5)","x==(chubby.E_TIMEOUT,)")
check("c.lock('test/lock2',30,5)","x[0]==0")
l = c.lock('test/lock3',10,5)
check("l","l[0]==0")
l = l[1]
check("l.isLockValid()","x==True")
check("l.list('/')","x[0]==0")
time.sleep(10)
check("l.isLockValid()","x==False")
check("l.list('/')","x==(chubby.E_LOCKEXPIRED,)")
l = c.lock('test/lock',10,5)
check("l","l[0]==0")
l = l[1]
os.system("python %s test2 %s&"%(sys.argv[0],l.getLockString()))
check("c.lock('test/lock',10,5)","x[0]==0")
check("c.get('/test/test1result')","x[0]==0 and x[2]=='21/21'")
time.sleep(10)
check("c.get('/test/test2result')","x[0]==0 and x[2]=='2/2'")

print "%d/%d passed"%(passed,ntest)
