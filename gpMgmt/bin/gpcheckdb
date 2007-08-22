#!/usr/bin/env python
'''
gpcheckdb -- Validate the Greenplum Database

Usage: gpcheckdb <options>

    --version  : print version information
    -?         : print this help text
    -v         : enable verbose
    -V         : enable very verbose
    -h hostname: where hostname is the Greenplum Database master hostname [default=localhost]
    -p port    : where port is the Greenplum Database port number [default=5432]
'''
import os, sys, getopt, stat

if sys.version_info < (2, 3):
    sys.exit("Error: gpcheckdb is supported on Python versions 2.3 or greater\n")


def usage(exitarg):
    help_path = os.path.join(sys.path[0], '..', 'docs', 'cli_help', os.path.split(__file__)[-1] + '_help');
    try:
        f = open(help_path);
        for line in f:
            print line,
    except:
        print __doc__
    sys.exit(exitarg)

script_name = os.path.split(__file__)[-1]
opt = {}
opt['-v'] = False
opt['-V'] = False
opt['-h'] = 'localhost'
opt['-p'] = 5432

GPHOME=os.getenv('GPHOME')
if GPHOME is None:
    sys.exit('GPHOME is not set')
PSQL=os.path.join(GPHOME, 'bin', 'psql')
OS = os.uname()


def parseInt(val): 
    try: return int(val)
    except ValueError: return 0

def run(cmd):
    p = os.popen(cmd)
    ret = []
    for line in p:
        ret.append(line)
    rc = p.close()

    return (not rc, ret)

######### class Config -- gp_configuration
class Config:
    class Record:
        def __init__(self, line):
            line = line.split('|')
            line = [l.strip() for l in line]
            self.content = int(line[0])
            self.definedprimary = line[1] == 't'
            self.dbid = line[2]
            self.isprimary = line[3] == 't'
            self.valid = line[4] == 't'
            self.hostname = line[5]
            self.port = line[6]
            self.datadir = line[7]
        
    def __init__(self):
        self.record = []
        self.fill()

    def fill(self):
        self.record = []
        (ok, out) = run('psql template1 -h %s -p %d -t -q -c \"select * from gp_configuration\"' % (opt['-h'], opt['-p']))
        if not ok:
            sys.exit('[%s:%d] [Error] Unable to select gp_configuration' % (opt['-h'], opt['-p']))
        for line in out:
            line = line.strip()
            if line: self.record.append(Config.Record(line))

    def get(self):
        return self.record

    def hasMirror(self):
        return reduce(lambda x, y: x or y,
                      [not r.isprimary for r in self.record])

    def getHosts(self, unique = True):
        list = map(lambda x: x.hostname, self.record)
        if unique:
            u = {}
            for h in list: u[h] = 1
            list = u.keys()
        return list


def parseCommandLine():
    global opt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], '?vVh:p:', ['version'])
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if (switch == '-?'): usage(0)
        elif (switch[1] in 'vV'): opt[switch] = True
        elif (switch == '-p'): opt[switch] = parseInt(val)
        elif (switch == '-h'): opt[switch] = val
        elif (switch == '--version'): print_version()

    if opt['-V']: opt['-v'] = True

    if not (opt['-p'] > 0):
        usage('Error: please specify port number for -p switch')


########################################################
# GPDB
########################################################
#  validate GPDB function

def match(hostname, port, key, qd_value, qe_value):
    if qd_value != qe_value:
        print '[%s:%s] [Error] %s mismatch (value %s should == %s)' % (hostname, port, key, str(qe_value), str(qd_value))
        return False
    return True

def validate_gp_id(qd_gpname, qd_numsegments, qe_content, qe_definedprimary, qe_dbid, \
                       qe_isprimary, qe_valid, qe_hostname, qe_port, qe_datadir):

    log = []

    (ok, out) = run('PGOPTIONS=\"-c gp_session_role=utility\" psql -h %s -p %s template1 -t -c \"select * from pg_catalog.gp_id\"' % \
                        (qe_hostname, qe_port))

    if ok == False:
        print '[%s:%s] [Error] Unable to select pg_catalog.gp_id' % (qe_hostname, qe_port)
        return ok

    idrow = out[0].split('|')
    gpname = idrow[0].strip()
    numsegments = idrow[1].strip()
    dbid = idrow[2].strip()
    content = parseInt(idrow[3].strip())
    
    ok = True
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_id.gpname', qd_gpname, gpname)
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_id.numsegments', qd_numsegments, numsegments)
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_id.dbid', qe_dbid, dbid)
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_id.content', qe_content, content)

    if ok == False:
        print '[%s:%s] [Error] checking pg_catalog.gp_id table on segment (content=%d, dbid=%s) ... failed' %  \
            (qe_hostname, qe_port, qe_content, qe_dbid)
    else:
        if opt['-v']:
            print '[%s:%s] [Info] checking pg_catalog.gp_id table on segment (content=%d, dbid=%s) ... passed' %  \
                (qe_hostname, qe_port, qe_content, qe_dbid)
    
    return ok

def validate_gp_version_at_initdb(qd_gpname, qd_numsegments, qd_schemaversion, \
                                      qd_productversion, qe_hostname, qe_port, qe_content, qe_dbid):

    (ok, out) = run('PGOPTIONS=\"-c gp_session_role=utility\" psql -h %s -p %s template1 -t -c \"select * from pg_catalog.gp_version_at_initdb\"' % (qe_hostname, qe_port))

    if ok == False:
        print '[%s:%s] [Error] Unable to select gp_version_at_initdb' % (qe_hostname, qe_port)
        return ok

    verrow = out[0].split('|')
    schemaversion = verrow[0].strip()
    productversion = verrow[1].strip()

    ok = True
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_version_at_initdb.schemaversion', qd_schemaversion, schemaversion)
    ok &= match(qe_hostname, qe_port, 'pg_catalog.gp_version_at_initdb.productversion', qd_productversion, productversion)
    if ok == False:
        print '[%s:%s] [Error] checking pg_catalog.gp_version_at_initdb table on segment (content=%d, dbid=%s) ... failed' % \
            (qe_hostname, qe_port, qe_content, qe_dbid)
    else:
        if opt['-v']:
            print '[%s:%s] [Info] checking pg_catalog.gp_version_at_initdb table on segment (content=%d, dbid=%s) ... passed' % \
                (qe_hostname, qe_port, qe_content, qe_dbid)

    return ok

def validate_pg_catalog(config):

    (ok, idbuf) = run('psql template1 -h %s -p %d -t -c \"select * from pg_catalog.gp_id\"' % (opt['-h'], opt['-p']))
    if ok == False:
        sys.exit('[%s:%d] [Error] Unable to select pg_catalog.gp_id' % (opt['-h'], opt['-p']))

    (ok, verbuf) = run('psql template1 -h %s -p %d -t -c \"select distinct * from pg_catalog.gp_version_at_initdb\"' % (opt['-h'], opt['-p']))
    if ok == False:
        sys.exit('[%s:%d] [Error] Unable to select pg_catalog.gp_version_at_initdb' % (opt['-h'], opt['-p']))

    verrow = verbuf[0].split('|')
    qd_schemaversion = verrow[0].strip()
    qd_productversion = verrow[1].strip()


    idrow = idbuf[0].split('|')
    qd_gpname = idrow[0].strip()
    qd_numsegments = idrow[1].strip()
    qd_dbid = idrow[2].strip()
    qd_content = idrow[3].strip()

    nfail = 0
    for qe in config.get():
    
        if qe.content == -1:
            continue

        ok = validate_gp_id(qd_gpname, qd_numsegments, qe.content, qe.definedprimary, \
                                qe.dbid, qe.isprimary, qe.valid, qe.hostname, qe.port, qe.datadir)
        if not ok:
            nfail += 1
            
        ok = validate_gp_version_at_initdb(qd_gpname, qd_numsegments, qd_schemaversion, \
                                               qd_productversion, qe.hostname, qe.port, qe.content, qe.dbid)
        if not ok:
            nfail += 1

    return nfail

def validate_GPDB(config):
    nfail = validate_pg_catalog(config)
    if nfail > 0:
        print '[Error] validatiion failure [%d error(s) found]' % nfail
    else:
        print '[Info] vaildation success'

########################################################
# main
########################################################
# __init__

try:
    parseCommandLine()

    # get gp_configuration
    config = Config()

    # validate QE Greenplum Database
    if opt['-v']: print '[Info] validate Greenplum Database...'
    validate_GPDB(config)
        
except KeyboardInterrupt:
    print '[Abort] Keyboard Interrupt...'
