#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

""" Unittesting for gplog module
"""

import unittest

import gplog

class gplogTestCase(unittest.TestCase):
    
    def testBasics(self):
        self.assertTrue(gplog.LOGGER is None)
        logger=gplog.get_default_logger()
        self.assertTrue(gplog.LOGGER is not None)
        pass


#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()
