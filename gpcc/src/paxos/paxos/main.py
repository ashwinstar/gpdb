import fcntl
import os
import socket
import sys

import utils

from client import Client
from configuration import Configuration
from priest import Priest
from simplemessenger import SimpleMessenger
from udpmessenger import UdpMessenger
from tester import Tester

BASE_PORT = 2000
N_TESTER = 0
N_PRIEST = 3

def test():
    m = UdpMessenger()
#    m = SimpleMessenger()
    m.setDisableLog(True)
    priests = []
    hostname = socket.gethostbyname(socket.gethostname())
    for i in xrange(N_PRIEST):
	priests.append(hostname+":"+str(BASE_PORT+i))
    priests = ",".join(priests)
    for i in xrange(N_PRIEST):
	sb = ["priests="+priests]
	sb.append("\nhost=%s\n"%hostname)
	sb.append("port=%d\n"%(BASE_PORT+i))
	config = Configuration("".join(sb))
	f = "Priest-%d/data"%i
	try:
	    os.makedirs(f)
	except OSError,e:
	    if e.errno!=17:
		raise
	Priest(config,m,f).start()
    for i in xrange(N_TESTER):
	Tester(utils.stringToAddresses(m,priests),m).start()

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
    lockfile.write(str(os.getpid()))
    lockfile.flush()
    ps = open("../log","a")
    sys.stdout = ps
    sys.stderr = ps

def runClient(argv):
    Client(UdpMessenger(),argv)

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
    Priest(config,UdpMessenger(),"data").start()

def runTester(argv):
    lock();
    messenger = UdpMessenger()
    priests = utils.stringToAddresses(messenger,argv[1])
    Tester(priests,messenger).start()

def main(argv):
    argv0 = argv.pop(0)
    if len(argv)==0:
	usage(argv0)
    if argv[0]=="test":
	test()
    elif argv[0]=="Client" and len(argv)>=2:
	runClient(argv)
    elif argv[0]=="Priest":
	runPriest(argv)
    elif argv[0]=="Tester" and len(argv)>=2:
	runTester(argv)
    else:
	usage(argv0)

def usage(argv0):
    sys.stderr.write("usage:\n")
    sys.stderr.write("    python "+argv0+" test\n");
    sys.stderr.write("    python "+argv0+" Client addresses [decree]\n");
    sys.stderr.write("    python "+argv0+" Priest\n");
    sys.stderr.write("    python "+argv0+" Tester addresses\n");
    sys.exit(1)

main(sys.argv)
