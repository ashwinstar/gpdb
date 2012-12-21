#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2012. All Rights Reserved. 
#

""" Unittesting for gphostcachelookup module
"""
import os, sys
import unittest2 as unittest
from gppylib import gplog
from gppylib.commands.base import Command, ExecutionError

logger = gplog.get_unittest_logger()

sys.path.append(os.path.join(os.getenv('GPHOME'), 'bin', 'lib'))

class GpHostCacheLookupTestCase(unittest.TestCase):

    LOOKUP_FAIL_MSG = '__lookup_of_hostname_failed__'


    def test01_gphost_lookup(self):
        """ Test gphostcachelookup for known interface (present in host cache)"""

        test_interface = "localhost"
        hostname = os.uname()[1]

        result = self._run_gphost_lookup(test_interface)

        self.assertEqual(hostname, result)


    def test02_gphost_lookup(self):
        """Test gphostcachelookup for unknown interface (not present in host cache)"""

        test_interface = "foo"

        result = self._run_gphost_lookup(test_interface)

        self.assertIn(self.LOOKUP_FAIL_MSG, result)


    def test03_gphost_lookup(self):
        """Test gphostcachelookup for empty interface name """

        test_interface = ""

        result = self._run_gphost_lookup(test_interface)

        self.assertIn(self.LOOKUP_FAIL_MSG, result)


#-------------------------------- Non-test Helper -----------------------

    def _run_gphost_lookup(self, test_interface):
        """This is a helper function to simply run 'gphostcachelookup.py' for given interface"""

        
        cmd = Command("Running host lookup for given interface",
                        "echo %s | python -m lib.gphostcachelookup" % test_interface)

        try:
            cmd.run(validateAfter = True)
            
        except ExecutionError, e:
            self.fail("ExecutionError %s" % str(e))

        result = cmd.get_results().stdout.strip()
        return result



#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()
