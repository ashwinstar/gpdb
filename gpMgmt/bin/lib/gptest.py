import unittest, os, popen2, time, sys


def getOutFile(fname):
    return os.path.splitext(fname)[0] + '.out'


def getResultFile(fname):
    fname = os.path.splitext(fname)[0]

    v = fname + '.v'
    if not os.access(v, os.R_OK): v = None

    s = fname + '.s'
    if not os.access(s, os.R_OK): s = None

    if v and s:
	print 'Warning: both %s and %s exist' % (v, s)
	print '   - picking %s as result file' % v

    return v or s


def chksumUnsorted(fname):
    f = None
    try:
	f = os.popen('cat %s | md5' % fname)
	return f.readlines()
    finally:
	if f: f.close()


def chksumSorted(fname):
    f = None
    try:
	f = os.popen('cat %s | sort | md5' % fname)
	return f.readlines()
    finally:
	if f: f.close()


class GPTestError(Exception): pass


class GPTestCase(unittest.TestCase):

    DB_NAME = 'gptest'
    PORT = os.getenv('PGPORT')
    if not PORT:
	sys.exit('Error: PGPORT env variable not set (for psql)')
    try:
	PORT = int(PORT)
    except ValueError:
	sys.exit('Error: PGPORT env variable is not valid (%s)', PORT)
	

    def reload(self): pass

    def reloadAlways(self): return False

    def __init__(self, name):
	self.lastCommand = ''
	unittest.TestCase.__init__(self, name)

    def _execpsql(self, db = None, ifile = None, ofile = None, cmd = None):

	if not db: db = self.DB_NAME
	arg = db

	if cmd: arg = '%s -c "%s"' % (arg, cmd)
	elif ifile: arg = arg + ' < ' + ifile

	if not ofile: ofile = '/dev/null'
	arg = '%s > %s 2>&1' % (arg, ofile)

	self.lastCommand = 'psql -p %d -e %s' % (self.PORT, arg);
	return os.system(self.lastCommand)

    def resetDatabase(self):
	print 'creating database:', self.DB_NAME
	self._execpsql('template1', cmd = 'drop database ' + self.DB_NAME)
	rc = self._execpsql('template1', cmd = 'create database ' + self.DB_NAME)
	if rc:
	    raise GPTestError('Error: cannot create database\n' % self.DB
			      + 'Last command: %s\n' % self.lastCommand)

    def psqlCmd(self, cmd):
	startTime = time.time()
	if self._execpsql(cmd=cmd): return False
	elapsed = time.time() - startTime
	return elapsed and elapsed or 0.1

    def psql(self, ifile):
	ofile = getOutFile(ifile)
	startTime = time.time()
	if self._execpsql(ifile=ifile, ofile=ofile): 
	    raise GPTestError("Error: cannot run psql command"
			      + "\tCommand: %s" % self.lastCommand)
	elapsed = time.time() - startTime
	return elapsed and elapsed or 0.1

    def checkResult(self, ifile):
	f1 = getOutFile(ifile)
	f2 = getResultFile(ifile)
	if not f2:
	    raise GPTestError('Error: cannot find result file for %s' % ifile
			      + '\nHint: output can be found in %s' % f1
			      + '\nHint: create a result (.s or .v) file for %s' % ifile)
	chksum = f2[len(f2)-1] == 's' and chksumSorted or chksumUnsorted
	return chksum(f1) == chksum(f2)

