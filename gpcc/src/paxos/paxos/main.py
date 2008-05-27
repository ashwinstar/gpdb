import fcntl
import os
import signal
import socket
import sys
import time

import utils

from configuration import Configuration
from priest import Priest
from simplemessenger import SimpleMessenger
from udpmessenger import UdpMessenger
from tester import Tester

def test(argv):
    npriest = 1
    ntester = 0
    simple_messenger = False
    verbose = False
    port = 2000
    while argv:
	opt = argv.pop(0)
	if opt=="-npriest":
	    npriest = int(argv.pop(0))
	elif opt=="-ntester":
	    ntester = int(argv.pop(0))
	elif opt=="-simple-messenger":
	    simple_messenger = True
	elif opt=="-verbose":
	    verbose = True
	elif opt=="-port":
	    port = int(argv.pop(0))
	else:
	    usage()
    m = simple_messenger and SimpleMessenger() or UdpMessenger()
    if verbose:
	m.setDisableLog(False)
    priests = []
    hostname = socket.gethostbyname(socket.gethostname())
    for i in xrange(npriest):
	priests.append(hostname+":"+str(port+i))
    priests = ",".join(priests)
    for i in xrange(npriest):
	sb = ["priests="+priests]
	sb.append("\nhost=%s\n"%hostname)
	sb.append("port=%d\n"%(port+i))
	config = Configuration("".join(sb))
	f = "Priest-%d/data"%i
	try:
	    os.makedirs(f)
	except OSError:
	    pass
	Priest(config,m,f).start()
    for i in xrange(ntester):
	Tester(utils.stringToAddresses(m,priests),m).start()
    while 1:
	time.sleep(1)

def lock():
    global lockfile
    if not os.path.exists("pid"):
	lockfile = open("pid","w+")
    else:
	lockfile = open("pid","r+")
    try:
	fcntl.lockf(lockfile,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except IOError:
	sys.stderr.write("Can not lock.\n")
	sys.exit(1)
    lockfile.truncate(0)
    lockfile.write(str(os.getpid())+'\n')
    lockfile.flush()
    ps = open("../log","a",1)
    sys.stdout = ps
    sys.stderr = ps

def runPriest(argv):
    lock()
    f = open("configuration","r")
    s = f.read()
    f.close()
    config = Configuration(s)
    try:
	os.mkdir("data")
    except OSError,e:
	if e.errno!=17:
	    raise
    m = UdpMessenger()
#    m.setDisableLog(False)
    Priest(config,m,"data").run()

def runTester(argv):
    if len(argv)<1:
	usage()
    lock()
    messenger = UdpMessenger()
    priests = utils.stringToAddresses(messenger,argv[0])
    Tester(priests,messenger).run()

def main(argv):
    if len(argv)==0:
	usage()
    if argv[0]=="test":
	test(argv[1:])
    elif argv[0]=="Priest":
	runPriest(argv[1:])
    elif argv[0]=="Tester":
	runTester(argv[1:])
    else:
	usage()

def usage():
    sys.stderr.write("usage:\n")
    sys.stderr.write("    python "+sys.argv[0]+" test [-npriest n] [-ntester n] [-verbose] [-simple_messenger]\n")
    sys.stderr.write("    python "+sys.argv[0]+" Priest\n")
    sys.stderr.write("    python "+sys.argv[0]+" Tester addresses\n")
    sys.exit(1)

try:
    main(sys.argv[1:])
except KeyboardInterrupt:
    utils.killPython()
