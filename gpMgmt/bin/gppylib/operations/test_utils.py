#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

import unittest
import pickle
from gppylib.commands.base import ExecutionError
from gppylib.operations import Operation
from gppylib.operations.utils import RemoteOperation

class utilsTestCase(unittest.TestCase):
    def test_Remote_basic(self):
        """ Basic RemoteOperation test """
        self.assertTrue(TestOperation().run() == RemoteOperation(TestOperation(), "localhost").run())

    def test_Remote_exceptions(self):
        """ Test that an Exception returned remotely will be raised locally. """
        try:
            RemoteOperation(RaiseOperation(), "localhost").run()
        except MyException, e: 
            return
        self.fail("RaiseOperation should have thrown a MyException")

    def test_inner_exceptions(self):
        """ Verify that an object not at the global level of this file cannot be pickled properly. """
        try:
            RemoteOperation(RaiseOperation2(), "localhost").run()
        except ExecutionError, e:
            return
        self.fail("A PicklingError should have been raised remotely, because RaiseOperation2 is not at the global-level.")

    def test_unsafe_exceptions_with_args(self):
        try:
            RemoteOperation(RaiseOperation3(), "localhost").run()
        except TypeError, e:            # Because Exceptions don't retain init args, they are not pickle-able normally      
            return
        RemoteOperation(RaiseOperation4(), "localhost").run()

    def test_proper_exceptions_with_args(self):
        try:
            RemoteOperation(RaiseOperation4(), "localhost").run()
        except ExceptionWithArgs, e:
            self.assertTrue(e.x == 1 and e.y == 2)
            return
        self.fail("RaiseOperation4 should have thrown ExceptionWithArgs(1, 2)")

    # It is crucial that the gppickle-RMI is debuggable!
    def test_Remote_harden(self):
        """ Ensure that some logging occurs in event of error. """
        operation = "thisismyoperation"         # clearly, this is not "run"nable
        RemoteOperation(operation, "localhost").run()
        pass

class TestOperation(Operation):
    def execute(self):
        return 1

class MyException(Exception): pass
class RaiseOperation(Operation):
    def execute(self):
        raise MyException()

# Exceptions cannot be nested. 
# They cannot be pickled for reasons inherent to python. See utils.py
class RaiseOperation2(Operation):
    def execute(self):
        raise RaiseOperation2.MyException2()
    class MyException2(Exception): pass

# Exceptions with args must follow a strange idiom! http://bugs.python.org/issue1692335
class RaiseOperation3(Operation):
    def execute(self):
        raise ExceptionWithArgsUnsafe(1, 2)

class RaiseOperation4(Operation):
    def execute(self):
        raise ExceptionWithArgs(1, 2)

# This is the proper idiom for a pickle-able exception with arguments: http://bugs.python.org/issue1692335
class ExceptionWithArgs(Exception):
    def __init__(self, x, y):
        self.x, self.y = x, y 
        Exception.__init__(self, x, y)
        
class ExceptionWithArgsUnsafe(Exception):
    def __init__(self, x, y):
        self.x, self.y = x, y

if __name__ == '__main__':
    unittest.main()
