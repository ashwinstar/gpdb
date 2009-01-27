#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

""" Unittesting for pg8000 module
"""
import logging
import unittest

try: 
    from pg8000 import *
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))



import gplog
from db.dbconn import *

logger=gplog.get_default_logger()
gplog.enable_verbose_logging()


class pg8000TestCase(unittest.TestCase):

    
    
    def test_connect(self):
        #def connect(user, host=None, unix_sock=None, port=5432, database=None, password=None, socket_timeout=60, ssl=False, options=None):
        dburl = DbURL()        
        self.failUnlessRaises(TypeError,DBAPI.connect)        
        self.failUnlessRaises(ProgrammingError,DBAPI.connect,dburl.pguser)        
        conn=DBAPI.connect(dburl.pguser,host=dburl.pghost,database=dburl.pgdb)
        conn.close()
        
    
    def test_nonexist_objects(self):
        dburl=DbURL()
        conn=DBAPI.connect(dburl.pguser,host=dburl.pghost,database=dburl.pgdb)
        curs=conn.cursor()
        curs.execute("DROP TABLE notable")
        
        
    def test_datatypes(self):
        #
        dburl=DbURL()
        conn=DBAPI.connect(dburl.pguser,host=dburl.pghost,database=dburl.pgdb)
        curs=conn.cursor()
        curs.execute("CREATE TABLE tab1 ( a int )")
        curs.execute("INSERT INTO tab1 VALUES 1")
        curs.execute("INSERT INTO tab1 VALUES 100")
        curs.execute("INSERT INTO tab1 VALUES -10000")
        curs.execute("DROP TABLE tab1)")
        curs.commit()
    
    
            

#------------------------------- non-test helper --------------------------------

#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()

