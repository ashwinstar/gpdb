#!/usr/bin/env python
'''
gplog.py -- print system log

Usage: gplog.py [-s] [-p port] [-d dir] [-t "%yyyy-%mm-%dd %hh:%MM:%SS"] 

        -s  : print system properties in Python pickled format
        -p  : port number
        -t  : log time
        -d  : data directory
'''

import os, platform, sys, getopt, pickle, time, StringIO

sys.path.append(sys.path[0])
try:
    import gplib
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))

opt = {}
opt['-p'] = 0
opt['-t'] = time.strftime('%Y-%m-%d %H:%M:%S')
opt['-d'] = None
opt['-s'] = False

################
def usage(exitarg):
    print __doc__
    sys.exit(exitarg)


def parseCommandLine():
    global opt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], 'p:t:d:s')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-p':	opt['-p'] = gplib.parse_port(val)
        elif switch == '-t':    opt['-t'] = val
        elif switch == '-d':    opt['-d'] = val
        elif switch == '-s':    opt['-s'] = True
    
    if opt['-p'] != None:
        if not opt['-p'] > 0: usage('Error: please specify port number for -p switch')

    if opt['-d'] == None:
        usage('Error: please specify the data directory for -d switch')


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

def runWithRawOutput(cmd):
    f = None
    ok = False
    out = StringIO.StringIO()
    try:
        f = os.popen(cmd)
        for line in f:
            out.write(line)
        ok = not f.close()
    finally:
        if f: f.close()
    return (ok, out.getvalue())

def getlog():
    stime = opt['-t']
    print stime
    stime = stime.split(' ')
    
    cmd = '''awk -v date="%s" -v time="%s" 'BEGIN {r = 0; } {
                if (r == 1) {
                     print $0;
                } else {
                     if ($1 ~ /20[0-9][0-9]-[0-1][0-9]-[0-3][0-9]/ && $1 >= date && $2 >= time) {
                        r = 1;
                        print $0;
                     }
                }
             }' %s''' % (stime[0], stime[1], '%s.log' % opt['-d'])

    return runWithRawOutput(cmd)



parseCommandLine()
res = {}

############################## NETSTAT #############################
(ok, out) = runWithRawOutput('netstat -an')
res['netstat'] = out

############################## PG_HBA.CONF #############################
(ok, out) = runWithRawOutput('cat %s/pg_hba.conf' % opt['-d'])
res['pg_hba.conf'] = out

############################## LOG #############################
(ok, out) = getlog()
res['log'] = out

if opt['-s']:
    print 'BEGINDUMP'
    print pickle.dumps(res)
    print 'ENDDUMP'
else:
    for i in res:
        print '############## %s ################' % i
        print res[i]
