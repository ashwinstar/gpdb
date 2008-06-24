#!/usr/bin/env python

'''

Recovers Greenplum segment instances that are marked as invalid, if 
mirroring is configured and operational

Usage: gprecoverseg [--version] [-?qaf] [-D] [-c {rsync|ssh}] [-i input_config] [-o output_config] [-d master_data_dir] [-l log_dir] [-p hostname] [-s pri_dir,mir_dir] [-z failed_datadir:failed_host] [-B number]
           
          General options:
          --version : print version information
          -?        : print this help screen

          Logging options:
          -D        : set logging output to debug level
          -q        : quiet mode, do not log progress to screen [default: verbose output to screen]
          -a        : non-interactive mode [default: interactive mode]

          Config options:
          -i        : <input_config> input configuration file which is generated from -o option
          -o        : <output_config> output configuration file

          Recovery options:
          -l        : <log directory> alternative log file directory [optional]
          -p        : <hostname> recover to spare pool node
                          the pool host must have the following characteristics:
                          1. Same OS version
                          2. Same locale
                          3. Correct Greenplum Database software version
                          4. Inter-ssh access to active Greenplum array
                          5. Greenplum superuser account configured
                          6. Sufficient capacity left to accommodate recovered segments
          -s        : <Primary segment directory, Mirror segment directory>
                          example -s /data_p4/pri,/data_m4/mir, this assumes that the Greenplum array was
                          configured with spare pool segment instance directories on each segment instance host
          -f        : force Greenplum Database instance shutdown and restart
          -z        : <failed_segment_instance_directory_name:failed_hostname> 
                          force a recovery, even though no instance segments are marked as invalid on the master
                          CAUTION:-USE THIS OPTION WITH CARE!
                          example gprecoverseg -z /dbfast1/segment1:localhost
          -B        : <number> run this batch of recovery segment processes in parallel [default: 4]

          Connection options:
          -d        : <master data directory> name of alternative Master database to start up [optional]

          Copy options:
          -c        : {rsync|ssh}  use rsync or ssh to copy data to failed segment [default: ssh]

          Format of configuration file:
          failed_host:failed_port:failed_datadir [new_host:new_port:new_datadir]
          e.g.
              dbhost1:41023:/data1/primary/gp-0 dbhost3:41023:/data1/primary/gp-0
              dbhost2:41024:/data1/primary/gp-1 dbhost2:41025:/data2/primary/gp-1
              dbhost1:42022:/data1/mirror/gp-2

'''
from __future__ import with_statement
import os, sys, getopt, socket, StringIO
from threading import Thread
from datetime import date, datetime, timedelta

sys.path.append(sys.path[0] + '/lib')
try:
    import pg8000

    import gplib
    import gpmlib
    from gpmlib import ENV
except ImportError, e:
    gpmlib.error('[ERROR]:- unable to import module: ' + str(e))


script_name = os.path.split(__file__)[-1]
opt = {}
opt['-d'] = None
opt['-B'] = 4
opt['-z'] = None
opt['-a'] = False
opt['-s'] = None
opt['-p'] = None
opt['-f'] = False
opt['-q'] = False
opt['-l'] = None
opt['-i'] = None
opt['-o'] = None
opt['-D'] = False
opt['-c'] = None

arg = {}
arg['exit'] = 0

################
def usage(exitarg):
    print gplib.cli_help() or __doc__
    sys.exit(exitarg)

################
def print_version():
    print '%s version $Revision$' % script_name
    sys.exit(0)

################
def parseCommandLine():
    global opt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], '?Dqafs:d:z:B:p:l:c:i:o:', ['version'])
    except Exception, e:
        usage('[ERROR]:-' + str(e))

    for (switch, val) in options:
        if (switch == '-?'):          usage(0)
        elif (switch == '-d'):        opt[switch] = val
        elif (switch == '-B'):        opt[switch] = int(val)
        elif (switch == '-z'):        opt[switch] = val
        elif (switch == '-a'):        opt[switch] = True
        elif (switch == '-s'):        opt[switch] = val
        elif (switch == '-p'):        opt[switch] = val
        elif (switch == '-f'):        opt[switch] = True
        elif (switch == '-q'):        opt[switch] = True
        elif (switch == '-l'):        opt[switch] = val
        elif (switch == '-c'):        opt[switch] = val
        elif (switch == '-i'):        opt[switch] = val
        elif (switch == '-o'):        opt[switch] = val
        elif (switch == '-D'):        opt[switch] = True

    gpmlib.log_set_module(script_name)
    # set verbose
    if opt['-q']:
        gpmlib.log_set_verbose(False)

    if opt['-l']:
        if not gpmlib.directory_exists(opt['-l']):
            gpmlib.error('log directory %s not exist' % opt['-l'])

        opt['-l'] = os.path.join(opt['-l'], '%s_%s.log' % (script_name, date.today().strftime('%Y%m%d')))
        gpmlib.log_set_file(opt['-l'])
    else:
        logdir = os.path.join(os.environ.get('HOME'), 'gpAdminLogs')
        if not gpmlib.directory_exists(logdir):
            try:
                os.mkdir(logdir)
            except OSError, e:
                gpmlib.error('mkdir %s failed' % logdir)

        opt['-l'] = os.path.join(logdir, '%s_%s.log' % (script_name, date.today().strftime('%Y%m%d')))
        gpmlib.log_set_file(opt['-l'])

    if not opt['-d']:
        opt['-d'] = os.getenv('MASTER_DATA_DIRECTORY')
        if not opt['-d']:
            gpmlib.error('MASTER_DATA_DIRECTORY not found')

    if not os.path.isdir(opt['-d']):
        gpmlib.error('MASTER_DATA_DIRECTORY %s not exists or not a directory' % opt['-d'])

    if not opt['-c']:
        opt['-c'] = 'ssh'
    if opt['-c'] != 'rsync' and opt['-c'] != 'ssh':
        gpmlib.error('Only support ssh or rsync to copy data to remote host.  Please specify -c rsync or -c ssh.')

    if opt['-c'] == 'rsync':
        gpmlib.findCmdInPath('rsync')

    
################
def yesorno(msg, abortmsg):
    print msg
    line = sys.stdin.readline()
    line = line.strip().lower()
    if line == 'y':
        return True
    print abortmsg
    return False


################
def info_text():
    gpmlib.log_info('To be able to progress this issue, complete the following steps')
    gpmlib.log_info('    Step 1 Shutdown the Greenplum Database instance with gpstop -zfa')
    gpmlib.log_info('    Step 2 Edit the Master postgresql.conf file and set gp_fault_action=readonly')
    gpmlib.log_info('    Step 3 Restart the Greenplum Database instance gpstart -a')
    gpmlib.log_info('    Step 4 Re-run this utility')

################
def gp_fault_action(db):
    db.execute('show gp_fault_action')
    fault_action_row = db.read_tuple()
    return fault_action_row[0]
    

################
#   config file 
#   host:port:data_dir

def parseMirrorConfig():
    file = None
    lines = []
    try:
        try:
            file = open(opt['-m'], 'r')
            lines = file.readlines()
        except Exception, e:
            gpmlib.error('read file %s error' % opt['-m'])
    finally:
        if file:
            file.close()

    mirrors = []
    for l in lines:
        l = l.strip()
        if l == '':
            continue
        mirror = l.split(':')
        if len(mirror) != 3:
            gpmlib.error('mirror config format error')
        m = {}
        m['hostname'] = mirror[0]
        m['port'] = int(mirror[1])
        m['datadir'] = mirror[2]
        mirrors.append(m)
        
    return mirrors
        
################
def get_minmax_port(db):
    db.execute("select max(port) from gp_configuration where content<>-1 and definedprimary='t'")
    row = db.read_tuple()
    max_pri_port = row[0]    
    db.execute("select min(port) from gp_configuration where content<>-1 and definedprimary='t'")
    row = db.read_tuple()
    min_pri_port = row[0]    
    db.execute("select max(port) from gp_configuration where content<>-1 and definedprimary='f'")
    row = db.read_tuple()
    max_mir_port = row[0]    
    db.execute("select min(port) from gp_configuration where content<>-1 and definedprimary='f'")
    row = db.read_tuple()
    min_mir_port = row[0]    
    return (min_pri_port, max_pri_port, min_mir_port, max_mir_port)

################
def get_fail_count_type(db, failed_dbids):
    pri_fail_count = 0
    mir_fail_count = 0

    for dbid in failed_dbids:
        db.execute("select definedprimary from %s as _gp_pg_view where dbid=%d" % (ENV.GP_PG_VIEW, dbid))
        r = db.read_tuple()
        isPrimary = r[0]

        if isPrimary:
            pri_fail_count += 1
        else:
            mir_fail_count += 1

    gpmlib.log_info('Number of primary segments failed = %d' % pri_fail_count)
    gpmlib.log_info('Number of mirror segments failed  = %d' % mir_fail_count)

    return (pri_fail_count, mir_fail_count)

################
def calc_mirror_port_base(db):
    global opt
    max_mir_port = 61000
    min_mir_port = 7000

    mirror_offset = opt['-p']
    (min_port, max_port) = get_minmax_port(db)
    if max_port > 40000:
        port_base = min_port - mirror_offset-1
    else:
        port_base = min_port + mirror_offset-1

    if port_base >= max_mir_port or port_base <= min_mir_port:
        gpmlib.log_fatal(' Value of MIRROR_OFFSET supplied via -a option is out of valid')
        gpmlib.fatal('Mirror port base range must be between %d and %d' % (min_mir_port, max_mir_port))


################
def chk_mirror_config(qes, mirrors):
    if len(qes) != len(mirrors):
        gpmlib.error('Number of mirror segments must be equal to number of primary segments')

    max_dbid = max([ qe['dbid'] for qe in qes ])
    dbid = max_dbid + 1
    gp_prefix = gpmlib.get_gp_prefix(os.environ.get('MASTER_DATA_DIRECTORY'))
    if not gp_prefix:
        gp_prefix = 'gp-'

    config = []
    for i in range(len(qes)):
        mir = mirrors[i]
        mir['dbid'] = dbid
        mir['content'] = qes[i]['content']
        mir['datadir'] = os.path.join(mir['datadir'], '%s%d' % (gp_prefix, mir['content']))
        config.append(mir)
        dbid += 1

    pri_hosts = gpmlib.unique([ qe['hostname'] for qe in qes ])
    mir_hosts = gpmlib.unique([ m['hostname'] for m in mirrors ])
    new_hosts = []
    for h in mir_hosts:
        if h not in pri_hosts:
            new_hosts.append(h)

    new_addr = []
    for h in new_hosts:
        ips = gpmlib.get_ipaddr(h)
        if not ips:
            gpmlib.error('Get IP Address from %s failed' % h)

        new_addr.extend(ips)
    new_addr = gpmlib.unique(new_addr)

    return (config, new_addr)

################
def chk_mirror_dir(qes, mirrors):
    for i in range(len(qes)):
        pri_host = qes[i]['hostname']
        mirror_host = mirrors[i]['hostname']
        mirror_datadir = mirrors[i]['datadir']

        # check datadir exist
        if gpmlib.directory_exists(mirror_datadir, mirror_host):
            gpmlib.error('Directory %s already exists on %s' % (mirror_datadir, mirror_host))

        # check write access of the parent datadir
        dir = os.path.dirname(mirror_datadir)
        tmpfile = os.path.join(dir, 'gptmpfile')
        (ok, out) = gpmlib.run('%s "%s %s"' % (gplib.ssh_prefix(host=mirror_host), ENV.TOUCH, tmpfile))
        if ok:
            (ok, out) = gpmlib.run('%s "%s -f %s"' % (gplib.ssh_prefix(host=mirror_host), ENV.RM, tmpfile))            
        else:
            gpmlib.error('Cannot write to %s on segment host %s' % (dir, mirror_host))

        # check access from primary host to mirror host
        (ok, out) = gpmlib.run('%s \'%s ls /tmp\'' % (gplib.ssh_prefix(host=pri_host),
                                                       gplib.ssh_prefix(host=mirror_host)))
        if not ok:
            gpmlib.error('Check inter host ssh access from %s to %s failed' % (pri_host, mirror_host))

    gpmlib.log_info('Completed host and directory checks')
        
################
#
#  opt['-p']
#
################
def chk_pool_host(db, failed_dbids):
    global opt

    if not gpmlib.ping_host(opt['-p']):
        gpmlib.fatal('Issue with network access to %s' % opt['-p'])
    else:
        gpmlib.log_info('Established network connection to %s' % opt['-p'])

    gpmlib.log_info('Checking ssh access as user %s to %s' % (ENV.USER, opt['-p']))
    (ok, out) = gpmlib.run('%s "ls /tmp > /dev/null"' % (gplib.ssh_prefix(host=opt['-p'])))
    if not ok:
        gpmlib.error('ssh access check to %s failed' % opt['-p'])

    if gpmlib.directory_exists(os.environ.get('GPHOME'), opt['-p']):
        gpmlib.log_info('Check for directory %s passed' % os.environ.get('GPHOME'))
    else:
        gpmlib.fatal('Issue with %s directory on %s' % (os.environ.get('GPHOME'), opt['-p']))

    initdb = os.path.join(os.environ.get('GPHOME'), 'bin/initdb')
    (ok, out) = gpmlib.run('%s -V'  % initdb)
    if not ok:
        gpmlib.fatal('initdb not found in $GPHOME')
    
    qd_initdb_version = out[0]

    (ok, out) = gpmlib.run('%s %s -V' % (gplib.ssh_prefix(host=opt['-p']), initdb))
    if not ok:
        gpmlib.error('[FATAL]:- initdb not found in $GPHOME')
    pool_initdb_version = out[0]
    
    if qd_initdb_version != pool_initdb_version:
        gpmlib.error('[FATAL]:- initdb version mismatch between master and %s' % opt['-p'])
    else:
        gpmlib.log_info('initdb version check passed')

    (pri_fail_count, mir_fail_count) = get_fail_count_type(db, failed_dbids)
    
    # Check for directores in use
    db.execute("select datadir from gp_configuration where hostname='%s' and definedprimary='t'" %  (opt['-p']))
    pri_pool_host_in_use = [ r[0] for r in db.iterate_tuple() ]
    gpmlib.log_info('Pool host %s active primary segment count = %d' % (opt['-p'], len(pri_pool_host_in_use)))

    db.execute("select datadir from gp_configuration where hostname='%s' and definedprimary='f'" %  (opt['-p']))    
    mir_pool_host_in_use = [ r[0] for r in db.iterate_tuple() ]
    gpmlib.log_info('Pool host %s active mirror segment count = %d' % (opt['-p'], len(mir_pool_host_in_use)))
    
    # Get number of primary and mirror segments on a standard host
    db.execute("select count(distinct port) from gp_configuration where definedprimary='t' and content<>-1")
    r = db.read_tuple()
    pool_host_limit = r[0]

    gpmlib.log_info('Pool host segment limit is %d' % pool_host_limit)

    pri_free = pool_host_limit - len(pri_pool_host_in_use)
    if pri_free < pri_fail_count:
        gpmlib.log_fatal('There are insufficient spare primary pool directories')
        gpmlib.log_fatal('Required=%d Free=%d Used=%d' % (pri_fail_count, pri_free, len(pri_pool_host_in_use)))
        gpmlib.log_info('Consider using gprecoverseg with -s option')
        gpmlib.error('[FATAL]:- Unable to continue')
    else:
        gpmlib.log_info('Sufficient spare primary pool directories')
        gpmlib.log_info('Required=%d Free=%d Used=%d' % (pri_fail_count, pri_free, len(pri_pool_host_in_use)))

    mir_free = pool_host_limit - len(mir_pool_host_in_use)
    if mir_free < mir_fail_count:
        gpmlib.log_fatal('There are insufficient spare mirror pool directories')
        gpmlib.log_fatal('Required=%d Free=%d Used=%d' % (mir_fail_count, mir_free, len(mir_pool_host_in_use)))
        gpmlib.log_info('Consider using gprecoverseg with -s option')
        gpmlib.error('[FATAL]:- Unable to continue')
    else:
        gpmlib.log_info('Sufficient spare mirror pool directories')
        gpmlib.log_info('Required=%d Free=%d Used=%d' % (mir_fail_count, mir_free, len(mir_pool_host_in_use)))

    seg_prefix = gpmlib.get_gp_prefix(os.environ.get('MASTER_DATA_DIRECTORY'))
    gpmlib.log_info('Greenplum instance segment prefix is %s' % seg_prefix)
    gpmlib.log_info('Obtaining a list of primary segment datadir prefixes')

    db.execute("select datadir from gp_configuration where content<>-1 and definedprimary='t'")
    pri_datadir_prefix = [ os.path.dirname(x[0]) for x in db.iterate_tuple()]
    pri_datadir_prefix = gpmlib.unique(pri_datadir_prefix)

    db.execute("select datadir from gp_configuration where content<>-1 and definedprimary='f'")
    mir_datadir_prefix = [ os.path.dirname(x[0]) for x in db.iterate_tuple()]
    mir_datadir_prefix = gpmlib.unique(mir_datadir_prefix)
    
    for mir_dir in mir_datadir_prefix:
        gpmlib.directory_writable(mir_dir, opt['-p'])
                    
    return (pri_pool_host_in_use, mir_pool_host_in_use, pri_datadir_prefix, mir_datadir_prefix)
                    
##################
#
#  opt['-p']
#
##################
def pool_host_dir_validation(db, failed_instance_array, pri_pool_host_in_use, mir_pool_host_in_use,
                             pri_datadir_prefix, mir_datadir_prefix):
    global opt


    (min_pri_port, max_pri_port, min_mir_port, max_mir_port) =  get_minmax_port(db)

    pool_failed_instance_array = []
    pool_port_in_use = []
    update_sql = []
    for failed_qe in failed_instance_array:
        seg_type = failed_qe['definedprimary']
        if seg_type:
            # primary segment
            new_host = opt['-p']
            if len(pri_pool_host_in_use) != 0:
                dir_chk_list = pri_datadir_prefix
                dir_used_list = pri_pool_host_in_use
                new_dir = get_free_dir(failed_qe, dir_used_list, dir_chk_list)
            else:
                new_dir = failed_qe['datadir']
            pri_pool_host_in_use.append(new_dir)
            new_port = get_free_port(db, failed_qe, new_host, min_mir_port, max_mir_port, pool_port_in_use)
            pool_port_in_use.append(new_port)
        else:
            # mirror segment
            new_host = opt['-p']
            if len(mir_pool_host_in_use) != 0:
                dir_chk_list = mir_datadir_prefix
                dir_used_list = mir_pool_host_in_use
                new_dir = get_free_dir(failed_qe, dir_used_list, dir_chk_list)
            else:
                new_dir = failed_qe['datadir']
            mir_pool_host_in_use.append(new_dir)
            new_port = get_free_port(db, failed_qe, new_host, min_mir_port, max_mir_port, pool_port_in_use)
            pool_port_in_use.append(new_port)

        # FIXME: save the update for future use
        update_sql.append((new_host, new_dir, new_port, failed_qe['dbid']))
        
        a = {}
        a['hostname'] = new_host
        a['port'] = new_port
        a['datadir'] = new_dir
        a['dbid'] = failed_qe['dbid']
        a['content'] = failed_qe['content']
        a['definedprimary'] = failed_qe['definedprimary']
        pool_failed_instance_array.append(a)

    return (update_sql, pool_failed_instance_array)

################
def get_free_dir(failed_qe, dir_used_list, dir_chk_list):
    global opt

    content = failed_qe['content']
    dir = failed_qe['datadir']

    gpmlib.log_info('Checking to see if %s in use on %s' % (dir, opt['-p']))
    qe_dirname = os.path.dirname(dir)

    found = False
    for p in dir_used_list:
        if p.startswith(qe_dirname):
            found = True

    if found:
        gpmlib.log_info('Directory %s on pool host already allocated, looking for alternative' % qe_dirname)
        for d in dir_chk_list:
            found = False
            for p in dir_used_list:
                if p.startswith(d):
                    found = True

            if found:
                gpmlib.log_info('Directory %s used, looking for alternative' % d)
            else:
                prefix = gpmlib.get_gp_prefix(os.environ.get('MASTER_DATA_DIRECTORY'))
                new_dir = os.path.join(d, '%s%d' % (prefix, content))
                return new_dir
    else:
        gpmlib.log_info('Original directory not in use, will re-use %s' % qe_dirname)
        return dir

################
def get_free_port(db, failed_qe, host, min_port, max_port, port_in_use):

    db.execute("select count(*) from gp_configuration where hostname=$1 and port=$2", 
               host, failed_qe['port'])
    r = db.read_tuple()
    free_count = r[0]
    
    found = False
    for p in port_in_use:
        if p == failed_qe['port']:
            found = True

    if free_count == 0 and not found:
        gpmlib.log_info('Failed port %d not in use on %s, will re-use' % (failed_qe['port'], host))
        free_port = failed_qe['port']
    else:
        free_port = 0
        for p in range(min_port, max_port):
            db.execute("select count(*) from gp_configuration where hostname=$1 and port=$2", 
                       host, p)
            r = db.read_tuple()
            free_count = r[0]
            gpmlib.log_info('Free port count for port=%d is %d' % (p, free_count))

            found = False
            for pp in port_in_use:
                if pp == p:
                    found = True

            if free_count == 0 and not found:
                free_port = p
        
    return free_port



################
# multi-thread running data sychronization
class SyncDataThread(Thread):
    def __init__(self, failed_qe, active_qe, pool_array, timeout, logfile):
        Thread.__init__(self)
        self.failed_qe = failed_qe
        self.active_qe = active_qe
        self.pool_array = pool_array
        self.status = False
        self.logfile = logfile
        self.timeout = datetime.today() + timedelta(seconds=timeout)

    def run(self):
        global opt

        # kill failed_qe process

        if True or self.failed_qe != self.pool_array:
            # remove database directory
            gpmlib.log_info('Delete data directory %s on %s' % (self.failed_qe['datadir'], self.failed_qe['hostname']))
            (ok, out) = gpmlib.run('%s rm -rf %s %s.log' % (gplib.ssh_prefix(host=self.failed_qe['hostname']),
                                                            self.failed_qe['datadir'], self.failed_qe['datadir']))
            if not ok:
                gpmlib.log_warn('Issue with forced delete %s or %s.log on %s' % 
                                (self.failed_qe['datadir'],
                                 self.failed_qe['datadir'],
                                 self.failed_qe['hostname']))
            

        # sychronize pool_array
        gpmlib.log_info('Start to synchronize data between primary segment (%s:%s) and mirror segment (%s:%s)' % 
                        (self.active_qe['hostname'], self.active_qe['datadir'], 
                         self.pool_array['hostname'], self.pool_array['datadir']))

        if not gpmlib.sync_segment_datadir(self.active_qe['hostname'], 
                                           self.active_qe['datadir'],
                                           self.pool_array,
                                           self.logfile,
                                           force_stop = opt['-f'],
                                           copy = opt['-c']):
            gpmlib.log_error(' failed to copy data from (%s:%s) to (%s:%s)' % 
                             (self.active_qe['hostname'], self.active_qe['datadir'], 
                              self.pool_array['hostname'], self.pool_array['datadir']))
            return

        self.status = True

    def timed_out(self):
        return datetime.today() > self.timeout

################
def recover_data(failed_instance_array, active_instance_array, pool_failed_instance_array):
    global opt

    timeout = 1800
    error = []
    kill = []
    thlist = []
    n = 0
    gpmlib.log_info('Commencing parallel segment instance startup, please wait...')
    gpmlib.log_info('Time out value = %d seconds' % timeout)

    for i in range(len(failed_instance_array)):
        failed_qe = failed_instance_array[i]
        active_qe = active_instance_array[i]

        if pool_failed_instance_array:
            pool_array = pool_failed_instance_array[i]
        else:
            pool_array = failed_qe

        th = SyncDataThread(failed_qe, active_qe, pool_array, timeout, opt['-l'])
        thlist.append(th)
        th.start()

        if len(thlist) == opt['-B']:
            n += 1
            gpmlib.log_info('Spawning parallel processes Batch[%d], please wait...' % n)
            
            while len(thlist) > 0:
                tmplist = []
                while len(thlist) > 0:
                    th = thlist.pop()
                    th.join(2)
                    if th.isAlive():
                        if th.timed_out():
                            # python no thread.kill, just ignore the thread now
                            kill.append((th.mir_cfg['hostname'], th.mir_cfg['datadir']))
                            gpmlib.log_error('Thread (%s, %s) timed out' % (th.mir_cfg['hostname'], th.mir_cfg['datadir']))
                        else:
                            tmplist.append(th)
                        sys.stdout.write('.')
                        sys.stdout.flush()
                    else:
                        if not th.status:
                            error.append((th.failed_qe['hostname'], th.failed_qe['datadir']))

                if len(tmplist) > 0:
                    thlist = tmplist
            thlist = []

    if len(thlist) > 0:
        n += 1
        gpmlib.log_info('Spawning parallel processes Batch[%d], please wait...' % n)

        while len(thlist) > 0:
            tmplist = []
            while len(thlist) > 0:
                th = thlist.pop()
                th.join(2)
                if th.isAlive():
                    if th.timed_out():
                        # python no thread.kill, just ignore the thread now                       
                        kill.append((th.mir_cfg['hostname'], th.mir_cfg['datadir'])) 
                        gpmlib.log_error('Thread (%s, %s) timed out' % (th.mir_cfg['hostname'], th.mir_cfg['datadir']))
                    else:
                        tmplist.append(th)
                    sys.stdout.write('.')
                    sys.stdout.flush()
                else:
                    if not th.status:
                        error.append((th.failed_qe['hostname'], th.failed_qe['datadir']))

            if len(tmplist) > 0:
                thlist = tmplist

    sys.stdout.write('\n')
    sys.stdout.flush()

    completed = len(failed_instance_array) - len(error) - len(kill)
    killed = len(kill)
    failed = len(error)
    gpmlib.log_info('-------------------------------------------')
    gpmlib.log_info('Parallel process exit status')
    gpmlib.log_info('-------------------------------------------')
    gpmlib.log_info('Total processes marked as completed  = %d' % completed)
    gpmlib.log_info('Total processes marked as killed     = %d' % killed)
    gpmlib.log_info('Total processes marked as failed     = %d' % failed)
    gpmlib.log_info('-------------------------------------------')

    if failed + killed > 0:
        return (False, completed, killed, failed, len(failed_instance_array))

    return (True, completed, killed, failed, len(failed_instance_array))

################
def update_gp_conf(mirrors):
    db = None

    # start master in admin mode
    (ok, out) = gpmlib.run('gpstart -qam > /dev/null 2>&1')
    if not ok:
        gpmlib.error('start master in admin mode failed')

    try:
        db = pg8000.Connection(host='localhost', port=master_port, 
                               user=ENV.USER, database=ENV.DBNAME, 
                               options='-c gp_session_role=utility')

        # update Greenplum database configuration
        for m in mirrors:
            db.execute("insert into gp_configuration values($1, $2, $3, $4, $5, $6, $7, $8)",
                       m['content'], False, m['dbid'], False, True, 
                       m['hostname'], m['port'], m['datadir'])
        
    except pg8000.errors.InterfaceError, e:
        gpmlib.error(e)
    except pg8000.errors.ProgrammingError, e:
        gpmlib.error(e)

    if db:
        db.close()
        db = None

    # stop master
    (ok, out) = gpmlib.run('gpstop -qma > /dev/null 2>&1')
    if not ok:
        gpmlib.error('stop master in admin mode failed')

################
def display_param(fault_action, failed_array, active_array, multi_home):
    global opt, arg

    gpmlib.log_info('Greenplum instance recovery parameters')
    gpmlib.log_info('---------------------------------------------------------')
    gpmlib.log_info('Fault tolerance setting       = %s' % fault_action)
    if opt['-i']:
        gpmlib.log_info('Recovery from configuration -i option supplied')

    if opt['-z']:
        gpmlib.log_info('Forced Recovery -z option supplied')

    if opt['-p']:
        gpmlib.log_info('Recovery type             = Pool Host')
        gpmlib.log_info('Pool host for recovery    = %s' % opt['-p'])
        type_text = 'Pool    '
    if opt['-s']:
        dir = opt['-s'].split(',')
        gpmlib.log_info('Recovery type             = Pool Directory')
        gpmlib.log_info('Primary pool directory    = %s' % dir[0])
        gpmlib.log_info('Mirror pool directory     = %s' % dir[1])
        type_text = 'Pool dir'

    if opt['-p'] is None and opt['-s'] is None:
        gpmlib.log_info('Recovery type             = Standard')
        type_text = 'Failed  '

    count = 0
    gpmlib.log_info('Recovery parallel batch value = %d' % opt['-B'])


    if arg['exit'] != 0:
        gpmlib.log_warn('Warnings have been generated      <<<<<<<<<<<<<<<<<')
        gpmlib.log_info('But recovery can continue')

    for i in range(len(failed_array)):
        gpmlib.log_info('---------------------------------------------------------')
        gpmlib.log_info('Recovery %d of %d' % (i+1, len(failed_array)))
        gpmlib.log_info('---------------------------------------------------------')

        gpmlib.log_info('%s instance host              = %s' % (type_text, failed_array[i]['hostname']))
        gpmlib.log_info('%s instance directory         = %s' % (type_text, failed_array[i]['datadir']))
        gpmlib.log_info('%s instance port              = %d' % (type_text, failed_array[i]['port']))
        gpmlib.log_info('Recovery instance host        = %s' % (active_array[i]['hostname']))
        gpmlib.log_info('Recovery instance directory   = %s' % (active_array[i]['datadir']))
        gpmlib.log_info('Recovery instance port        = %s' % (active_array[i]['port']))

    gpmlib.log_info('---------------------------------------------------------')          
    if fault_action == 'continue':
        if opt['-f']:
            gpmlib.log_info('Procedure will STOP database as gp_fault_action=%s' % fault_action)
        else:
            gpmlib.log_fatal('Procedure cannot continue without database interruption')
            gpmlib.log_fatal('as gp_fault_action=%s' % fault_action)
            gpmlib.fatal('Unable to continue')
    else:
        if opt['-f']:
            gpmlib.log_info('Procedure will STOP database as -f option supplied')
        else:
            gpmlib.log_info('Procedure can continue without database interruption')
            gpmlib.log_info('as gp_fault_action=%s' % fault_action)
    gpmlib.log_info('---------------------------------------------------------')

################                                      
def get_datadirs(ndatadir):
    
    datadirs = []
    for i in range(ndatadir):
        print 'Enter mirror segment data directory location %d of %d >' % (i+1, ndatadir)
        line = sys.stdin.readline() 
        line = line.strip()
        if line == '':
            gpmlib.error('[FATAL]:- Null value entered for mirror data directory')
        datadirs.append(line)

    return datadirs

################                                      
def get_failed_dbids(db):
    db.execute("select dbid from %s as _gp_pg_view where valid=$1" % ENV.GP_PG_VIEW, False)
    dbids = [r[0] for r in db.iterate_tuple()]
    return dbids

################                                      
def chk_separate_host(db, mir_type_num, failed_dbids):
    global opt, arg

    if mir_type_num == 3 and opt['-p']:
        # group mirror type is separate hosts, check to see if pool host is active
        db.execute("select distinct(definedprimary) from gp_configuration where hostname='%s'" % opt['-p'])
        type_active = [ r[0] for r in db.iterate_tuple() ]
        if len(type_active) == 0:
            gpmlib.log_info('Mirror type is separate hosts, no active segments on pool host')
        elif len(type_active) == 1:
            gpmlib.log_info('Mirror type is separate hosts, active type on pool host = %s' % str(type_active[0]))
            pri_count = 0
            mir_count = 0
            
            for dbid in failed_dbids:
                db.execute("select definedprimary from gp_configuration where dbid=%d" % dbid)
                r = db.read_tuple()
                if r[0]:
                    pri_count += 1
                else:
                    mir_count += 1

            if type_active[0] and mir_count > 0:
                gpmlib.log_info('***************************************************')
                gpmlib.log_warn('About to locate failed primary to pool host with mirror segments')
                gpmlib.log_warn('Current array mirror type is separate hosts')
                gpmlib.log_warn('Recovery will break this mirror configuration')
                gpmlib.log_info('***************************************************')
                arg['exit'] = 1

            if type_action[0] == False and pri_count > 0:
                gpmlib.log_info('***************************************************')
                gpmlib.log_warn('About to locate failed primary to pool host with mirror segments')
                gpmlib.log_warn('Current array mirror type is separate hosts')
                gpmlib.log_warn('Recovery will break this mirror configuration')
                gpmlib.log_info('***************************************************')
                arg['exit'] = 1
        else:
            gpmlib.log_warn('Mirror type is separate hosts.')
            gpmlib.log_warn('Have primary and mirror segment instances active on pool host')
            arg['exit'] = 1

################                                      
def get_mirror_details(db, failed_dbids):
    gpmlib.log_info(' Obtaining recovery information')

    failed_instance_array = []
    active_instance_array = []

    for dbid in failed_dbids:
        db.execute("select definedprimary, hostname, port, datadir, dbid, content from gp_configuration where dbid=%d" % dbid)
        r = db.read_dict()
        failed_instance_array.append(r)

        db.execute('''select dbid from %s as _gp_pg_view where content in 
                      (select content from %s as _gp_pg_view2 where dbid = %d) 
                      and dbid <> %d''' % (ENV.GP_PG_VIEW, ENV.GP_PG_VIEW, dbid, dbid))
        r = db.read_tuple()
        active_dbid = r[0]

        db.execute('''select definedprimary, hostname, port, datadir, dbid, content from gp_configuration 
                      where valid='t' and 
                      content in (select content from gp_configuration where dbid=%d) 
                      and dbid <> %d''' % (active_dbid, dbid))

        r = db.read_dict()
        active_instance_array.append(r)

    return (failed_instance_array, active_instance_array)

################                                      
def validate_params():
    global opt

    if opt['-p'] and opt['-s']:
        gpmlib.fatal('Cannot supply -s and -p options at same time')

    if opt['-p']:
        if socket.gethostname() == opt['-p']:
            gpmlib.fatal('Pool host cannot be master instance hostname')

    if opt['-s']:
        dir = opt['-s'].split(',')
        if len(dir) != 2:
            gpmlib.fatal('Expected dir,dir for -s option')

        if dir[0].startswith('/'):
            gpmlib.fatal('Primary directory not prefixed by /')

        if dir[1].startswith('/'):
            gpmlib.fatal('Mirror directory not prefixed by /')


################
def chk_pool_dir_available(db, failed_dbids):
    global opt

    dir = opt['-s'].split(',')
    if len(dir) != 2:
        gpmlib.error('[FATAL]:- option -s %s format error' % opt['-s'])
    pri_pool_dir = dir[0]
    mir_pool_dir = dir[1]

    gpmlib.log_info('Primary segment pool directory set as %s' % pri_pool_dir)
    gpmlib.log_info('Mirror segmet pool directory set as %s' % mir_pool_dir)
    seg_prefix = gpmlib.get_gp_prefix(os.environ.get('MASTER_DATA_DIRECTORY'))
    gpmlib.log_info('Greenplum instance segment prefix is %s' % seg_prefix)

    db.execute('''select hostname, port, datadir, dbid from gp_configuration where 
                  datadir like '%s%%' and content<>-1''' % 
               (os.path.join(pri_pool_dir, seg_prefix)))

    active_pri_pool_array = [ x for x in db.iterate_dict() ]
    gpmlib.log_info('There are %d primary directory pools active' % len(active_pri_pool_array))
    
    db.execute('''select hostname, port, datadir, dbid from gp_configuration where 
                  datadir like '%s%%' and content<>-1''' % 
               (os.path.join(mir_pool_dir, seg_prefix)))

    active_mir_pool_array = [ x for x in db.iterate_dict() ]
    gpmlib.log_info('There are %d mirror directory pools active' % len(active_mir_pool_array))    

    # maximum pool directory usage will be defined by number of primary QE hosts in the array
    db.execute("select count(distinct(hostname)) from gp_configuration where content<>-1 and definedprimary='t'")
    r = db.read_tuple()
    pool_dir_limit = r[0]

    gpmlib.log_info('Maximum pool usage limit is %d for this Greenplum instance' % pool_dir_limit)

    (pri_fail_count, mir_fail_count) = get_fail_count_type(db, failed_dbids)

    if pri_fail_count != 0:
        pri_free = pool_dir_limit - len(active_pri_pool_array)
        if pri_free < pri_fail_count:
            gpmlib.log_fatal('There are insufficient spare primary pool directories')
            gpmlib.log_fatal('Required=%d Free=%d Used=%d' %
                             (pri_fail_count, pri_free, len(active_pri_pool_array)))
            gpmlib.log_info('Consider using a pool host with -p option to this utility')
            gpmlib.log_info('Or supply different directory names to -s option, if available')
            gpmlib.log_info('If host with failed segment instance still online try')
            gpmlib.log_info('gprecoverseg without any options to attempt recovery')
            gpmlib.log_info('to original segment instance directory')
            gpmlib.fatal('Unable to continue')
        else:
            gpmlib.log_info('Sufficient spare primary pool directories')
            gpmlib.log_info('Required=%d Free=%d Used=%d' % 
                            (pri_fail_count, pri_free, len(active_pri_pool_array)))

    if mir_fail_count != 0:
        mir_free = pool_dir_limit - len(active_mir_pool_array)
        if mir_free < mir_fail_count:
            gpmlib.log_fatal('There are insufficient spare mirror pool directories')
            gpmlib.log_fatal('Required=%d Free=%d Used=%d' %
                             (mir_fail_count, mir_free, len(active_mir_pool_array)))
            gpmlib.log_info('Consider using a pool host with -p option to this utility')
            gpmlib.log_info('Or supply different directory names to -s option, if available')
            gpmlib.log_info('If host with failed segment instance still online try')
            gpmlib.log_info('gprecoverseg without any options to attempt recovery')
            gpmlib.log_info('to original segment instance directory')
            gpmlib.fatal('Unable to continue')
        else:
            gpmlib.log_info('Sufficient spare mirror pool directories')
            gpmlib.log_info('Required=%d Free=%d Used=%d' % 
                            (mir_fail_count, mir_free, len(active_mir_pool_array)))
                            



################
def pool_dir_validation(db, failed_instance_array):
    global opt

    dir = opt['-s'].split(',')
    pri_pool_dir = dir[0]
    mir_pool_dir = dir[1]
    seg_prefix = gpmlib.get_gp_prefix(os.environ.get('MASTER_DATA_DIRECTORY'))

    (min_pri_port, max_pri_port, min_mir_port, max_mir_port) =  get_minmax_port(db)

    db.execute('''select distinct(hostname) from gp_configuration
                where content<>-1 and definedprimary='t' order by hostname''')
    pri_pool_dir_host_array = [ x[0] for x in db.iterate_tuple() ]

    db.execute('''select distinct(hostname) from gp_configuration
                where content<>-1 and definedprimary='f' order by hostname''')
    mir_pool_dir_host_array = [ x[0] for x in db.iterate_tuple() ]
    
    pri_pool_dir_host_chk_array = []
    for host in pri_pool_dir_host_array:
        gpmlib.log_info('Checking access to primary host %s' % host)
        if gpmlib.ping_host(host):
            pri_pool_dir_host_chk_array.append(host)
        else:
            gpmlib.log_warn('Failed to establish network contact with %s' % host)

    mir_pool_dir_host_chk_array = []
    for host in mir_pool_dir_host_array:
        gpmlib.log_info('Checking access to mirror host %s' % host)
        if gpmlib.ping_host(host):
            mir_pool_dir_host_chk_array.append(host)
        else:
            gpmlib.log_warn('Failed to establish network contact with %s' % host)

    # process the failed instance list
    # FIXME: save the update sql for future use
    pri_used = []
    mir_used = []
    pool_failed_instance_array = []
    update_sql = []
    for failed_qe in failed_instance_array:
        if failed_qe['definedprimary']:
            # primary segment
            new_host = get_host(db, failed_qe, pri_pool_dir_host_chk_array, 
                                mir_pool_dir_host_chk_array, pri_used, mir_used)
            new_dir = os.path.join(pri_pool_dir, '%s%d' % (seg_prefix, failed_qe['content']))
            new_top_dir = pri_pool_dir
            new_port = get_free_port(db, failed_qe, new_host, min_pri_port, max_pri_port, [])
            if new_port == 0:
                new_port = max_pri_port + 1
            pri_used.append(new_host)
        else:
            new_host = get_host(db, failed_qe, pri_pool_dir_host_chk_array, 
                                mir_pool_dir_host_chk_array, pri_used, mir_used)
            new_dir = os.path.join(mir_pool_dir, '%s%d' % (seg_prefix, failed_qe['content']))
            new_top_dir = mir_pool_dir
            new_port = get_free_port(db, failed_qe, new_host, min_mir_port, max_mir_port, [])
            if new_port == 0:
                new_port = max_mir_port + 1
            mir_used.append(new_host)            

        # FIXME: save the update for future use
        update_sql.append((new_host, new_dir, new_port, failed_qe['dbid']))

        a = {}
        a['hostname'] = new_host
        a['port'] = new_port
        a['datadir'] = new_dir
        a['dbid'] = failed_qe['dbid']
        a['content'] = failed_qe['content']
        a['definedprimary'] = failed_qe['definedprimary']
        pool_failed_instance_array.append(a)        

    return (update_sql, pool_failed_instance_array)

        
################
def get_host(db, failed_qe, pri_pool_dir_host_chk_array,
             mir_pool_dir_host_chk_array, pri_used, mir_used):

    global opt

    dir = opt['-s'].split(',')
    pri_pool_dir = dir[0]
    mir_pool_dir = dir[1]

    db.execute("select hostname from gp_configuration where content=$1 and definedprimary<> $2",
               failed_qe['content'], failed_qe['definedprimary'])
    r = db.read_tuple()
    partner_host = r[0]

    gpmlib.log_info('Partner host for %s is %s' % (failed_qe['hostname'], partner_host))
    new_host = None

    if failed_qe['definedprimary']:
        chk_partner_array = pri_pool_dir_host_chk_array
        dir_chk = pri_pool_dir
        used = pri_used
    else:
        chk_partner_array = mir_pool_dir_host_chk_array
        dir_chk = mir_pool_dir
        used = mir_used        
        
    for chk_host in chk_partner_array:
        found = False
        for u in used:
            if u == chk_host:
                found = True

        if not found:
            if chk_host != partner_host:
                gpmlib.log_info('Checking host %s to see if pool directory active')
                db.execute('''select count(*) from gp_configuration where hostname=$1 and datadir like $2''', 
                           chk_host, '%s%%' % dir_chk)
                r = db.read_tuple()
                is_used = r[0]

                if is_used == 0:
                    new_host = chk_host
                    break
                else:
                    gpmlib.log_info('Host %s has an active pool directory' % chk_host)
            else:
                gpmlib.log_info('Possible host %s is a partner, checking for another alternative host' % chk_host)


    if new_host is None:
        db.execute('''select count(*) from gp_configuration where hostname=$1 and datadir like $2''',
                   partner_host, '%s%%' % dir_chk)
        r = db.read_tuple()
        is_used = r[0]
        if is_used == 0:
            gpmlib.log_info('******************************************************')
            gpmlib.log_warn('Have to locate segment on partner host %s' % partner_host)
            gpmlib.log_warn('If host containing failed segment instance is')
            gpmlib.log_warn('still active, consider use of gprecoverseg')
            gpmlib.log_warn('without options to attempt recovery to original')
            gpmlib.log_warn('segment instance directory')
            gpmlib.log_info('******************************************************')
            new_host = partner_host
        else:
            gpmlib.log_fatal('Unable to locate a pool directory host for recovery')
            gpmlib.log_info('Consider use of gprecoverseg without any options')
            gpmlib.log_info('Or use gprecoverseg -p <pool host name>')
            gpmlib.fatal('unable to continue')

    else:
        gpmlib.log_info('Identified a non-partner host %s' % new_host)


    return new_host


################
def force_recover(db):
    global opt

    c = opt['-z'].split(':')
    if len(c) != 2:
        gpmlib.fatal('datadir:host expected for -z option')

    failed_dir = c[0]
    failed_host = c[1]

    if failed_dir == '' or failed_host == '' or failed_dir == failed_host:
        gpmlib.fatal('Option -z string %s incorrect' % opt['-z'])
        
    # Obtain hostname for segment instance directory that is to be recovered
    db.execute("select count(*) from gp_configuration where datadir=$1 and hostname=$2",
               failed_dir,
               failed_host)
    r = db.read_tuple()
    failed_instance_count = r[0]
    if failed_instance_count == 0:
        gpmlib.error('[FATAL]:- Could not locate %s for host %s' % (failed_dir, failed_host))
        if failed_instance_count > 1:
            gpmlib.error('[FATAL]:- Found multiple records for %s for host %s' (failed_dir, failed_host))
                
    db.execute("select definedprimary, hostname, port, datadir, dbid, content from gp_configuration where datadir=$1 and hostname=$2",
               failed_dir, failed_host)
    failed_instance_array = [ x for x in db.iterate_dict() ]

    failed_dbids = [ x['dbid'] for x in failed_instance_array]
    db.execute('''select definedprimary, hostname, port, datadir, dbid, content from gp_configuration 
                  where dbid<> %d and 
                  content in (select content from gp_configuration where dbid=%d)''' %
               (failed_dbids[0], failed_dbids[0]))
    active_instance_array = [ x for x in db.iterate_dict() ]
    if len(active_instance_array) != 1:
        gpmlib.fatal('Did not locate 1 recovery segment instance, obtained %d active segment' % len(active_instance_array))

    opt['-f'] = True

    return (failed_dbids, failed_instance_array, active_instance_array)

################
def test_access(failed_instance_array, active_instance_array):
    gpmlib.log_info('Testing remote access')
    gpmlib.log_info('Please wait...')

    for i in range(len(failed_instance_array)):
        failed_qe = failed_instance_array[i]
        active_qe = active_instance_array[i]

        gpmlib.log_info('Test cycle %d of %d' % (i+1, len(failed_instance_array)))
        gpmlib.log_info('--------------------------------------')
        gpmlib.log_info('Test 1, create temporary file on %s' % failed_qe['hostname'])
        
        tmpfile1 = '/tmp/remote_test1.$$'
        (ok, out) = gpmlib.run('%s "%s %s && rm -f %s"' % 
                               (gplib.ssh_prefix(host=failed_qe['hostname']),
                                ENV.TOUCH, tmpfile1, tmpfile1))

        if not ok:
            gpmlib.error('[FATAL]:- write file to remote host %s failed' % failed_qe['hostname'])

        tmpfile2 = '/tmp/remote_test2.$$'
        (ok, out) = gpmlib.run('%s "%s %s && rm -f %s"' % 
                               (gplib.ssh_prefix(host=active_qe['hostname']),
                                ENV.TOUCH, tmpfile2, tmpfile2))        
        if not ok:
            gpmlib.error('[FATAL]:- write file to remote host %s failed' % active_qe['hostname'])            

        # TODO: check rsync instead of ssh
        
    gpmlib.log_info('Remote access checks completed')


################
def postgres_checkpoint(db):
    gpmlib.log_info('Issuing a checkpoint against database')
    db.execute('checkpoint')

################                                      
def stop_database(failed_instance_array):
    global opt

    if opt['-f']:
        gpmlib.log_info('Forcing shutdown the database')
        (ok, out) = gpmlib.run('gpstop -zfa')
        gpmlib.log_info('Successfully shutdown the Greenplum Database instance')
    else:
        # stop the failed segment
        for failed_qe in failed_instance_array:
            gpmlib.log_info('Forcing shutdown of %s for port=%d' % (failed_qe['hostname'], failed_qe['port']))
            (ok, out) = gpmlib.run('%s "export LD_LIBRARY_PATH=%s; env PGOPTIONS=\\"-c gp_session_role\\" %s -W stop -D %s -m immediate -l %s.log 2> /dev/null"' %
                                   (gplib.ssh_prefix(host=failed_qe['hostname']),
                                    os.environ.get('LD_LIBRARY_PATH'),
                                    os.path.join(os.environ.get('GPHOME'), 'bin/pg_ctl'),
                                    failed_qe['datadir'],
                                    failed_qe['datadir']))
            if not ok:
                gpmlib.log_warn('Issue with force shutdown of %s on %s' % (failed_qe['datadir'], failed_qe['hostname']))
            gpmlib.log_info('Checking for postmaster proces on %s port %d' % (failed_qe['hostname'], failed_qe['port']))
        
            (ok, pid) = gpmlib.postgres_active(failed_qe['port'], failed_qe['hostname'])
            if ok:
                gpmlib.log_warn('Found postmaster process on %s port %d' % (failed_qe['hostname'], failed_qe['port']))
                gpmlib.log_warn('Will kill process %d' % pid)
                (ok, out) = gpmlib.run('%s %s -9 %d  2> /dev/null' % 
                                       (gplib.ssh_prefix(host=failed_qe['hostname']),
                                        ENV.KILL,
                                        pid))
                if not ok:
                    gpmlib.error('kill -9 %d on remote host %s failed' % (pid, failed_qe['hostname']))

            else:
                gpmlib.log_info('No postmaster proces on %s port %d' % (failed_qe['hostname'], failed_qe['port']))

            gpmlib.log_info('Cleaning /tmp/lock files for %s for port %s' % (failed_qe['hostname'], failed_qe['port']))
            (ok, out) = gpmlib.run('%s %s -f /tmp/.s.PGSQL.%d.lock /tmp/.s.PGSQL.%d' %
                                   (gplib.ssh_prefix(host=failed_qe['hostname']),
                                    ENV.RM, 
                                    failed_qe['port'],
                                    failed_qe['port']))


################                                      
def pool_catalog_update(db, update_sql):
    for sql in update_sql:
        db.execute("update gp_configuration set hostname=$1, datadir=$2, port=$3 where dbid=$4", sql[0], sql[1], sql[2], sql[3])

################
def update_status(db, failed_instance_array):

    for failed_qe in failed_instance_array:
        gpmlib.log_info('Setting %s dbid=%d port=%d as active' % 
                        (failed_qe['hostname'],
                         failed_qe['dbid'],
                         failed_qe['port']))

        db.begin()
        db.execute("update gp_configuration set valid = 't' where dbid = CAST(%d AS smallint)" % failed_qe['dbid'])
        db.execute("update gp_configuration set isprimary = definedprimary where content = CAST(%d AS smallint)" % failed_qe['content'])
        db.commit()

################
def reset_valid(db, failed_dbids):
    gpmlib.log_info('Resetting invalid segment status values')
    for dbid in failed_dbids:
        try:
            db.execute("update gp_configuration set valid='t' where dbid=%d" % dbid)
        except pg8000.errors.InterfaceError, e:
            gpmlib.log_warn('Issue with reset dbid %d as valid' % dbid)

################
def reset_qe_connect(db):
    db.execute("select oid from pg_namespace where nspname='public'")
    r = db.read_tuple()
    if r:
        public_oid = r[0]
        gpmlib.log_info('Obtained public schema OID value of %d' % public_oid)
        db.execute("select relname from pg_class where relkind='r' and relnamespace=%d order by reltype desc limit(1)" % public_oid)
        r = db.read_tuple()
        if r:
            table_name = r[0]
            gpmlib.log_warn('Testing database access using table %s' % table_name)
            try:
                db.execute("select * from %s limit 1" % table_name)
                for r in db.iterate_tuple():
                    print str(r)
            except pg8000.errors.InterfaceError, e:
                gpmlib.log_warn('Issue with test select from table %s in public schema, check log file' % table_name)
        else:
            gpmlib.log_info('Could not locate a user table in public schema to test database access')
    else:
        gpmlib.log_info('Unable to run a database table access test as public schema does not exist')

    gpmlib.log_info('Dump of gp_pgdatabase ***********************************************')
    db.execute('select * from %s as _gp_pg_view' % ENV.GP_PG_VIEW)
    for r in db.iterate_tuple():
        buf = StringIO.StringIO()
        for x in r:
            buf.write(str(x))
            buf.write('|')
        gpmlib.log_info(buf.getvalue())

    gpmlib.log_info('Dump of gp_configuration ********************************************')
    db.execute('select * from gp_configuration')
    for r in db.iterate_tuple():
        buf = StringIO.StringIO()
        for x in r:
            buf.write(str(x))
            buf.write('|')
        gpmlib.log_info(buf.getvalue())

################                                      
#
# host:port:data_dir  [pool_host:port:pool_dir]
################

def parse_config(db):
    global opt

    failed_instance_array = []
    active_instance_array = []
    pool_failed_instance_array = []
    
    with open(opt['-i']) as f:
        for line in f:
            line = line.strip()
            if line == '':
                continue
            failed_qe = line.split()
            if len(failed_qe) == 1 or len(failed_qe) == 2:
                cfg = failed_qe[0].split(':')
                if len(cfg) != 3:
                    gpmlib.error('[FATAL]:- config file format error. %s' % line)
                db.execute('''select definedprimary, hostname, port, datadir, dbid, content from gp_configuration where hostname=$1 and port=$2 and datadir=$3''',
                           cfg[0].strip(), int(cfg[1]), cfg[2].strip())
                r = db.read_dict()
                if r is None:
                    gpmlib.error('[FATAL]:- segment %s not found' % line)                
                failed_instance_array.append(r)
            else:
                gpmlib.error('[FATAL]:- config file format error')
                
            if len(failed_qe) == 2:
                cfg = failed_qe[1].split(':')
                if len(cfg) != 3:
                    gpmlib.error('[FATAL]:- config file format error. %s' % line)
                a = failed_instance_array[-1].copy()
                a['hostname'] = cfg[0].strip()
                a['port'] = int(cfg[1])
                a['datadir'] = cfg[2].strip()
                pool_failed_instance_array.append(a)
            else:
                pool_failed_instance_array.append(failed_instance_array[-1])

    if pool_failed_instance_array == failed_instance_array:
        pool_failed_instance_array = None

    failed_dbids = [ x['dbid'] for x in failed_instance_array]

    for failed_dbid in failed_dbids:
        db.execute('''select definedprimary, hostname, port, datadir, dbid, content from gp_configuration 
                  where dbid<> %d and 
                  content in (select content from gp_configuration where dbid=%d)''' %
                   (failed_dbid, failed_dbid))  
        r = db.read_dict()
        active_instance_array.append(r)

    update_sql = None
    if pool_failed_instance_array is not None:
        update_sql = []
        for qe in pool_failed_instance_array:
            update_sql.append((qe['hostname'], qe['datadir'], qe['port'], qe['dbid']))

    return (failed_dbids, failed_instance_array, active_instance_array, pool_failed_instance_array, update_sql)
    
################
def print_config(failed_instance_array, active_instance_array, pool_failed_instance_array):
    global opt
    with open(opt['-o'], 'w') as f:    
        for i in range(len(failed_instance_array)):
            f.write('%s:%d:%s' % (failed_instance_array[i]['hostname'],
                                  failed_instance_array[i]['port'],
                                  failed_instance_array[i]['datadir']))
            if pool_failed_instance_array is not None:
                f.write(' %s:%d:%s' % (pool_failed_instance_array[i]['hostname'],
                                       pool_failed_instance_array[i]['port'],
                                       pool_failed_instance_array[i]['datadir']))
            f.write('\n')


################                                      
try:

    parseCommandLine()

    ENV.chk_environ()

    master_port = gpmlib.get_master_port(opt['-d'])
    
    # check passive standby
    if gpmlib.chk_on_passive_standby(master_port):
        gpmlib.fatal('Unable to process requested command') 

    db = None
    qes = None
    new_addr = []
    multi_home = False
    mirrors = []
    update_sql = None
    failed_instance_array = None
    active_instance_array = None
    pool_failed_instance_array = None
    failed_array = None
    active_array = None
    failed_dbids = None

    # connect to master QD in admin mode
    try:
        db = pg8000.Connection(host='localhost', port=master_port, 
        user=ENV.USER, database=ENV.DBNAME, 
        options='-c gp_session_role=utility')
    except pg8000.errors.InterfaceError, e:
        gpmlib.log_error('Database is not started.')
        # TODO: start master in admin mode and check mirror configured, finally exit
        gpmlib.error(e)

    try:
        # check database running
        if not gpmlib.chk_db_running(db, True):
            gpmlib.error('Database is not running')

        # check mirror configured
        if not gpmlib.chk_mirrors_configured(db):
            gpmlib.log_fatal('There are no mirrors configured for this Greenplum Datanase instance')
            gpmlib.log_fatal('The Greenplum Database instance will need to be recovered manually')
            gpmlib.log_fatal('Try gpstop -r -z or see the gp_restore command')
            gpmlib.fatal('No further progress can be made')

        # get qe details
        qes = gpmlib.get_qe_details(db, 'content')
        qe_hosts = [ x['hostname'] for x in qes ]
        hosts = gpmlib.unique(qe_hosts)

        # check multi-home
        multi_home = gpmlib.chk_multi_home(hosts)

        # get gp_fault_action
        fault_action =  gp_fault_action(db)
        if fault_action != 'readonly' and fault_action != 'continue':
            gpmlib.log_fatal('Parameter gp_fault_action is not set to either readonly or continue')
            gpmlib.log_fatal('Greenplum fault tolerance is not set')
            gpmlib.log_fatal('Unable to determine if there are actual segment failures')
            info_text()
            gpmlib.fatal('Unable to continue')

        if fault_action == 'continue':
            opt['-f'] = True

        # generate list of failed instances, active instances and failed dbids
        if opt['-i']:
            (failed_dbids, failed_instance_array, active_instance_array, pool_failed_instance_array, update_sql) = parse_config(db)
        else:

            if opt['-z']:
                # Force recovery
                (failed_dbids, failed_instance_array, active_instance_array) = force_recover(db)
            else:

                # get mirror type
                (mir_type_num, mir_type) = gpmlib.get_mirror_type(db, multi_home)

                # get failed segments' dbid
                failed_dbids = get_failed_dbids(db)
                if len(failed_dbids) == 0:
                    gpmlib.log_info('No invalid segment instances located')
                    sys.exit(0)

                # check separate host
                chk_separate_host(db, mir_type_num, failed_dbids)

                # check failed segment count
                if len(failed_dbids) > len(qes):
                    gpmlib.log_fatal('Insufficient remaining active instances to be able to recover instance')
                    gpmlib.fatal('Unable to continue')
                else:
                    gpmlib.log_info('Sufficient active instances to be able to start recovery')
            
                # get details of failed and its segment
                (failed_instance_array, active_instance_array) = get_mirror_details(db, failed_dbids)



            # pool host opt['-p']
            if opt['-p']:
                (pri_pool_host_in_use, mir_pool_host_in_use, pri_datadir_prefix, mir_datadir_prefix) = chk_pool_host(db, failed_dbids)
            
                (update_sql, pool_failed_instance_array) = pool_host_dir_validation(db, failed_instance_array, pri_pool_host_in_use, 
                                                                                    mir_pool_host_in_use, pri_datadir_prefix, mir_datadir_prefix)

            # pool dir opt['-s']
            if opt['-s']:
                chk_pool_dir_available(db, failed_dbids)
                (update_sql, pool_failed_instance_array) = pool_dir_validation(db, failed_instance_array)

            if opt['-o']:
                print_config(failed_instance_array, active_instance_array, pool_failed_instance_array)
                gpmlib.log_info('Configuration file output to %s successfully.' % opt['-o'])
                sys.exit(0)


        if len(failed_dbids) == 0:
            gpmlib.log_info('No invalid segment instances located')
            if fault_action != 'readonly':
                gpmlib.log_info('Fault tolerance is set to %s' % fault_action)
                gpmlib.log_info('Please review the following')
                info_text()
                gpmlib.fatal('Unable to continue')
            sys.exit(0)

        active_array = active_instance_array
        if opt['-p'] or opt['-s'] or pool_failed_instance_array is not None:
            failed_array = pool_failed_instance_array
        else:
            failed_array = failed_instance_array

        # yes or no
        if not opt['-a']:
            # display parameter setting
            display_param(fault_action, failed_array, active_array, multi_home)
            if not yesorno('Continue with segment recovery procedure? [yY|nN]', 'User aborted'):
                sys.exit(1)

        # force a transaction log checkpoint
        postgres_checkpoint(db)

    except pg8000.errors.InterfaceError, e:
        gpmlib.error(e)
    except pg8000.errors.ProgrammingError, e:
        gpmlib.error(e)

    if db:
        db.close()
        db = None

    # test access
    test_access(failed_instance_array, active_instance_array)
    
    # stop database or failed instances
    stop_database(failed_instance_array)

    # recover data
    (success, completed, killed, failed, total) = recover_data(failed_instance_array, active_instance_array, pool_failed_instance_array)

    # start master QD in admin mode if force restart
    if opt['-f']:
        (ok, out) = gpmlib.run('gpstart -maq')

    try:
        db = pg8000.Connection(host='localhost', port=master_port, 
                               user=ENV.USER, database=ENV.DBNAME, 
                               options='-c gp_session_role=utility')

        if opt['-p'] or opt['-s']:
            pool_catalog_update(db, update_sql)


        if opt['-f']:
            # update status 
            update_status(db, failed_instance_array)

            # update valid='t'
            reset_valid(db, failed_dbids)

            # stop master QD
            gpmlib.log_info('Shutting down the master instance')
            (ok, out) = gpmlib.run('gpstop -maq')
            gpmlib.log_info('Successfully shutdown the master instance')

            # start database
            (ok, out) = gpmlib.run('gpstart -a')
        else:
            reset_qe_connect(db)
            
    except pg8000.errors.InterfaceError, e:
        gpmlib.log_error('Database is not started.')
        gpmlib.error(e)

    if db: 
        db.close()

    if success:
        gpmlib.log_info('Recovery completed successfully')
    else:
        gpmlib.log_warn('Segment recovery failures detected, please review log file')

    gpmlib.log_info('Recovered %d of %d failed segments' % (completed, total))

    sys.exit(arg['exit'])

except KeyboardInterrupt:
    sys.exit('\nUser Interrupted')



