#!/usr/bin/env python
'''
gprsync.py -- perform an ad-hoc rsync operation

Usage: gprsync.py [-?v] -p port -o rsyncopt -d srcdir file [file]... HOST:DEST

	-?	    : print help screen
        -v          : verbose
	-p port     : specifies the port number
        -o rsyncopt : -o '-cnav' means checksum, dry-run, archive, verbose
                      -o '-cav' means checksum, archive, verbose
		      -o '-cav --delete' means copy to DEST and delete files 
				in DEST that don't exist on SRC.
	-d srcdir   : specifies the source dir
        file	    : source files directly under srcdir
	HOST:DEST   : destination host, and destination directory on 
			destination host.

'''

import os, sys, getopt, random, time, subprocess, signal

###############################
random.seed()

class Global():
    USER = os.environ.get('USER')
    PASS = random.randint(1, 1000000)
    SUFFIX = '%s.%d' % (USER, os.getpid())
    SECRET = '%d.%d' % (random.randint(1, 1000000), random.randint(1, 1000000))
    opt={}
    opt['-p'] = 0
    opt['-v'] = False
    opt['-V'] = False
    opt['-o'] = None
    opt['-d'] = None
    desthost = None
    destdir = None
    srcdir = None
    srcfile = []

GV = Global()


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
        (options, args) = getopt.getopt(sys.argv[1:], 'Vv?p:o:d:')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-?':	usage(0)
        elif switch == '-p':	GV.opt['-p'] = parse_port(val)
        elif switch == '-v':	GV.opt['-v'] = True
        elif switch == '-V':    GV.opt['-v'] = True; GV.opt['-V'] = True
        elif switch == '-o':	GV.opt['-o'] = val
        elif switch == '-d':	GV.opt['-d'] = val

    if not GV.opt['-o']:
        usage('Please specify -o option')

    if not GV.opt['-d']:
        usage('Please specify -d option')

    if len(args) < 2:
        usage('Missing arguments')

    try:
        (GV.desthost, GV.destdir) = args.pop().split(':')
    except ValueError:
        usage('Invalid HOST:DEST argument')

    GV.srcdir = GV.opt['-d']
    if not os.path.exists(GV.srcdir):
        usage('option -d "%s" directory does not exist' % GV.srcdir)
    GV.srcdir = os.path.abspath(GV.srcdir)
    if not os.path.isdir(GV.srcdir):
        usage('option -d "%s" is not a directory' % GV.srcdir)
        
    GV.srcfile = []
    srcdirlen = len(GV.srcdir)
    for f in args:
        ff = os.path.join(GV.srcdir, f)
        if not os.path.exists(ff): 
            usage('file "%s" does not exist' % ff)
        GV.srcfile.append(f)

    return True


###############################
def strcmd(cmd):
    return reduce(lambda x, y: x + ' ' + y, map(lambda x: x.find(' ') > 0 and "'" + x + "'" or x, cmd))


###############################
def makeConfFile(srcdir, flist):
    '''Create rsyncd config files in /tmp. Use the global SUFFIX to ensure unique filenames'''
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter makeConfFile'
    fname = '/tmp/rsyncd.conf.' + GV.SUFFIX
    f = open(fname, 'w')
    f.write('log file = /tmp/rsyncd.log.%s\n' % GV.SUFFIX)
    f.write('pid file = /tmp/rsyncd.pid.%s\n' % GV.SUFFIX)
    f.write('\n')
    f.write('[%s]\n' % GV.SECRET)
    f.write('\t path = %s\n' % srcdir)
    f.write('\t use chroot = false\n')
    f.write('\t read only = true\n')
    f.write('\t list = false\n')
    f.write('\t exclude = ')
    for x in flist: 
        if (os.path.isdir(os.path.join(srcdir, x))):
            f.write(' + /%s/' % x)
        else:
            f.write(' + /%s' % x)
    f.write(' - /*\n')
    f.close()
    return fname



###############################
def startLocalRsyncDaemon(port, srcdir, flist):
    '''Start rsync daemon locally'''
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter startLocalRsyncDaemon'
    confpath = makeConfFile(srcdir, flist)
    cmd = 'rsync -vv --port=%d --daemon --no-detach --config=%s < /dev/null' % (port, confpath)
    if GV.opt['-v']: print '[INFO]', cmd
    proc = subprocess.Popen(cmd, shell=True, stderr=subprocess.STDOUT)
    time.sleep(1)
    
    if proc.poll(): 
        return None

    # test
    doTest = True
    if doTest:
        testcmd = 'rsync --port=%d localhost::' % port
        if GV.opt['-v']: print '[INFO]', testcmd
        x = subprocess.Popen(testcmd.split(' '), stdout=subprocess.PIPE)
        x.stdout.read(-1)
        rc = x.wait()
        if rc:
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait()
            sys.exit('[ERROR] cannot start rsync daemon locally\n'
                     + '[ERROR] ... command was: ' + cmd)

    return proc


###############################
def checkRemoteSSH(host):
    '''Check that we are able to ssh to remote host without password'''
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter checkRemoteSSH'
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
        if out.find('Permission denied') >= 0:
            print '[ERROR] Permission denied while ssh to %s' % host
            print '        hint: run gpssh-exkeys'
        else:
            print '[ERROR] Cannot ssh to %s' % host
            print '[INFO] ssh output:', out
        sys.exit(1)

    return True
    

###############################
def checkRemote(host, dir):
    '''check that rsync available and dir exists on host, and is indeed a directory'''
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter checkRemote'
    cmd = ['ssh', '-o', 'BatchMode yes', 
           '-o', 'StrictHostKeyChecking no', host, 'python']
    E_NORSYNC = 10
    E_NOTEXIST = 11
    E_NOTDIR = 12
    script = '''import os, sys
dir = os.path.abspath("""%s""")
if os.system("bash -c 'type -ap rsync > /dev/null'"): sys.exit(%d)
if not os.path.exists(dir): sys.exit(%d)
if not os.path.isdir(dir): sys.exit(%d)
print os.path.abspath(dir)
sys.exit(0)
''' % (dir, E_NORSYNC, E_NOTEXIST, E_NOTDIR)
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    if GV.opt['-v']: 
        for line in script.split('\n'): 
            print '[INFO]   ', line
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE)
    proc.stdin.write(script)
    proc.stdin.close()
    abspath = (proc.stdout.read(-1)).strip()
    rc = proc.wait()
    if rc:
        if rc == E_NORSYNC:
            print ('[ERROR] rsync not found on host %s' % host)
        elif rc == E_NOTEXIST:
            print ("[ERROR] the path '%s' does not exist on host %s" 
                   % (dir, host))
        elif rc == E_NOTDIR:
            print ("[ERROR] the path '%s' on host %s is not a directory"
                   % (dir, host))
        sys.exit(1)

    return True



###############################
def checkLocal():
    '''check that rsync available locally'''
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter checkLocal'
    if os.system('''bash -c 'type -ta rsync' > /dev/null'''):
        print '[ERROR] rsync not found locally'
        sys.exit(1)
    return True

###############################
def doRemoteRsync(desthost, destdir, localport, rsyncopt, slist):
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter doRemoteRsync'
    cmd= []
    cmd.append('ssh')
    cmd.append('-o')
    cmd.append('BatchMode yes')
    cmd.append('-o')
    cmd.append('StrictHostKeyChecking no')
    cmd.append(desthost)
    cmd.append('python')
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    script = [
        '''import os, sys''',
        '''host = os.environ.get('SSH_CLIENT').split()[0]''',
        '''port = %d''' % localport,
        '''srclist = %s''' % str(slist),
        '''destdir = '%s' ''' % destdir,
        '''rsyncopt = '%s' ''' % rsyncopt,
        '''secret = '%s' ''' % GV.SECRET,
        '''for src in srclist:''',
        '''    cmd = ('rsync -v --progress --port=%d %s %s::%s/%s %s 2>&1' % ''',
        '''           (port, rsyncopt, host, secret, src, destdir))''',
        '''    print '-----------------' ''',
        '''    print 'Running', cmd''',
        '''    sys.stdout.flush()''',
        '''    rc = os.system(cmd)''',
        '''    sys.stdout.flush()''',
        '''    if rc: print 'Error: rsync failed with error', rc'''
        '''sys.exit(0)''']

    if GV.opt['-v']:
        for line in script: print '[INFO]   ', line

    script = '\n'.join(script) + '\n'
    proc.stdin.write(script)
    proc.stdin.close()

    while True:
        line = proc.stdout.readline()
        if not line: break
        print ('[INFO@%s] %s' % (desthost, line)),

    rc = proc.wait()
    if rc: 
        sys.exit('[ERROR] remote rsync failed')
    return True


###############################
def doCleanup(proc = None):
    if GV.opt['-V']: 
        print '[INFO] ---------------- enter doCleanup'

    pidfile = '/tmp/rsyncd.pid.%s' % GV.SUFFIX
    if proc:
        os.system("kill $(< %s) > /dev/null 2>&1 " % pidfile)
        os.system('kill -9 %d > /dev/null 2>&1' % proc.pid)
        proc.wait()
        time.sleep(0.5)

    os.system('rm -f /tmp/rsyncd.*' + GV.SUFFIX)
    return True


###############################
if __name__ == '__main__':
    os.umask(0x077)
    doCleanup()
    parseCommandLine()
    portBase = GV.opt['-p']

    # check that we can ssh to desthost
    checkRemoteSSH(GV.desthost)

    # check rsync exists on desthost
    checkRemote(GV.desthost, GV.destdir)

    # check rsync exists locally
    checkLocal()

    # start rsync daemon locally
    proc = None
    for x in xrange(10):
        port = portBase + x
        GV.opt['-p'] = port
        proc = startLocalRsyncDaemon(port, GV.srcdir, GV.srcfile)
        if proc: break

    if proc:
        # do remote rsync 
        if not doRemoteRsync(GV.desthost, GV.destdir, 
                             port, GV.opt['-o'], GV.srcfile):
            doCleanup(proc)
            sys.exit('[ERROR] rsync failed')
    
    doCleanup(proc)
    if GV.opt['-v']: print '[INFO] SUCCESS'
    sys.exit(0)
