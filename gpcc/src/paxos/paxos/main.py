import atexit
import fcntl
import os
import signal
import socket
import sys

import utils

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
    m.setDisableLog(False)
    priests = []
    hostname = socket.gethostbyname(socket.gethostname())
    for i in xrange(N_PRIEST):
	priests.append(hostname+":"+str(BASE_PORT+i))
    priests = ",".join(priests)
    t = []
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
	t.append(Priest(config,m,f))
    for i in xrange(N_TESTER):
	t.append(Tester(utils.stringToAddresses(m,priests),m))
    for i in t[0:]:
	i.start()
    t[0].run()

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
    lock()
    messenger = UdpMessenger()
    priests = utils.stringToAddresses(messenger,argv[1])
    Tester(priests,messenger).run()

def main(argv):
    argv0 = argv.pop(0)
    if len(argv)==0:
	usage(argv0)
    if argv[0]=="test":
	test()
    elif argv[0]=="Priest":
	runPriest(argv)
    elif argv[0]=="Tester" and len(argv)>=2:
	runTester(argv)
    else:
	usage(argv0)

def usage(argv0):
    sys.stderr.write("usage:\n")
    sys.stderr.write("    python "+argv0+" test\n")
    sys.stderr.write("    python "+argv0+" Priest\n")
    sys.stderr.write("    python "+argv0+" Tester addresses\n")
    sys.exit(1)

utils.helpPythonExit()
main(sys.argv)
