#!/usr/bin/env python
'''
gpsys.py -- print system properties

Usage: gpsys.py [-p] [-t]

	-p  : print system properties in Python pickled format
        -t  : print system properties in Text (default).
'''

import os, platform, sys, getopt, pickle

opt = {}
opt['-p'] = False

################
def usage(exitarg):
    print __doc__
    sys.exit(exitarg)


def parseCommandLine():
    global opt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], 'pt')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-p':	opt['-p'] = True
        elif switch == '-t':    opt['-p'] = False

def run(cmd):
    f = None
    ok = False
    out = []
    try:
        f = os.popen(cmd)
        for line in f:
            out.append(line)
        ok = not f.close()
    finally:
        if f: f.close()

    return (ok, out)

def add(res, prefix, lines):
    for line in lines:
        x = line.split(' ', 1)
        if len(x) == 2:
            x[0] = x[0].strip()
            x[1] = x[1].strip()
            if (x[0] and x[1]):
                if x[0][-1] == ':':
                    x[0] = x[0][:-1]
                elif x[1][0] == ':':
                    x[1] = x[1][1:]
                elif x[0][-1] == '=':
                    x[0] = x[0][:-1]
                elif x[1][0] == '=':
                    x[1] = x[1][1:]

                if x[0]:
                    res[prefix + x[0].lower().strip()] = x[1].strip()
    return res


def do_gppath(res):
    out = os.getenv("GPHOME")
    if out is None:
	return False

    res['gp.gphome'] = out.strip()
    return True


def do_postgres_md5(res):
    (ok, out) = run("cat $GPHOME/bin/postgres | "
                    "python -c 'import md5, sys; m = md5.new(); m.update(sys.stdin.read()); print m.hexdigest()'")
    if ok:
        for line in out:
            if len(line) == 33:
                res['gp.postgres.md5'] = line.lower().strip()
                return True
    return False


def do_postgres_version(res):
    (ok, out) = run('$GPHOME/bin/postgres --version')
    if ok and len(out) == 1: 
        res['gp.postgres.version'] = out[0].strip()
        return True
    return False
            


def do_sysctl(res):
    (ok, out) = run('export PATH="/sbin:/usr/sbin:$PATH" && sysctl -a 2> /dev/null')
    if ok:
        add(res, 'sysctl.', out)
    return ok


def do_ulimit(res):
    (ok, out) = run('ulimit -u && ulimit -n')
    if ok:
        res['ulimit.nproc'] = out[0].strip()
        res['ulimit.nofile'] = out[1].strip()
    return ok


def do_platform(res):
    res['python.platform.platform'] = platform.platform()
    uname = platform.uname()
    res['python.platform.system'] = uname[0].lower()
    res['python.platform.node'] = uname[1]
    res['python.platform.release'] = uname[2]
    res['python.platform.version'] = uname[3]
    res['python.platform.machine'] = uname[4]
    res['python.platform.processor'] = uname[5]
    return True

def do_system(res):
    SYSTEM_KEYS = ('rlim_fd_max', 'rlim_fd_cur', 'shmsys:shminfo_shmmax', 'semsys:seminfo_semmni')
    f = open('/etc/system', 'r')
    content = f.read()
    f.close()
    
    p = []
    lines = content.splitlines()
    for line in lines:
        line = line.strip()
        if line.startswith('set'):
            for key in SYSTEM_KEYS:
                if line.find(key) != -1:
		    res[key] = line[3:].split('=').strip()
                    break

    return True

def do_meminfo(res):
    if not os.path.exists('/proc/meminfo'):
        return False

    f = None
    try:
        f = open('/proc/meminfo', 'r')
        list = []
        for line in f:
            list.append(line)
        add(res, '/proc/meminfo', list)
        return True
    finally:
        if f: f.close()

def do_ndd(res):
    if not os.path.exists('/usr/sbin/ndd'):
        return False

    list = ('tcp_conn_req_max_q tcp_conn_req_max_q0 tcp_largest_anon_port ' +
            'tcp_smallest_anon_port tcp_time_wait_interval').split()
    for key in list:
        (ok, out) = run('/usr/sbin/ndd /dev/tcp ' + key)
        if ok and len(out) == 1:
            res['ndd.' + key] = out[0].strip()
    

parseCommandLine()
res = {}
do_gppath(res)
do_postgres_md5(res)
do_postgres_version(res)
do_platform(res)
do_meminfo(res)
if res['python.platform.system'] == 'sunos':
    do_system(res)
    do_ndd(res)
elif res['python.platform.system'] == 'linux':
    do_sysctl(res)
    do_ulimit(res)


if opt['-p']:
    print pickle.dumps(res)
else:
    for i in res:
        print i, ' | ', res[i]

