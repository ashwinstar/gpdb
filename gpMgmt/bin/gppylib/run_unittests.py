#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008.  All Rights Reserved
#
# crude tool used for running unit tests with or without code coverage using figleaf.
#
# usage ./run_unittests.py  run from the gpMgmt/bin/gppylib directory
#
# at the end of this script there will be a summary of the 
import os,sys
import unittest
import figleaf

testfiles = []


path = os.path.abspath(os.path.dirname(__file__))


print "Deleting previous .figleaf files"
os.system('rm .figleaf')


for root,dirs,files in os.walk(path):
        for f in files:
            if f.startswith("test") and not f.endswith(".pyc"):
                print "Adding test %s/%s" % (root,f)
                subdir = root.strip(path)
                test = f.split('.')[0]
                if subdir == '':
                    testfiles.append(test)
                else:
                    testfiles.append(subdir + "/" + test)
          


figleaf.start()
testLoader = unittest.TestLoader()

modules = []
for n in testfiles:
    m = __import__(n, globals(), locals())
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


figleaf.stop()
figleaf.write_coverage('.figleaf',append=True)



# generate html report
cmd = "../ext/figleaf/figleaf2html -d figleaf_report .figleaf "
print "Executing cmd: '%s'" % cmd
result = os.system(cmd) 

