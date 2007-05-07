import unittest, os, popen2, time, sys, getopt


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


class GPTestResult(unittest._TextTestResult):
    def __init__(self, stream, descriptions):
	unittest._TextTestResult.__init__(self, stream, descriptions, verbosity=2)
	self.startTime = 0
	self.endTime = 0
	
    def startTest(self, test):
	self.startTime = time.time()
	return unittest._TextTestResult.startTest(self, test)

    def addSuccess(self, test):
	unittest.TestResult.addSuccess(self, test)
	self.endTime = time.time()
	if self.showAll:
	    self.stream.writeln('ok - %3.2f sec' % (self.endTime - self.startTime))
	elif self.dots:
	    self.stream.write('.')


class GPTestRunner(unittest.TextTestRunner):
    def _makeResult(self):
	return GPTestResult(self.stream, self.descriptions)


LOG_FILE = None
def writeLog(m, m1 = None, m2 = None, m3 = None):
    global LOG_FILE
    if not LOG_FILE: LOG_FILE = open('gptest.log', 'a')
    LOG_FILE.write(m)
    if m1: 
	LOG_FILE.write(m1)
	if m2: 
	    LOG_FILE.write(m2)
	    if m3:
		LOG_FILE.write(m3)
    LOG_FILE.write('\n')
    LOG_FILE.flush()


class GPTestCase(unittest.TestCase):

    LOAD_FLAG = False
    DB_NAME = 'gptest'
    PORT = os.getenv('PGPORT')
    if not PORT:
	sys.exit('Error: PGPORT env variable not set (for psql)')
    try:
	PORT = int(PORT)
    except ValueError:
	sys.exit('Error: PGPORT env variable is not valid (%s)', PORT)
	
    def __init__(self, name):
	self.lastCommand = ''
	unittest.TestCase.__init__(self, name)

    # override unittest.TestCase.defaultTestResult
    def defaultTestResult(self):
	return GPTestResult()

    def log(self, m, m1 = None, m2 = None, m3 = None):
	return writeLog(m, m1, m2, m3)

    def _execpsql(self, db = None, ifile = None, ofile = None, cmd = None):

	if not db: db = self.DB_NAME
	arg = db

	if cmd: arg = '%s -c "%s"' % (arg, cmd)
	elif ifile: arg = arg + ' < ' + ifile

	if not ofile: ofile = '/dev/null'
	arg = '%s > %s 2>&1' % (arg, ofile)

	self.lastCommand = 'psql -p %d -e %s' % (self.PORT, arg);
	self.log('EXEC: ', self.lastCommand)
	rc = os.system(self.lastCommand)
	self.log('  RC: ', str(rc))
	return rc


    def resetDatabase(self):
	print 'creating database:', self.DB_NAME
	rc = self._execpsql('template1', 
			    cmd = 'drop database if exists %s; create database %s' % (self.DB_NAME, self.DB_NAME))
	if rc:
	    raise GPTestError('Error: cannot create database %s\n' % self.DB_NAME
			      + 'Last command: %s\n' % self.lastCommand)
	print 'database created'
	
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


defaultTestRunner = GPTestRunner()


class TestProgram(unittest.TestProgram):

    USAGE = '''\
Usage: %(progName)s [option] [test] [...]

Options: 
	-?   show this message
	-l   set reload flag (tests will drop and recreate gptest database)

Examples:
	%(progName)s                       - run default set of tests
	%(progName)s mytestsuite           - run suite 'mytestsuite'
	%(progName)s tpch1.testQuery05     - run tpch1.testQuery05
	%(progName)s tpch1                 - run all test* methods in tpch1
'''

    def __init__(self, module='__main__', defaultTest=None, argv=None):
	unittest.TestProgram.__init__(self, module=module, 
				      defaultTest=defaultTest, 
				      argv=argv, 
				      testRunner=defaultTestRunner)
    
    def usageExit(self, msg = 0):
	print self.USAGE % self.__dict__
	sys.exit(msg)

    def parseArgs(self, argv):
	try:
	    options, args = getopt.getopt(argv[1:], '?hvql')
	    for opt, value in options:
		if opt == '-l': 
		    GPTestCase.LOAD_FLAG = True
		elif opt == '-h' or opt == '-?':
		    self.usageExit()
	except getopt.error, msg:
	    self.usageExit(msg)

	args.insert(0, argv[0])
	args.insert(1, '-v')
	return unittest.TestProgram.parseArgs(self, args)


main = TestProgram

