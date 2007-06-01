import unittest, os, popen2, time, sys, getopt


def getOutFile(fname):
    return os.path.splitext(fname)[0] + '.out'


def getAnswerFile(fname):
    return os.path.splitext(fname)[0] + '.ans'


def chkDiff(f1, f2):
    return os.system('gpdiff.pl -w -I NOTICE: -I HINT: -I CONTEXT: -I GP_IGNORE: %s %s > /dev/null 2>&1' % (f1, f2))

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


class GPTestError(Exception): pass


class GPTestResult(unittest._TextTestResult):
    def __init__(self, stream, descriptions):
	unittest._TextTestResult.__init__(self, stream, descriptions, verbosity=2)
	self.startTime = 0
	self.endTime = 0
	
    def startTest(self, test):
	self.startTime = time.time()
	writeLog(self.separator1)
	writeLog(self.getDescription(test), ' STARTED ', time.ctime())
	writeLog(self.separator2)
	return unittest._TextTestResult.startTest(self, test)

    def addSuccess(self, test):
	unittest.TestResult.addSuccess(self, test)
	elapsed = time.time() - self.startTime
	if self.showAll:
	    self.stream.writeln('ok - % 4.2f sec' % elapsed)
	elif self.dots:
	    self.stream.write('.')

	writeLog(self.getDescription(test), ' ok - % 4.2f sec' % elapsed)

    def addError(self, test, err):
	writeLog(self.getDescription(test), ' ERROR')
	writeLog('\n%s' % str(err))
	unittest._TextTestResult.addError(self, test, err)
	
    def addFailure(self, test, err):
	writeLog(self.getDescription(test), ' FAILURE')
	writeLog('\n%s' % str(err))
	unittest._TextTestResult.addFailure(self, test, err)
	

class GPTestRunner(unittest.TextTestRunner):
    def _makeResult(self):
	return GPTestResult(self.stream, self.descriptions)



class GPTestCase(unittest.TestCase):

    LOAD_FLAG = False
    SANITY_CHECKED_FLAG = False
    DB_NAME = 'gptest'
	
    def __init__(self, name):
	self.lastCommand = ''
	unittest.TestCase.__init__(self, name)

    # override unittest.TestCase.defaultTestResult
    def defaultTestResult(self):
	return GPTestResult()

    def log(self, m, m1 = None, m2 = None, m3 = None):
	return writeLog(m, m1, m2, m3)

    def _execpsql(self, db = None, ifile = None, ofile = None, cmd = None):

	if not GPTestCase.SANITY_CHECKED_FLAG:
	    GPTestCase.SANITY_CHECKED_FLAG = True
	    rc = self._execpsql("template1", cmd='\l')
	    if rc:
		sys.exit('Unable to run psql ...\nPlease check the PGPORT/PGHOST env\n')
	    self._execpsql("template1", cmd="create database %s" % self.DB_NAME)
	    rc = self._execpsql(cmd = "select version()", ofile="VERSION")
	    if rc:
		sys.exit("Unable to connect to database %s", self.DB_NAME)

	if not db: db = self.DB_NAME
	arg = db

	if cmd: arg = '%s -c "%s"' % (arg, cmd)
	elif ifile: arg = arg + ' < ' + ifile

	if not ofile: ofile = '/dev/null'
	arg = '%s > %s 2>&1' % (arg, ofile)

	self.lastCommand = 'psql -e %s' % arg
	self.log('\tEXEC: ', self.lastCommand)
	rc = os.system(self.lastCommand)
	self.log('\t  RC: ', str(rc))
	return rc


    def resetDatabase(self):
	print 'creating database:', self.DB_NAME
	rc = self._execpsql('template1', 
			    cmd = 'drop database if exists %s' % self.DB_NAME)
	rc = self._execpsql('template1', 
			    cmd = 'create database %s' % self.DB_NAME)
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
	f2 = getAnswerFile(ifile)
	if not os.access(f1, os.R_OK):
	    raise GPTestError('Error: cannot find out file for %s' % ifile)
	if not os.access(f2, os.R_OK):
	    raise GPTestError('Error: cannot find answer file for %s' % ifile
			      + '\nHint: output can be found in %s' % f1
			      + '\nHint: create an answer (%s) file for %s' % (f2, ifile))
	return 0 == chkDiff(f1, f2)


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

