#!/usr/bin/env python

'''
gposcheck -- check os for consistency and compatibility with GPDB.

Usage: gposcheck [--version] [-?v] [-p port] [-u user] [-h host] [-f hostfile] 


          --version : print version information
          -?        : print this help screen
          -v        : verbose mode
          -p port   : ssh port to use 
          -u user   : the user to run command
          -h host   : the host to connect to (multiple -h is okay)
          -f file   : a file listing all hosts to connect to

'''

import os, sys, getopt, popen2, pickle

if sys.version_info < (2, 3):
    sys.exit("Error: Python version 2.3 or greater only.\n"
             + "Please upgrade python installed on this machine.")

sys.path.append(sys.path[0] + '/lib')

script_name = os.path.split(__file__)[-1]
opt = {}
opt['-v'] = False
opt['-p'] = 22
opt['-u'] = os.environ.get('LOGNAME') or os.environ.get('USER')
opt['-h'] = []
opt['-f'] = False
GPHOME = os.environ.get('GPHOME')
if not GPHOME:
    sys.exit('GPHOME env not set')


################
def usage(exitarg):
    help_path = os.path.join(sys.path[0], '..', 'docs', 'cli_help', script_name + '_help');
    try:
	f = open(help_path);
	for line in f: 
	    print line,
    except:
        print __doc__
    sys.exit(exitarg)

#############
def parsePort(val): 
    try: return int(val)
    except ValueError: return 0


#############
def print_version():
    print '%s version $Revision$' % script_name
    sys.exit(0)

#############
def parseCommandLine():
    global opt, argcmd
    try:
	(options, args) = getopt.getopt(sys.argv[1:], '?evp:u:h:f:', ['version'])
    except Exception, e:
	usage('Error: ' + str(e))

    for (switch, val) in options:
	if (switch == '-?'):	      usage(0)
	elif (switch[1] in 'v'):      opt[switch] = True
	elif (switch[1] in 'uf'):     opt[switch] = val
	elif (switch == '-h'):        opt[switch].append(val)
	elif (switch == '-p'):        opt[switch] = parsePort(val)
        elif (switch == '--version'): print_version()

    if not opt['-p'] > 0: usage('Error: please specify port number for -p switch')

    hf = (len(opt['-h']) and 1 or 0) + (opt['-f'] and 1 or 0)
    if hf != 1:
	usage('Error: please specify at least one of -h or -f args, but not both')

    if (len(args) >= 1):
	argcmd = " ".join(args)

################
def parseHostLine(line):
    '''Parse line into (user, host, port) tuple'''
    try:
	user = opt['-u']
	host = None
	port = opt['-p']
	t = line.strip().split('@')
	cnt = len(t)
	if t[0][0] == '#': return None
        if cnt == 1: 
	    host = t[0]
	else:
	    user = t[0]
	    host = t[1]

	t = host.split(':')
	if len(t) > 1:
	    host = t[0]
	    port = int(t[1])
	    
	return (user, host, port)

    except ValueError: pass
    except IndexError: pass
    return None

#############
def readHostFile(fname):
    retList = []
    if not fname: return retList
    f = None; lno = 0; host = {}
    try:
	try:
	    f = open(fname, 'r')
	    for line in f:
		lno = lno + 1
		h = parseHostLine(line)
		if not h: continue
		key = '%s@%s' % (h[0], h[1])
		host[key] = h
	    for n in host: retList.append(host[n])
	except IOError:
	    sys.exit('[ERROR] cannot read host file ' + fname 
		     + (lno > 0 and (' (line %d)' % lno) or ''))
    finally:
	if f: f.close()

    return retList


##########
def sshGpSys():
    session = []
    user = {}
    for h in opt['-h']: user[h[0]] = h
    sameUser = (len(user) == 1)
    for h in opt['-h']:
        cmd = ('export PATH=%s && export LD_LIBRARY_PATH=%s && export GPHOME=%s && $GPHOME/bin/lib/gpsys.py -p' % (os.getenv('PATH'), os.getenv('LD_LIBRARY_PATH'), GPHOME))
        ssh = 'ssh -p %d -o "BatchMode yes" -o "StrictHostKeyChecking no" %s@%s' % (h[2], h[0], h[1])
	ssh = ssh + ' eval  \'%s\' ' % cmd
        if opt['-v']: print '[INFO %s@%s] %s' % (h[0], h[1], cmd)
        p = popen2.Popen4(ssh)
        p.x_user = h[0]; p.x_host = h[1]
	p.x_cmd = cmd;   p.x_ssh = ssh
	p.x_peer = sameUser and h[1] or ('%s@%s' % (h[0], h[1]))
	p.tochild.close()
	session.append(p)

    return session

def waitSession(session):
    errmsg = None
    permissionDenied = False
    for p in session:
        p.x_res = p.fromchild.read()
        if opt['-v']: print p.x_res
        try:
            p.x_res = pickle.loads(p.x_res)
        except pickle.UnpicklingError:
            permissionDenied = (p.x_res.find('Permission denied') >= 0)
            if not permissionDenied:
                errmsg = '[ERROR %s] bad output while %s' % (p.x_peer, p.x_ssh)
                print errmsg
	status = p.wait()
	if status:
	    errmsg = '[ERROR %s] Command failed. Exit status = %s' % (p.x_peer, status)
	    print errmsg
		
    if permissionDenied:
	print
	print 'hint: check if the host is alive'
	print 'hint: use gpssh-exkeys to setup public-key authentication between hosts'
	print
    if errmsg: sys.exit(1)


def match(session, key, value):
    ok = True
    for p in session:
        v = p.x_res.get(key, '')
        p.x_match = (v == value)
        if not p.x_match: 
            print '  [ERROR %s] %s mismatch (value %s should == %s)' % (p.x_peer, key, v, value)
            ok = False
    return ok

def matchList(session, key, value):
    ok = True
    for p in session:
	v = p.x_res.get(key, '')
	l = v.split()
        p.x_match = (l == value)
        if not p.x_match:
	    print '  [Error %s] %s mismatch (value %s should == %s)' % (p.x_peer, key, str(l), str(value))
            ok = False
    return ok

def matchMore(session, key, value):
    ok = True
    value = int(value)
    for p in session:
        v = int(p.x_res.get(key, 0))
        p.x_match = (v >= value)
        if not p.x_match:
            print '  [ERROR %s] %s invalid (value %s should be >= %s)' % (p.x_peer, key, v, value)

try:
    parseCommandLine()
    tab = []
    for h in opt['-h']:
        h = parseHostLine(h)
        if h: tab.append(h)
        else: usage('Error: invalid -h argument')

    tab.extend(readHostFile(opt['-f']))

    if (len(tab) == 0):
        print '[ERROR] No host specified'
        sys.exit(1)

    opt['-h'] = tab
    session = sshGpSys()
    waitSession(session)

    p0 = session[0]
    # check gphome
    key = 'gp.gphome'
    for p in session:
        value = p.x_res.get(key)
        if not value:
            print '  [ERROR %s] GPHOME not set' % p.x_peer

    # check postgres 
    key = 'gp.postgres.md5'
    value = p0.x_res.get(key)
    if not value: 
        print '  [ERROR %s] postgres binaries unavailable' % p0.x_peer
    else:
        print key, '=', value
        match(session, key, value)
        # check postgres version
        key = 'gp.postgres.version'
        value = p0.x_res.get(key)
        if not value: 
            print '  [ERROR %s] postgres version unavailable' % p0.x_peer
        else:
            print key, '=', value
            match(session, key, value)

    key = 'python.platform.system'
    value = p0.x_res.get(key)
    if value == 'linux' and match(session, key, 'linux'):
        key = 'python.platform.release'
        v = session[0].x_res['python.platform.release']
        if v.find('2.6') != 0:
            print '  [ERROR %s] Linux version (version %s should be >= 2.6)' % (p.x_peer, v)
        elif match(session, key, v):
            # check shm
            key = 'sysctl.kernel.shmmax'
            matchMore(session, key, 500000000)
            key = 'sysctl.kernel.shmmni'
            matchMore(session, key, 4096)
            key = 'sysctl.kernel.shmall'
            matchMore(session, key, 4000000000)

            # check sem
            key = 'sysctl.kernel.sem'
            matchList(session, key, ['250', '64000', '100', '512'])

            # check net
            key = 'sysctl.net.ipv4.tcp_tw_recycle'
            matchMore(session, key, 1)
            key = 'sysctl.net.ipv4.tcp_max_syn_backlog'
            match(session, key, '4096')
            key = 'sysctl.net.core.netdev_max_backlog'
            match(session, key, '10000')

            # check overcommit memory
            key = 'sysctl.vm.overcommit_memory'
            match(session, key, '2')

            # check ulimit
            key = 'ulimit.nofile'
            matchMore(session, key, 65536)
            key = 'ulimit.nproc'
            matchMore(session, key, 131072)

    elif value == 'sunos' and match(session, key, 'sunos'):
        key = 'python.platform.release'
        v = session[0].x_res['python.platform.release']
        if v.find('5.10') != 0:
            print '  [ERROR %s] Solaris version (version %s should be >= 5.10)' % (p.x_peer, v)
        elif match(session, key, v):
            # check ndd
            key = 'ndd.tcp_conn_req_max_q'
            match(session, key, '4096')

            key = 'ndd.tcp_conn_req_max_q0'
            match(session, key, '4096')

            key = 'ndd.tcp_largest_anon_port'
            match(session, key, '65535')
            
            key = 'ndd.tcp_smallest_anon_port'
            match(session, key, '4096')

            key = 'ndd.tcp_time_wait_interval'
            match(session, key, '100')

            # check system
            key = 'system.rlim_fd_max'
            match(session, key, '262144')
            
            key = 'system.rlim_fd_cur'
            match(session, key, '65536')

            key = 'system.shmsys:shminfo_shmmax'
            matchMore(session, key, 0x2000000)

            key = 'system.semsys:seminfo_semmi'
            match(session, key, '1024')
    
    elif value:
        print ('[ERROR %s] invalid system (value %s should be either Linux or SunOS)' 
               % (p0.x_peer, value))
    else:
        print ('[ERROR %s] unknown system (key %s should be either Linux or SunOS)' 
               % (p0.x_peer, key))

    

except KeyboardInterrupt:
    sys.exit('\nInterrupted ...')
