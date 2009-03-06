#!/usr/bin/env python

'''
gpsnap -- snapshot a gpdb array

Usage: gpsnap [-vlfz?] [-u user] {-c|-r|-d|-i} snapshot_name

	-c: create a snapshot
        -r: restore a snapshot
        -d: delete a snapshot
        -i: show information on snapshot

        -z: use zfs snapshot mechanism
        -l: list snapshots
        -v: verbose
        -V: very verbose
        -f: force
        -?: print help

'''

import os, sys
progname = os.path.split(sys.argv[0])[-1]
if sys.version_info < (2,5,0):
    sys.exit(
'''Error %s is supported on Python version 2.5 or greater
Please upgrade python installed on this machine.''' % progname)

import subprocess, time, datetime, threading, Queue, pickle, random


############
MASTER_DATA_DIRECTORY = os.getenv('MASTER_DATA_DIRECTORY')
if not MASTER_DATA_DIRECTORY:
    sys.exit('MASTER_DATA_DIRECTORY env not defined')
os.putenv('PGHOST', '')
os.putenv("PGOPTIONS", '-c gp_session_role=utility')

class __globals__:
    opt = {}
    opt['-v'] = False
    opt['-V'] = False
    opt['-c'] = False
    opt['-r'] = False
    opt['-d'] = False
    opt['-i'] = False
    opt['-f'] = False
    opt['-l'] = False
    opt['-z'] = False
    opt['-u'] = ''
    snapname = ''



GV = __globals__()

############
def usage(exitarg):
    print __doc__
    sys.exit(exitarg)

############
def humantime(td):
    d = td.days > 0 and td.days or 0
    h = int(td.seconds / 60 / 60)
    m = int(td.seconds / 60) % 60
    s = td.seconds % 60
    ret = ''
    if d: ret = ret + '%dD ' % d
    if h: ret = ret + '%dh ' % h
    ret = ret + ('%dm %ds' % (m, s))
    return ret

############
def tstamp():
    return datetime.datetime.now().strftime('[%Y-%m-%d %H:%M:%S]')


############
def msg(s):
    print '%s %s' % (tstamp(), s)

def vmsg(s):
    if GV.opt['-v']: msg(s)

def vvmsg(s):
    if GV.opt['-V']: msg(s)

############
def die(s):
    sys.exit('%s ERROR: %s' % (tstamp(), s))


############
def confirm(s):
    if not GV.opt['-f'] and sys.stdin.isatty():
        ok = raw_input('%s\n ... proceed (y/n)? ' % s)
        print
        ok = ok.strip().lower()
        return ok and ok[0] == 'y'
    return True

############
def parseCommandLine():
    import getopt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], '?Vvlcrdifzu:')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '-?': usage(0)
        elif switch[1] in 'Vvlcrdifz': GV.opt[switch] = True
        elif switch == '-u': GV.opt[switch] = val

    if GV.opt['-l']:
        return
        
    if 1 == reduce(lambda x,y: x+y, [GV.opt['-'+s] and 1 or 0 for s in "crdi"]):
        pass
    else:
        usage('Error: please specify one of -c / -r / -d / -i')
    
    if 1 == len(args):
        GV.snapname = args[0]
        import re
        if not re.match('\A[a-zA-Z0-9][a-zA-Z0-9\-\_\:]*\Z', GV.snapname):
            usage("\n".join(["Error: invalid snapshot name",
                             "Hint: valid name like 'thursday_2009-03_19:00'"]))
    else:
        usage('Error: missing snapshot_name')


    
############
def run(cmd):
    vvmsg(cmd)
    p = os.popen(cmd)
    out = p.readlines()
    if GV.opt['-V']:
        for line in out: vvmsg(line[:-1])
    rc = p.close()
    return (rc, out)

def runas(cmd):
    if GV.opt['-u']:
        vvmsg(cmd)
        p = subprocess.Popen(['su', '-', GV.opt['-u'], '-c', cmd], 
                             stdout=subprocess.PIPE)
        out = p.stdout.readlines()
        if GV.opt['-V']:
            for line in out: vvmsg(line[:-1])
        return (p.wait(), out)
    else:
        return run(cmd)



############
def psql(sql, quiet=True):
    sql = sql.strip()
    vvmsg('SQL: ' + sql)
    cmd = ['psql', 'template1']
    if quiet: cmd.append('-q')
    cmd = cmd + ['-At', '-c', sql]
    stime = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = p.stdout.readlines()
    rc = p.wait()
    etime = time.time()
    if rc:
        die('PSQL ERROR\nSQL: ' + sql)

    if GV.opt['-V']:
        for line in out: vvmsg(line[:-1])

    vvmsg('ELAPSED: ' + str(etime - stime))
    return out

############
class SnapshotName:
    def __init__(self, datapath, name):
        (self.m_toppath, self.m_dbdir) = os.path.split(datapath)
        self.m_name = name
        self.m_prefix = '%s@%s' % (self.m_dbdir, self.m_name)

    def toppath(self): return self.m_toppath
    def dbdir(self):  return self.m_dbdir
    def prefix(self): return self.m_prefix
    def tgzname(self): return self.m_prefix + '.tgz'
    def tgzpath(self): return os.path.join(self.toppath(), self.tgzname())
    def ctlname(self): return self.m_prefix + '.snap' 
    def ctlpath(self): return os.path.join(self.toppath(), self.ctlname())

############
class Segment:
    def __init__(self, line):
        row = [x.strip() for x in line.split('|')]
        (self.content, self.definedprimary, self.dbid,
         self.isprimary, self.valid, self.hostname,
         self.port, self.datapath) = row

    def __str__(self):
        return "|".join([self.content, self.definedprimary, self.dbid, 
                         self.isprimary, self.valid, self.hostname, 
                         self.port, self.datapath])

    def type(self): return 'tgz'

    def ctlpath(self, name):
        sname = SnapshotName(self.datapath, name)
        return self.content == '-1' and sname.ctlpath() or None


    def createSnapshot(self, name):
        sname = SnapshotName(self.datapath, name)
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               ('cd %s && ' % sname.toppath())
               + ('(rm -f %s %s) && ' % (sname.tgzname(), sname.ctlname()))
               + ('(gtar cfz %s %s 2> /dev/null || tar cfz %s %s)' % (sname.tgzname(), sname.dbdir(), 
                                                                      sname.tgzname(), sname.dbdir()))]
        vvmsg("  - " + " ".join(cmd))
        rc = subprocess.Popen(cmd).wait()
        if rc: 
            # some errors occurred ... clean up
            self.deleteTgzSnapshot(name)
        else:
            msg('Created snapshot %s:%s' % (self.hostname, 
                                            sname.tgzpath()))
        return rc

        
    def deleteSnapshot(self, name):
        sname = SnapshotName(self.datapath, name)
        msg('Deleting snapshot of %s:%s' % (self.hostname, self.datapath))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               'rm -f %s %s' 
               % (sname.tgzpath(), sname.ctlpath())]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()


    def checkSnapshot(self, name):
        sname = SnapshotName(self.datapath, name)
        msg('Checking snapshot of %s:%s' % (self.hostname, self.datapath))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               '(gtar tfz %s || tar tfz %s) > /dev/null'
               % (sname.tgzpath(), sname.tgzpath())]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()

        
    def restoreSnapshot(self, name):
        sname = SnapshotName(self.datapath, name)
        msg('Restoring snapshot of %s:%s' % (self.hostname, self.datapath))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               'rm -rf %s; cd %s && (gtar xfz %s || tar xfz %s) 2> /dev/null' 
               % (self.datapath, sname.toppath(), sname.tgzname(), sname.tgzname())]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()
        



class ZfsSegment(Segment):
    def __init__(self, line):
        Segment.__init__(self, line)
	self.snapshotname = ''

    def type(self): return 'zfs'

    def get_snapshotname(self, name):
	if self.snapshotname: return self.snapshotname

        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname, 
		'zfs list -t filesystem ' + self.datapath]
	p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
	out = p.stdout.readlines()
	rc = p.wait()
	
	sname = ''
	for lines in out: 
	    t = map(lambda x: x.strip(), lines.split())
	    if len(t) != 5: return -1
	    if self.datapath.find(t[4]) == 0: 
		sname = t[0]+'@' + name; break

	self.snapshotname = sname
	return sname


    def createSnapshot(self, name):
	sname = self.get_snapshotname(name)
	if not sname:
	    return -1

        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname, 
		'zfs snapshot ' + sname]
	p = subprocess.Popen(cmd)
	rc = p.wait()

        vvmsg("  - " + " ".join(cmd))
        if not rc:
            msg('Created snapshot %s:%s' % (self.hostname, self.datapath))
        return rc

    def deleteSnapshot(self, name):
	sname = self.get_snapshotname(name)
	if not sname:
	    return -1
	
        msg('Deleting snapshot of %s:%s' % (self.hostname, self.datapath))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
		'zfs destroy ' + sname]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()

    def checkSnapshot(self, name):
	sname = self.get_snapshotname(name)
	if not sname:
	    return -1
	
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
		'zfs list -t snapshot ' + sname]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()
               
    def restoreSnapshot(self, name):

	sname = self.get_snapshotname(name)
	if not sname:
	    return -1
	
        msg('Restoring snapshot of %s:%s' % (self.hostname, self.datapath))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
		'zfs rollback ' + sname]
        vvmsg("  - " + " ".join(cmd))
        return subprocess.Popen(cmd).wait()
        


############
def pmap(func, jlist, numThreads = 10):
    if (numThreads > len(jlist)): 
        numThreads = len(jlist)

    inq = Queue.Queue(len(jlist))
    for i in jlist: inq.put(i)
    
    outq = Queue.Queue(len(jlist))
    def work():
        try:
            while True: 
                outq.put((None, func(inq.get_nowait())))
        except Queue.Empty: pass
        except:
            outq.put((sys.exc_info()[0], None))
            # drain 
            try:
                while True: inq.get_nowait()
            except Queue.Empty: pass

    thread = [threading.Thread(target=work) for i in xrange(numThreads)]
    for t in thread: t.start()
    for t in thread: t.join()
    
    ret = []
    try: 
        while True: 
            (ex, result) = outq.get_nowait()
            if ex: 
                raise ex
            ret.append(result)
    except Queue.Empty: pass
    return ret


def test_pmap():
    import random, time
    def p(x): time.sleep(random.random()); print x; return x
    jlist = [x for x in "abcdefghijklmnopqrstuvwxyz"]
    return pmap(p, jlist)



############
def check_psql():
    (rc, out) = runas("psql -c 'select now()' 2>&1")
    if rc:
        die('cannot run psql:\n\t%s' % ''.join(out))


############
class ControlInfo:
    def __init__(self, name, segments, stime, etime):
        (self.name, self.segments, self.stime, self.etime) = (name, segments, stime, etime)
        self.etime = self.etime.replace(microsecond=0)
        self.stime = self.stime.replace(microsecond=0)
        self.type = GV.opt['-z'] and 'zfs' or 'tgz'

    def delete(self, master):
        fpath = master.ctlpath(self.name)
        if not fpath:
            return
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', master.hostname,
               'rm -f %s' % fpath]
        vmsg(" ".join(cmd))
        p = subprocess.Popen(cmd, stdin = subprocess.PIPE)
        p.stdin.write(pickle.dumps(self))
        p.stdin.close()
        p.wait()

    def write(self, master):
        fpath = master.ctlpath(self.name)
        if not fpath:
            die('Internal error: cannot write control file to non-master')

        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', master.hostname,
               'cat > %s' % fpath]
        vvmsg(" ".join(cmd))
        p = subprocess.Popen(cmd, stdin = subprocess.PIPE)
        p.stdin.write(pickle.dumps(self))
        p.stdin.close()
        rc = p.wait()
        if not rc:
            msg('Control file at %s:%s' % (master.hostname, fpath))
        return rc

    @staticmethod
    def read(name):
        sname = SnapshotName(MASTER_DATA_DIRECTORY, name)
        f = None
        try:
            f = open(sname.ctlpath())
            return pickle.load(f)
        except IOError:
            return None
        finally: f and f.close()
    
    @staticmethod
    def list():
        fpath = os.path.join(MASTER_DATA_DIRECTORY, '..', '*@*.snap')
        p = os.popen('ls -1 %s 2> /dev/null' % fpath)
        out = p.readlines()
        p.close()
        
        ret = []
        for fpath in out:
            fpath = fpath.strip()
            f = None
            try:
                f = open(fpath)
                ret.append(pickle.load(f))
            except IOError: pass
            finally: f and f.close()
        
        return ret


def selectSegments():
    lines = psql('''
select content, definedprimary, dbid, isprimary, valid, hostname, port, datadir as datapath 
from gp_configuration
order by content''')
    if GV.opt['-z']:
        return [ZfsSegment(line) for line in lines]
    else:
        return [Segment(line) for line in lines]

        

############
def createSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if ctlInfo:
        die("Snapshot '%s' exists." % name)
    
    if not confirm('Create a snapshot. \nThis will shutdown the database.'):
        die("Aborted by user.")

    # need gp_configuration
    stime = datetime.datetime.now()
    vmsg("Starting masters in utility mode ...")
    runas('gpstart -m')
    vmsg("Checking psql connectivity ...")
    check_psql()
    segments = selectSegments()
    
    # shut it down
    vmsg("Stopping masters ...")
    runas('gpstop -af')

    # shuffle the segments so we spread work across the array
    segments = segments[:]
    random.shuffle(segments)

    # take the snapshot in parallel
    erows = filter(lambda (s, rc): rc,
                   pmap(lambda s: (s, s.createSnapshot(name)), segments))
    # if error -> rollback
    if erows:
        emsg = (["Create failed:"] +
                ["  unable to create %s:%s@%s" 
                 % (s.hostname, s.datapath, name) for (s, rc) in erows])
        # delete the snapshots
        pmap(lambda s: (s, s.deleteSnapshot(name)), segments)
        die("\n".join(emsg))

    # create control file
    etime = datetime.datetime.now()
    ctlInfo = ControlInfo(name, segments, stime, etime)

    # write the control file to the masters
    masters = filter(lambda s: s.content == '-1', segments)
    erows = filter(lambda (m, rc): rc, 
                   map(lambda m: (m, ctlInfo.write(m)), masters))
    # if error -> rollback
    if erows:
        emsg = (["Create failed:"] +
                ["  cannot create control file %s:%s"
                 % (m.hostname, m.ctlpath(name)) for (m, rc) in erows])
        # delete the snapshots
        pmap(lambda s: (s, s.deleteSnapshot(name)), segments)
        pmap(lambda m: (m, ctlInfo.delete(m)), masters)
        die("\n".join(emsg))
        
    msg("Created. Elapsed %s." % humantime(etime - stime))

############
def restoreSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if not ctlInfo:
        die("Unable to find snapshot '%s'" % name)
    
    if not confirm("Restore snapshot '%s'.\nThis will shutdown the database." % name):
        die("Aborted by user.")

    stime = datetime.datetime.now()

    # shut it down
    runas('gpstop -af')

    segments = ctlInfo.segments[:]
    random.shuffle(segments)

    erows = filter(lambda (s,rc): rc,
                   pmap(lambda s: (s, s.checkSnapshot(name)), segments))
    if erows:
        emsg = (["Snapshot failure"] +
                ["  Bad/missing snapshot of %s:%s" 
                 % (s.hostname, s.datapath) for (s, rc) in erows])
        die("\n".join(emsg))

    if not confirm("Continue to *delete* database and restore snapshot.\nThis is the point of no return."):
        die("Aborted by user.")

    erows = filter(lambda (s,rc): rc, 
                   pmap(lambda s: (s, s.restoreSnapshot(name)), segments))
    if erows:
        emsg = (["Restore failed:"] +
                ["  unable to restore %s:%s" 
                 % (s.hostname, s.datapath) for (s, rc) in erows])
        die("\n".join(emsg))

    etime = datetime.datetime.now()
    msg("Restored. Elapsed %s." % humantime(etime - stime))
    
    
############
def infoSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if not ctlInfo:
        die("Unable to find snapshot '%s'" % name)
    print 'snapshot name:',ctlInfo.name
    print 'created      :',str(ctlInfo.etime)
    print 'segments     :'
    for s in ctlInfo.segments: print "\t%s" % str(s)
    


############
def deleteSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if not ctlInfo:
        msg("Snapshot '%s' does not exist" % name)
        return                  # assume there isn't any snapshots available

    if not confirm("Delete snapshot '%s'" % name):
        msg("Aborted by user.")

    stime = datetime.datetime.now()
    segments = ctlInfo.segments[:]
    random.shuffle(segments)

    erows = filter(lambda (s,rc): rc,
                   pmap(lambda s: (s, s.deleteSnapshot(name)), segments))
    if erows:
        emsg = (["Delete failed:"] +
                ["  unable to delete %s:%s@%s" 
                 % (s.hostname, s.datapath, name) for (s, rc) in erows])
        die("\n".join(emsg))

    etime = datetime.datetime.now()
    msg("Deleted. Elapsed %s." % humantime(etime - stime))


############
def listSnapshots():
    ctlInfoList = ControlInfo.list()
    if not ctlInfoList:
        msg("No snapshot exists")
        return
    ctlInfoList.sort(lambda x,y: x.etime > y.etime)
    for i in ctlInfoList:
        print '%s %s %s' % (i.type, str(i.etime), i.name)


############
def main():
    parseCommandLine()
    if GV.opt['-c']: 
        createSnapshot(GV.snapname)
    elif GV.opt['-r']:
        restoreSnapshot(GV.snapname)
    elif GV.opt['-d']:
        deleteSnapshot(GV.snapname)
    elif GV.opt['-i']:
        infoSnapshot(GV.snapname)
    elif GV.opt['-l']:
        listSnapshots()
    else:
        usage('Error: invalid flags and/or arguments')

if __name__ == '__main__':
    main()
