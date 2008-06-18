#!/usr/bin/env python

import fcntl
import os
import select
import subprocess

bootstrapSource="""
import sys
exec(sys.stdin.read(int(sys.stdin.readline())))
"""

pysyncSource="""

import sys
import time

def destination():
    sys.stderr.write('stuff written to stderr\\nline2\\n')
    while 1:
	time.sleep(1)

if 'pysyncSource' not in globals():
    destination()
"""

exec(pysyncSource)

def usage():
    sys.stderr.write("usage:\n")
    sys.stderr.write("    python "+sys.argv[0]+" sourcedir [user@]host:destdir\n")
    sys.exit(1)

def work(stdin,stdout,stderr):
    ready = select.select([stdout,stderr],[],[stdin,stdout,stderr])
    if ready[2]:
	raise IOError()
    if stderr in ready[0]:
	sys.stderr.write("Remote process printed this error message:\n")
	sys.stderr.write(stderr.read())
	sys.exit(1)

def main():
    if len(sys.argv)!=3:
	usage()
    sourcedir = sys.argv[1]
    destdir = sys.argv[2]
    i = destdir.find(':')
    if i==-1:
	usage()
    userAndHost = destdir[:i]
    destdir = destdir[i+1:]
    p = subprocess.Popen(["ssh",userAndHost,"python -c '%s'"%bootstrapSource],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
	p.stdin.write("%d\n%s"%(len(pysyncSource),pysyncSource))
	fcntl.fcntl(p.stdout,fcntl.F_SETFL,fcntl.fcntl(p.stdout,fcntl.F_GETFL)|os.O_NONBLOCK)
	fcntl.fcntl(p.stderr,fcntl.F_SETFL,fcntl.fcntl(p.stderr,fcntl.F_GETFL)|os.O_NONBLOCK)
	while 1:
	    work(p.stdin,p.stdout,p.stderr)
    finally:
	os.kill(p.pid,9)

main()
