#!/usr/bin/env python
'''
gprsync.py -- perform an ad-hoc rsync operation

Usage: gprsync.py [-v] [-p port] -o rsyncopt SRC HOST:DEST

	-p port     : specifies the port number
        -v          : verbose
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
    PASS = random.randint(1, 1000000)
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
        (options, args) = getopt.getopt(sys.argv[1:], 'p:o:v')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-p':	GV.opt['-p'] = parse_port(val)
        elif switch == '-v':	GV.opt['-v'] = True
        elif switch == '-o':	GV.opt['-o'] = val

    if not GV.opt['-o']:
        usage('Please specify -o options')

    if len(args) != 2:
        usage()

    srcpath = args[0]
    try:
        (GV.desthost, GV.destpath) = args[1].split(':')
    except ValueError:
        usage('Invalid HOST:DEST argument')

    if not os.path.exists(srcpath):
        usage('SRC path does not exist')

    GV.srcpath = os.path.abspath(srcpath)



###############################
def strcmd(cmd):
    return reduce(lambda x, y: x + ' ' + y, map(lambda x: x.find(' ') > 0 and "'" + x + "'" or x, cmd))



###############################
def makeConfFile():
    '''Create rsyncd config files in /tmp. Use the global SUFFIX to ensure unique filenames'''
    fname = '/tmp/rsyncd.conf.' + GV.SUFFIX
    f = open(fname, 'w')
    f.write('pid file = /tmp/rsyncd.pid.%s\n' % GV.SUFFIX)
    f.write('log file = /tmp/rsyncd.log.%s\n' % GV.SUFFIX)
    f.write('\n')
    f.write('[%s]\n' % SECRET)
    f.write('\t path = %s\n' % srcpath)
    f.write('\t use chroot = false\n')
    f.close()
    return fname



###############################
def startLocalRsyncDaemon(port):
    '''Start rsync daemon locally'''
    confpath = makeConfFile()
    cmd = 'rsync --port=%d --daemon --no-detach --config=%s' % (port, confpath)
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd.split(' '))
    time.sleep(1)
    cmd = 'rsync --port=%d localhost::' % opt['-p']
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
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
def doRemoteRsync(port):
    cmd= []
    cmd.append('ssh')
    cmd.append('-o')
    cmd.append('BatchMode yes')
    cmd.append('-o')
    cmd.append('StrictHostKeyChecking no')
    cmd.append(desthost)
    cmd.append('cat | python')
    if GV.opt['-v']: print '[INFO]', strcmd(cmd)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    script = '''
import os, sys
host = os.environ.get('SSH_CLIENT').split()[0]
port = %d
src = '%s'
dst = '%s'
rsyncopt = '%s'
# os.system('hostname')
cmd = 'rsync --port=%%d %%s %%s::%%s %%s' %% (port, rsyncopt, host, src, dst)
print cmd
rc = 0
rc = os.system(cmd)
sys.exit(rc)
''' % (port, SECRET, destpath, opt['-o'])
    #print 'writing: '
    #print script
    proc.stdin.write(script)
    proc.stdin.close()
    #print 'reading: '
    while True:
        line = proc.stdout.readline()
        print '[RSYNC]', line,
    #print 'waiting: '
    rc = proc.wait()
    return not rc

    


###############################
def doCleanup(proc = None):
    if proc:
        try:
            os.kill(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        proc.wait()
    os.system('rm -f /tmp/rsyncd.*' + GV.SUFFIX)
    pass


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

    # start rsync daemon locally
    proc = None
    for x in xrange(10):
        port = portBase + x
        opt['-p'] = port
        proc = startLocalRsyncDaemon(port)
        if proc: break

    if proc:
        # do remote rsync 
        if not doRemoteRsync(port):
            doCleanup(proc)
            sys.exit('[ERROR] rsync failed')
    
    doCleanup(proc)
    sys.exit(0)
