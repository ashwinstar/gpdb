#!/usr/bin/env python

'''
gpinitconf.py 

Dump gp_configuration from Greenplum array and the output configuration file can be used as input of gpinitsystem -I option to create an array.

Usage: gpinitconf.py [-v] [-?q] [-p port] [-o output_config] [-l log_dir]

          General options:
          -v        : print version information
          -?        : print this help screen
          
          Logging options:
          -q        : quiet mode, do not log process to screen 
          
          Config options:
          -o        : <output_configu> output configuration file

          Connect options:
          -p        : port number to connect with. [default: value of PGPORT environment variable]
'''

from __future__ import with_statement
import os, sys, getopt
from datetime import date, datetime
sys.path.append(sys.path[0] + '/lib')
try:
    import pg8000

    import gplib
    import gpmlib
    from gpmlib import ENV
except ImportError, e:
    gpmlib.error('Unable to import module: ' + str(e))


script_name = os.path.split(__file__)[-1]
opt = {}
opt['-q'] = False
opt['-p'] = None
opt['-o'] = None
opt['-l'] = None

arg = {}
arg['exit'] = 0


################
def usage(exitarg):
    print gplib.cli_help() or __doc__
    sys.exit(exitarg)

################
def print_version():
    print '%s version $Revision: #32 $' % script_name
    sys.exit(0)

################
def parseCommandLine():
    global opt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], '?vql:p:o:', ['version'])
    except Exception, e:
        usage(str(e))

    for (switch, val) in options:
        if (switch == '-?'):          usage(0)
        elif (switch == '-v'):        print_version()
        elif (switch == '-q'):        opt[switch] = True
        elif (switch == '-l'):        opt[switch] = val
        elif (switch == '-p'):        opt[switch] = int(val)
        elif (switch == '-o'):        opt[switch] = val

    gpmlib.log_set_module(script_name)

    if opt['-q']:
        gpmlib.log_set_verbose(False)

    if opt['-l']:
        if not gpmlib.directory_exists(opt['-l']):
            gpmlib.error('Log directory %s does not exist' % opt['-l'])

        opt['-l'] = os.path.join(opt['-l'], '%s_%s.log' % (script_name, date.today().strftime('%Y%m%d')))
        gpmlib.log_set_file(opt['-l'])
    else:
        logdir = os.path.join(os.environ.get('HOME'), 'gpAdminLogs')
        if not gpmlib.directory_exists(logdir):
            try:
                os.mkdir(logdir)
            except OSError, e:
                gpmlib.error('Make directory %s error' % logdir)

        opt['-l'] = os.path.join(logdir, '%s_%s.log' % (script_name, date.today().strftime('%Y%m%d')))
        gpmlib.log_set_file(opt['-l'])

    if not opt['-o']:
        usage('Please specify -o output_config')

    if not opt['-p']:
        p = os.getenv('PGPORT')
        if not p:
            usage('Please specify -p pgport')
        opt['-p'] = int(p)


################
def gpconf():
    db = None
    
    try:
        db = pg8000.Connection(host='localhost', port=opt['-p'],
                               user=ENV.USER,
                               database=ENV.DBNAME,
                               options='-c gp_session_role=utility')
        
        query = """
           SELECT content, preferred_role = 'p' as definedprimary, dbid, role = 'p' as isprimary, hostname, port, fselocation as datadir
           FROM gp_segment_configuration
           JOIN pg_filespace_entry ON (dbid = fsedbid)
           WHERE fsefsoid = (SELECT oid FROM pg_filespace 
                             WHERE fsname = 'pg_system')
        """
        db.execute(query)
        gpconf = [ r for r  in db.iterate_dict() ] 

        master = filter(lambda(x): x['content'] < 0 and x['definedprimary'], gpconf)[0]
        standbymaster = filter(lambda(x): x['content'] < 0 and not x['definedprimary'], gpconf)
        primary = filter(lambda(x): x['content'] >= 0 and x['definedprimary'], gpconf)
        mirrors = filter(lambda(x): x['content'] >= 0 and not x['definedprimary'], gpconf)
        
        with open(opt['-o'], 'w') as f:
            f.write('ARRAY_NAME=GreenplumDB\n')
            f.write('QD_PRIMARY_ARRAY=%s:%d:%s:%d:%d\n' % (master['hostname'], master['port'],
                                                           master['datadir'], master['dbid'], master['content']))

            f.write('declare -a PRIMARY_ARRAY=(\n')
            for s in primary:
                f.write('%s:%d:%s:%d:%d\n' % (s['hostname'], s['port'], s['datadir'], s['dbid'], s['content']))
            f.write(')\n')

            if len(mirrors) > 0:
                f.write('declare -a MIRROR_ARRAY=(\n')
                for s in mirrors:
                    f.write('%s:%d:%s:%d:%d\n' % (s['hostname'], s['port'], s['datadir'], s['dbid'], s['content']))
                f.write(')\n')

            f.write('SEG_PREFIX=%s\n' % os.path.basename(master['datadir'])[0:-2])

    except pg8000.errors.InterfaceError, e:
        gpmlib.log_error('Database is not started')
        gpmlib.error(e)
    finally:
        if db:
            db.close()
    


#####################
try:
    parseCommandLine()
    ENV.chk_environ()
    gpconf()

    sys.exit(arg['exit'])

except KeyboardInterrupt:
    sys.exit('\nUser Interrupted')
