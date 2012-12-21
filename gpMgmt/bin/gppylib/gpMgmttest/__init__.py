import unittest2 as unittest

class GpMgmtTestRunner(unittest.TextTestRunner):
    def _makeResult(self):
        return GpMgmtTextTestResult(self.stream, self.descriptions, self.verbosity)

class GpMgmtTextTestResult(unittest.TextTestResult):

    def __init__(self, stream, descriptions, verbosity):
        super(GpMgmtTextTestResult, self).__init__(stream, descriptions, verbosity)
        self.verbosity = verbosity

    def getDescription(self, test):
        if self.verbosity > 1:
            if test.shortDescription():
                return '%s ... %s' % (test.__str__(), test.shortDescription())
            else:
                return '%s' % test.__str__()

    def startTest(self, test):
        super(GpMgmtTextTestResult, self).startTest(test)

    def addSuccess(self, test):
        super(GpMgmtTextTestResult, self).addSuccess(test)

    def addError(self, test, err):
        super(GpMgmtTextTestResult, self).addError(test, err)

    def addFailure(self, test, err):
        super(GpMgmtTextTestResult, self).addFailure(test, err)

    def addSkip(self, test, err):
        super(GpMgmtTextTestResult, self).addSkip(test, err)


