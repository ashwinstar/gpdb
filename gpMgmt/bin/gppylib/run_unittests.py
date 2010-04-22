#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008.  All Rights Reserved
#
# crude tool used for running unit tests with or without code coverage.
#
# usage ./run_unittests.py  run from the gpMgmt/bin/gppylib directory
#
# at the end of this script there will be a summary of the 
import os,sys
import unittest
from gppylib.gpcoverage import GpCoverage


testfiles = []


path = os.path.abspath(os.path.dirname(__file__))


# Delete figleaf files
coverage = GpCoverage()
coverage.delete_files()

coverage.start()

for root,dirs,files in os.walk(path):
        for f in files:
            if f.startswith("test") and not f.endswith(".pyc"):
                subdir = root.replace(path,'')
                test = f.split('.')[0]                
                if subdir == '':                   
                    #print "Adding test: %s" % test
                    testfiles.append(test)
                else:
                    test = subdir.lstrip('/').replace('/','.') + "." + test
                    #print "Adding test: %s" % test
                    testfiles.append(test)
          


testLoader = unittest.TestLoader()

modules = []
for n in testfiles:
    #print "trying to import: " + n
    m = __import__(n, globals(), locals())
    #python import is strange with modules with '.' in them.    
    if n.find('.') != -1:
        components = n.split('.')
        for comp in components[1:]:
            mod = getattr(m, comp)            
            modules.append(mod)
    else:
        modules.append(m)


            
tests= []
#poor man's cdbfast
load = unittest.defaultTestLoader.loadTestsFromModule
tests = unittest.TestSuite(map(load,modules))

t = unittest.TextTestRunner(verbosity=3)
result = t.run(tests)

errorCount = len(result.errors)
failureCount = len(result.failures)
totalCount = result.testsRun
if totalCount == 0: totalCount = 1  #avoid division by 0 later

print ('--- SUMMARY: %d total, %d error, %d failure -- '
        % (totalCount, errorCount, failureCount))
score = '%.2f' % (100 - (errorCount + failureCount) * 100 / totalCount)
if errorCount + failureCount:
    if score == '100.00':
        score = '99.9'

print ('--- SCORE: %s%% --' % score)


coverage.stop()
coverage.generate_report()
