#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

import unittest
import pickle
from gppylib.commands.base import ExecutionError
from gppylib.operations import Operation
from gppylib.operations.utils import RemoteOperation

# TODO: much of this code could be improved with assertion context managers that 
# exist in a later version of unit test, I believe

class utilsTestCase(unittest.TestCase):
    def test_Remote_basic(self):
        """ Basic RemoteOperation test """
        self.assertTrue(TestOperation().run() == RemoteOperation(TestOperation(), "localhost").run())

    def test_Remote_exceptions(self):
        """ Test that an Exception returned remotely will be raised locally. """
        try:
            RemoteOperation(RaiseOperation(), "localhost").run()
        except MyException, e: 
            pass
        else:
            self.fail("RaiseOperation should have thrown a MyException")

    def test_inner_exceptions(self):
        """ Verify that an object not at the global level of this file cannot be pickled properly. """
        try:
            RemoteOperation(RaiseOperation2(), "localhost").run()
        except ExecutionError, e:
            self.assertTrue(e.cmd.get_results().stderr.strip().endswith("raise RaiseOperation2.MyException2()"))
        else:
            self.fail("A PicklingError should have been caused remotely, because RaiseOperation2 is not at the global-level.")

    def test_unsafe_exceptions_with_args(self):
        try:
            RemoteOperation(RaiseOperation3(), "localhost").run()
        except TypeError, e:            # Because Exceptions don't retain init args, they are not pickle-able normally      
            pass
        else:
            self.fail("RaiseOperation3 should have caused a TypeError, due to an improper Exception idiom. See test_utils.ExceptionWithArgsUnsafe")
            
    def test_proper_exceptions_sanity(self):
        try:    
            RemoteOperation(RaiseOperation4(), "localhost").run()
        except ExceptionWithArgs, e:
            pass
        else:
            self.fail("ExceptionWithArgs should have been successfully raised + caught, because proper idiom is used.")

    def test_proper_exceptions_with_args(self):
        try:
            RemoteOperation(RaiseOperation4(), "localhost").run()
        except ExceptionWithArgs, e:
            self.assertTrue(e.x == 1 and e.y == 2)
        else:
            self.fail("RaiseOperation4 should have thrown ExceptionWithArgs(1, 2)")

    # It is crucial that the RMI is debuggable!
    def test_Remote_harden(self):
        """ Ensure that some logging occurs in event of error. """
        # One case encountered thus far is the raising of a pygresql DatabaseError,
        # which due to the import from a shared object (I think), does not behave
        # nicely in terms of imports and namespacing. """
        try:
            RemoteOperation(RaiseOperation5(), "localhost").run()
        except ExecutionError, e:
            self.assertTrue(e.cmd.get_results().stderr.strip().endswith("raise pg.DatabaseError()"))
        else:
            self.fail("""A pg.DatabaseError should have been raised remotely, and because it cannot 
                         be pickled cleanly (due to a strange import in pickle.py),
                         an ExecutionError should have ultimately been caused.""")
        # TODO: Check logs on disk. With gplogfilter?

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

class RaiseOperation5(Operation):
    def execute(self):
        from pygresql import pg
        raise pg.DatabaseError()

if __name__ == '__main__':
    unittest.main()
