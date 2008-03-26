#!/usr/bin/env python
'''
gprsync.py -- perform an ad-hoc rsync operation

Usage: gprsync.py [-v] -p port -o rsyncopt SRC [SRC...] HOST:DEST

	-?	    : print this help screen
        -v          : verbose
	-p port     : specifies the port number
        -o rsyncopt : -o '-cnav' means checksum, dry-run, archive, verbose
                      -o '-cav' means checksum, archive, verbose
		      -o '-cav --delete' means copy to DEST and delete files 
				in DEST that don't exist on SRC.

'''

import os, sys, getopt, random, time, subprocess, signal

###############################
random.seed()

class Global():
    USER = os.environ.get('USER')
    SUFFIX = '%s.%d' % (USER, os.getpid())
    SECRET = '%d.%d' % (random.randint(1, 1000000), random.randint(1, 1000000))
    opt={}
    opt['-p'] = 0
    opt['-v'] = False
    opt['-o'] = ''
    desthost = ''
    destpath = ''
    srcpath = ''

GV = Global()
MYD = os.path.abspath(os.path.dirname(__file__))

###############################
def usage(exitarg = None):
    print __doc__
    sys.exit(exitarg)


################
def parse_port(val):
    try: val = int(val)
    except ValueError: val = 0
    return val


###############################
def parseCommandLine():
    try:
        (options, args) = getopt.getopt(sys.argv[1:], 'p:o:v?')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-p':	GV.opt['-p'] = parse_port(val)
        elif switch == '-v':	GV.opt['-v'] = True
        elif switch == '-o':	GV.opt['-o'] = val
        elif switch == '-?':	usage(0)

    if not GV.opt['-p']:
        usage('Please specify -p port')

    if not GV.opt['-o']:
        usage('Please specify -o options')

    if len(args) < 2:
        usage('Missing arguments')

    try:
        (GV.desthost, GV.destpath) = args.pop().split(':')
    except ValueError:
        usage('Invalid HOST:DEST argument')

    GV.srcpath = []
    for i in args:
        if not os.path.exists(i):
            usage('file "%s" does not exist' % i)
        GV.srcpath.append(os.path.abspath(i))


###############################
def strcmd(cmd):
    return reduce(lambda x, y: x + ' ' + y, map(lambda x: x.find(' ') > 0 and "'" + x + "'" or x, cmd))

###############################
def copy_gprsyncd(host):
    '''copy the file gprsyncd.py to remote host'''
    path = os.path.join(MYD, 'gprsyncd')
    if not os.path.exists(path):
        sys.exit('[ERROR] cannot find file "%s"' % path)

    cmd = "scp -o 'BatchMode yes' -o 'StrictHostKeyChecking no' %s %s:/tmp/gprsyncd_$USER" % (path, host)
    if GV.opt['-v']: print '[INFO]', cmd
    if os.system(cmd):
        sys.exit('[ERROR] cannot scp gprsyncd to remote\n\tCommand was: %s' % cmd)


###############################
def startRemoteRsyncDaemon(host, port, destdir):
    '''Start rsync daemon remotely'''
    cmd = ['ssh', '-o', 'BatchMode yes', 
           '-o', 'StrictHostKeyChecking no', 
           host, 
           'python /tmp/gprsyncd_$USER %d %s' % (port, destdir)]
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    proc.stdin.write(GV.SECRET)
    proc.stdin.write('\n')
    proc.stdin.flush()
    time.sleep(1)
    raw_input('wait wait')

    # test it 
    cmd = 'rsync --port=%d %s::' % (port, host)
    if GV.opt['-v']: print '[INFO]', cmd
    x = subprocess.Popen(cmd.split(' '), stdout=subprocess.PIPE)
    print x.stdout.read(-1)
    ok = (not x.wait())
    if ok: 
        return proc

    os.kill(proc.pid, signal.SIGKILL)
    proc.wait()
    return None


###############################
def checkRemoteSSH(host):
    '''Check that we are able to ssh to remote host without password'''
    cmd= []
    cmd.append('ssh')
    cmd.append('-o')
    cmd.append('BatchMode yes')
    cmd.append('-o')
    cmd.append('StrictHostKeyChecking no')
    cmd.append(host)
    cmd.append('pwd')
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = proc.stdout.read(-1)
    rc = proc.wait()
    if rc: 
        print '[ERROR] cannot ssh to %s - hint: run gpssh-exkeys' % (host)
        sys.exit(1)


###############################
def doRemoteWhich(host, fname):
    '''Run 'which fname' on host. Return True if found.'''
    cmd= []
    cmd.append('ssh')
    cmd.append('-o')
    cmd.append('BatchMode yes')
    cmd.append('-o')
    cmd.append('StrictHostKeyChecking no')
    cmd.append(host)
    cmd.append('which %s' % fname)
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = proc.stdout.read(-1)
    rc = proc.wait()    # solaris has a busted 'which' that exit(0) even when not found
    # need to parse 'out'
    out = out.strip()
    if not out: return False
    out = out.split(' ')
    return len(out) == 1


###############################
def doLocalWhich(fname):
    '''Run 'which fname' on localhost. Return True if found.'''
    cmd= []
    cmd.append('which')
    cmd.append(fname)
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = proc.stdout.read(-1)
    rc = proc.wait()    # solaris has a busted 'which' that exit(0) even when not found
    # need to parse 'out'
    out = out.strip()
    if not out: return False
    out = out.split(' ')
    return len(out) == 1



###############################
def doRsync(host, port, destpath, srcpath):
    cmd= ['rsync' 
          '--port=%d' % port]
    for i in srcpath:
        cmd.append(i)
    
    cmd.append('%s:%s' % (host, destpath))
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    #print 'reading: '
    while True:
        line = proc.stdout.readline()
        if not line: break
        print '[RSYNC]', line,
    #print 'waiting: '
    rc = proc.wait()
    return not rc

    


###############################
def doCleanup(proc = None):
    if proc:
        try:
            os.kill(proc.pid, signal.SIGTERM)
            time.sleep(1)
            os.kill(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        proc.wait()


###############################
if __name__ == '__main__':
    os.umask(0x077)
    doCleanup()
    parseCommandLine()
    portBase = GV.opt['-p']
    if not portBase: portBase = 13500

    # check that we can ssh to desthost
    checkRemoteSSH(GV.desthost)

    # check rsync exists on desthost
    if not doRemoteWhich(GV.desthost, 'rsync'):
        print '[ERROR] rsync not found on server %s' % GV.desthost
        sys.exit(1)

    # check rsync exists locally
    if not doLocalWhich('rsync'):
        print '[ERROR] rsync not found locally'
        sys.exit(1)

    # start rsync daemon remotely
    copy_gprsyncd(GV.desthost)
    proc = None
    for x in xrange(10):
        port = portBase + x
        GV.opt['-p'] = port
        proc = startRemoteRsyncDaemon(GV.desthost, port, GV.destpath)
        if proc: break

    if proc:
        # do remote rsync 
        if not doRsync(GV.desthost, port, GV.destpath, GV.srcpath):
            doCleanup(proc)
            sys.exit('[ERROR] rsync failed')
    
    doCleanup(proc)
    sys.exit(0)
