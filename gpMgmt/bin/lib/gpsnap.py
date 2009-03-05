#!/usr/bin/env python

'''
gpsnap -- snapshot a gpdb array

Usage: gpsnap [-vf?] {-c|-r|-d|-i} snapshot_name

	-c: create a snapshot
        -r: restore a snapshot
        -d: delete a snapshot
        -i: show information on snapshot

        -v: verbose
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


class __globals__:
    opt = {}
    opt['-v'] = False
    opt['-c'] = False
    opt['-r'] = False
    opt['-d'] = False
    opt['-i'] = False
    opt['-f'] = False
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

############
def die(s):
    sys.exit('%s ERROR: %s' % (tstamp(), s))


############
def confirm(s):
    if not GV.opt['-f'] and sys.stdin.isatty():
        ok = raw_input('%s. Confirm? ' % s)
        ok = ok.strip().lower()
        if ok == '' or ok[0] != 'y':
            die("Aborted")
    return True

############
def parseCommandLine():
    import getopt
    try:
        (options, args) = getopt.getopt(sys.argv[1:], '?vcrdif')
    except Exception, e:
        usage('Error: ' + str(e))

    for (switch, val) in options:
        if switch == '?': usage(0)
        elif switch[1] in 'vcrdif': GV.opt[switch] = True
        
    if 1 == reduce(lambda x,y: x+y, [GV.opt['-'+s] and 1 or 0 for s in "crdi"]):
        pass
    else:
        usage('Error: please specify one of -c / -r / -d / -i')
    
    if 1 == len(args):
        GV.snapname = args[0]
    else:
        usage('Error: missing snapshot_name')


    
############
def run(cmd):
    if GV.opt['-v']: msg(cmd)
    p = os.popen(cmd)
    out = p.readlines()
    if GV.opt['-v']: 
        for line in out: msg(line[:-1])
    rc = p.close()
    return (rc, out)


############
def psql(sql, quiet=True):
    sql = sql.strip()
    if GV.opt['-v']: msg('SQL: ' + sql)
    cmd = ['psql']
    if quiet: cmd.append('-q')
    cmd = cmd + ['-At', '-c', sql]
    stime = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = p.stdout.readlines()
    rc = p.wait()
    etime = time.time()
    if rc:
        die('PSQL ERROR\nSQL: ' + sql)

    if GV.opt['-v']: 
        msg('ELAPSED: ' + str(etime - stime))

    return out

############
class SnapshotName:
    def __init__(self, datadir, name):
        (self.m_topdir, self.m_dbdir) = os.path.split(datadir)
        self.m_name = name
        self.m_prefix = '%s-%s' % (self.m_dbdir, self.m_name)

    def topdir(self): return self.m_topdir
    def dbdir(self):  return self.m_dbdir
    def prefix(self): return self.m_prefix
    def tgzname(self): return self.m_prefix + '.tgz'
    def tgzpath(self): return os.path.join(self.topdir(), self.tgzname())
    def ctlname(self): return self.m_prefix + '.snap' 
    def ctlpath(self): return os.path.join(self.topdir(), self.ctlname())
    

############
class Segment:
    def __init__(self, line):
        row = [x.strip() for x in line.split('|')]
        (self.content, self.definedprimary, self.dbid,
         self.isprimary, self.valid, self.hostname,
         self.port, self.datadir) = row

    def __str__(self):
        return "|".join([self.content, self.definedprimary, self.dbid, 
                         self.isprimary, self.valid, self.hostname, 
                         self.port, self.datadir])

    def ctlpath(self, name):
        sname = SnapshotName(self.datadir, name)
        return self.content == '-1' and sname.ctlpath() or None


    def createSnapshot(self, name):
        sname = SnapshotName(self.datadir, name)
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               ('cd %s && ' % sname.topdir())
               + ('((rm -f %s; rm -f %s) 2> /dev/null || true) && ' % (sname.tgzname(), sname.ctlname()))
               + ('(gtar cfz %s %s 2> /dev/null || tar cfz %s %s)' % (sname.tgzname(), sname.dbdir(), 
                                                                      sname.tgzname(), sname.dbdir()))]
        if GV.opt['-v']: msg(" ".join(cmd))
        p = subprocess.Popen(cmd)
        rc = p.wait()
        if rc: 
            # some errors occurred ... clean up, ignore error
            self.deleteSnapshot(name)
        else:
            msg('Created snapshot %s:%s' % (self.hostname, 
                                            sname.tgzpath()))
        return rc

    def deleteSnapshot(self, name):
        sname = SnapshotName(self.datadir, name)
        msg('Deleting snapshot of %s:%s' % (self.hostname, self.datadir))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               'rm -f %s %s 2> /dev/null || true' 
               % (sname.tgzpath(), sname.ctlpath())]
        if GV.opt['-v']: msg(" ".join(cmd))
        p = subprocess.Popen(cmd)
        return p.wait()


    def checkSnapshot(self, name):
        sname = SnapshotName(self.datadir, name)
        msg('Checking snapshot of %s:%s' % (self.hostname, self.datadir))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               '(gtar tfz %s || tar tfz %s) > /dev/null'
               % (sname.tgzpath(), sname.tgzpath())]
        if GV.opt['-v']: msg(" ".join(cmd))
        p = subprocess.Popen(cmd)
        return p.wait()
        

    def restoreSnapshot(self, name):
        sname = SnapshotName(self.datadir, name)
        msg('Restoring snapshot of %s:%s' % (self.hostname, self.datadir))
        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', self.hostname,
               'rm -rf %s 2> /dev/null; cd %s && (gtar xfz %s || tar xfz %s) 2> /dev/null' 
               % (self.datadir, sname.topdir(), sname.tgzname(), sname.tgzname())]
        if GV.opt['-v']: msg(" ".join(cmd))
        p = subprocess.Popen(cmd)
        return p.wait()
        

    @staticmethod
    def select():
        lines = psql('''
select content, definedprimary, dbid, isprimary, valid, hostname, port, datadir 
from gp_configuration
order by content''')
        return [Segment(line) for line in lines]



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

    thread = []
    for i in xrange(numThreads):
        t = threading.Thread(target=work)
        t.start()
        thread.append(t)

    for t in thread: t.join()
    
    ret = []
    try: 
        while True: 
            (ex, result) = outq.get_nowait()
            if ex: 
                raise ex
            ret.append(result)
    except Queue.Empty: 
        pass
    return ret


def test_pmap():
    import random, time
    def p(x): time.sleep(random.random()); print x; return x
    jlist = [x for x in "abcdefghijklmnopqrstuvwxyz"]
    return pmap(p, jlist)



############
def check_psql():
    os.putenv('PGHOST', '')
    os.putenv('PGDATABASE', 'template1')
    os.putenv('PGOPTIONS', '-c gp_session_role=utility')
    (rc, out) = run('psql -c "select now()" 2>&1')
    if rc:
        die('cannot run psql:\n\t%s' % ''.join(out))


############
class ControlInfo:
    def __init__(self, name, segments, stime, etime):
        (self.name, self.segments, self.stime, self.etime) = (name, segments, stime, etime)

    def write(self, master):
        fpath = master.ctlpath(self.name)
        if not fpath:
            die('Internal error: cannot write control file to non-master')

        cmd = ['ssh', '-o', 'BatchMode yes', '-o', 'StrictHostKeyChecking no', master.hostname,
               'cat > %s' % fpath]
        if GV.opt['-v']: msg(" ".join(cmd))
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
        finally:
            if f: f.close()
    


############
def createSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if ctlInfo:
        die("Snapshot '%s' exists." % name)
    confirm('Create a snapshot. Proceed to shutdown the database')

    # need gp_configuration
    stime = datetime.datetime.now()
    run('gpstart -m')
    check_psql()
    segments = Segment.select()
    
    # shut it down
    run('gpstop -af')


    # shuffle the segments so we spread work across the array
    slist = segments[:]
    random.shuffle(slist)

    # take the snapshot in parallel
    emsg = []
    out = pmap(lambda s: (s, s.createSnapshot(name)), slist)
    for (s, rc) in out:
        if rc: 
            emsg.append("    snapshot failed for segment %s:%s" % (s.hostname, s.datadir))
    
    # if error -> rollback
    if emsg:
        # delete the snapshots
        pmap(lambda j: (j, j.deleteSnapshot(name)), slist)
        emsg = "\n".join(["Failed to take snapshot '%s'" % name] + emsg)
        die(emsg)

    # create control file
    etime = datetime.datetime.now()
    ctlInfo = ControlInfo(name, segments, stime, etime)

    # write the control file to the masters
    masters = filter(lambda s: s.content == '-1', segments)
    for (m, rc) in map(lambda m: (m, ctlInfo.write(m)), masters):
        if rc:
            emsg.append("   cannot create control file %s:%s" % (m.hostname, m.getControlFilePath))
    
    # if error -> rollback
    if emsg:
        # delete the snapshots
        pmap(lambda j: (j, j.deleteSnapshot(name)), slist)
        emsg = "\n".join(["Failed to take snapshot"] + emsg)
        die(emsg)
        
    msg("Created. Elapsed %s." % humantime(etime - stime))

############
def restoreSnapshot(name):
    ctlInfo = ControlInfo.read(name)
    if not ctlInfo:
        die("Unable to find snapshot '%s'" % name)
    
    confirm("Restore snapshot '%s'. Proceed to shutdown database" % name)

    stime = datetime.datetime.now()

    # shut it down
    run('gpstop -af')

    slist = ctlInfo.segments[:]
    random.shuffle(slist)

    emsg = []
    out = pmap(lambda s: (s, s.checkSnapshot(name)), slist)
    for (s, rc) in out:
        if rc:
            emsg.append("Bad or missing snapshot of %s:%s" % (s.hostname, s.datadir))
    
    if emsg:
        emsg = "\n".join(["Snapshot failure"] + emsg)
        die(emsg)

    confirm("Proceed to delete DB and restore snapshot" % name)

    out = pmap(lambda s: (s, s.restoreSnapshot(name)), slist)
    for (s, rc) in out:
        if rc:
            emsg.append("  Unable to restore %s:%s" % (s.hostname, s.datadir))

    if emsg:
        emsg = "\n".join(["Failed to restore some segments"] + emsg)
        die(emsg)

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

    confirm("Delete snapshot '%s'" % name)

    stime = datetime.datetime.now()
    slist = ctlInfo.segments[:]
    random.shuffle(slist)

    emsg = []
    out = pmap(lambda s: (s, s.deleteSnapshot(name)), slist)
    for (s, rc) in out:
        if rc: 
            emsg.append("   unable to delete snapshot of %s:%s" % (s.hostname, s.datadir))

    if emsg:
        emsg = "\n".join(["Unable to delete snapshot '%s'" % name] + emsg)
        die(emsg)

    etime = datetime.datetime.now()
    msg("Deleted. Elapsed %s." % humantime(etime - stime))




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
    else:
        usage('Error: invalid flags and/or arguments')

if __name__ == '__main__':
    main()
