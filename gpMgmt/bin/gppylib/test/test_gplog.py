#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

""" Unittesting for gplog module
"""
import logging
import unittest


import gplog

class gplogTestCase(unittest.TestCase):
    
    def test_basics(self):
        gplog.LOGGER=None
        self.assertTrue(gplog.LOGGER is None)
        logger=gplog.get_default_logger()
        self.assertTrue(gplog.LOGGER is not None)
        pass

    def test_set_loglevels(self):
        logger=gplog.get_default_logger()
        self.assertLogLevel(logger,logging.INFO)
        gplog.enable_verbose_logging()
        self.assertLogLevel(logger,logging.DEBUG)        
        

#------------------------------- non-test helper --------------------------------
    def assertLogLevel(self,logger,level): 
        self.assertTrue(logger.getEffectiveLevel()==level)

#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()
