#!/usr/bin/env python
import unittest, os, popen2, time, sys, getopt, StringIO

MYD = os.path.abspath(os.path.dirname(__file__))
mkpath = lambda *x: os.path.join(MYD, *x)

def log(m, m1 = None, m2 = None, m3 = None):
    print m
    if m1: 
	print m1
	if m2: 
	    print m2
	    if m3:
		print m3
    print '\n'

class GPTestError(Exception): pass


class Shell:

    def __init__(self):
        self.lastcmd = ''

    def run(self, cmd):
        "Run a shell command. Return (True, [result]) if OK, or (False, []) otherwise."
        self.lastcmd = cmd
        p = os.popen(self.lastcmd)
        ret = []
        for line in p:
            ret.append(line)
        rc = p.close()
        return (not rc, ret)

shell = Shell()


class PSQLError(Exception): pass
    
class PSQL: 

    DBNAME = 'template1'

    def run(self, dbname = None, ifile = None, ofile = None, cmd = None, flag = '-e'):
        "Run a command or file against psql. Return True if OK."
        if not dbname:
            dbname = PSQL.DBNAME

        if cmd: 
            arg = '-c "%s"' % cmd
        elif ifile:
            arg = ' < ' + ifile
        else:
            raise PSQLError('missing cmd and ifile')

        if ofile == '-':
            ofile = '2>&1'
        elif not ofile: 
            ofile = '> /dev/null 2>&1'
        else:
            ofile = '> %s 2>&1' % ofile

        return shell.run('psql %s %s %s %s' % (dbname, flag, arg, ofile))


        
psql = PSQL() 



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
        (ok, out) = psql.run(flag = '-q -t', cmd = 'select * from gp_configuration order by dbid',
                             ofile = '-')
        if not ok:
            sys.exit('Unable to select gp_configuration')
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


config = Config()

def getnode():
    node = []
    for c in config.get():
    	node.append(c.hostname)
        
    node.sort()
    unique = []
    host = ''
    for n in node:
    	if host != n:
            unique.append(n)
            host = n
    return unique

def getnodeID(node, key):
    i=0
    for n in node:
    	if n == key:
            return i
	i+=1
    return -1


print '# please add the IP addresses of the nodes'
node = getnode()
i = 0
for n in node:
    print 'node.%d=%s' % (i, n)

print
print '# gpdb segment configurations'
print 'gpdb.name=mydb'

master_seg = 0    
primary_seg = 0
mirror_seg = 0
for c in config.get():
    if c.content == -1:
        print 'gpdb.seg.m.%d=node.%d:%s' % (master_seg, getnodeID(node, c.hostname), c.datadir)
        master_seg += 1
    else:
        if c.isprimary:
            print 'gpdb.seg.%d.0=node.%d:%s' % (primary_seg, getnodeID(node, c.hostname), c.datadir)
            primary_seg += 1
        else:
            print 'gpdb.seg.%d.1=node.%d:%s' % (mirror_seg, getnodeID(node, c.hostname), c.datadir)
            mirror_seg += 1

master_port = 0
base_port = 0
mirror_port = 0
for c in config.get():
    if c.content == -1:
        if master_port == 0:
            master_port = c.port
            print 'gpdb.seg.master_port=%s' % c.port
    else:
        if c.isprimary:
            if base_port == 0:
                base_port = c.port
                print 'gpdb.seg.base_port=%s' % c.port
        else:
            if mirror_port == 0:
                mirror_port = c.port
                print 'gpdb.seg.mirror_base_port=%s' % c.port
    
    if master_port != 0 and base_port != 0 and mirror_port != 0:
        break


print
print '# please modify the value belows'
print 'ma.datadir=/db/ma'
print 'ha.datadir=/db/ha'
print 'ma.0=node.0:11000'
