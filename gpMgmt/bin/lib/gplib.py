from __future__ import with_statement 
import os, sys

progname = os.path.split(sys.argv[0])[-1]

if sys.version_info < (2, 5, 0):
    sys.exit(
'''Error: %s is supported on Python versions 2.5 or greater
Please upgrade python installed on this machine.''' % progname)


################
def cli_help():
    help_path = os.path.join(sys.path[0], '..', 'docs', 'cli_help', progname + '_help');
    f = None
    try:
        try:
            f = open(help_path);
            return f.read(-1)
        except:
            return ''
    finally:
        if f: f.close()


################
def parse_port(val): 
    try: return int(val)
    except ValueError: return 0


#############
def ssh_prefix(host):
    ssh = 'ssh -o "BatchMode yes" -o "StrictHostKeyChecking no" ' + host
    return ssh


#############
def scp_prefix():
    scp = 'scp -o "BatchMode yes" -o "StrictHostKeyChecking no"'
    return scp

#############
class Error(Exception):
    '''Base class for exceptions in this module.'''
    pass


#############
class HostNameError(Error):
    def __init__(self, msg, lineno = 0):
        if lineno: self.msg = ('%s at line %d' % (value, lineno))
        else: self.msg = msg
    def __str__(self):
        return self.msg


#############
class SSHError(Error):
    def __init__(self, msg):
        self.msg = msg
    def __str__(self):
        return self.msg

#############
def ssh(host, cmd):
    s = 'ssh -o "BatchMode yes" -o "StrictHostKeyChecking no" %s %s 2>& 1' % (host, cmd)
    p = os.popen(s)
    if not p:
        raise SSHError('unable to ssh to %s' % host)
    yield p
    out = p.read(-1)
    rc = p.close() or 0 
    yield (rc, out)


#############
class HostList():
    
    def __init__(self): 
        self.list = []

    def get(self): 
	return self.list

    def add(self, host):
        '''Add a host to the hostlist.'''
        if host.find(':') >= 0 or host.find('@') >= 0:
            raise HostNameError(host)
        self.list.append(host)
        return self.list

    def parseFile(self, path):
        '''Add lines in a file to the hostlist.'''
        with open(path) as fp:
            lineno = 0
            for line in fp:
                lineno = lineno + 1
                line = line.strip()
                if not line: continue;
                if line[0] == '#': continue
                if not self.add(line): 
                    raise HostNameError(line, lineno)
        return self.list

    def batch(self, size=32):
        '''Return list of hostlists, each is of 'size' elements'''
        cnt = int((len(self.list) - 1) / size) + 1
        b = []
        for i in xrange(cnt):
            start = i * size
            end = start + size
            if end > len(self.list): 
                b.append(self.list[start:])
            else:
                b.append(self.list[start:end])
        return b

    def checkSSH(self):
        '''Check that ssh to hostlist is okay.'''
        for hlist in self.batch():
            slist = []
            for h in hlist:
                s = ssh(h, 'echo')
                s.next()
                slist.append((h, s))
            for (h, s) in slist:
                (rc, out) = s.next()
                if out and out.find('Permission denied') >= 0: 
                    raise SSHError("Permission denied while ssh to '%s'" % h)
                if rc: 
                    raise SSHError("Unable to ssh to '%s'" % h)
        return True

    def filterMultiHomedHosts(self):
        '''For multiple host that is of the same node, keep only one in the hostlist.'''
        unique = {}
        for hlist in self.batch():
            slist = []
            for h in hlist:
                s = ssh(h, 'hostname')
                s.next()
                slist.append((h, s))
            for (h, s) in slist:
                (rc, hname) = s.next()
                if rc: 
                    raise SSHError('Cannot execute hostname on host %s' % h)
                hname = hname.strip()
                if not unique.get(hname):
                    unique[hname] = h
                elif hname == h:
                    unique[hname] = h

        self.list = unique.values()
        return self.list

