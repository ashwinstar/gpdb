#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
# Unit Testing of catalog module.
#

import unittest



import gppylib.gplog
import dbconn
import catalog

class catalogTestCase(unittest.TestCase):
    
    def setUp(self):
        dburl=dbconn.DbURL()
        self.conn = dbconn.connect(dburl)
        
    
    def tearDown(self):
        pass
    
    
    def test_gpid(self):
        orig_gpid_table = catalog.GpIdTable(self.conn)        
        self.assertEquals(orig_gpid_table.content,-1)
        
        test_gpid_table = catalog.GpIdTable(self.conn)
        test_gpid_table.gpname="name of my cluster"
        test_gpid_table.numsegments=100
        test_gpid_table.dbid=200
        test_gpid_table.content=300
        
        test_gpid_table.update_db(self.conn)
        
        new_gpid_table = catalog.GpIdTable(self.conn)
        self.validate_gpid_table(test_gpid_table,new_gpid_table)
        
        orig_gpid_table.update_db(self.conn)
        new_gpid_table = catalog.GpIdTable(self.conn)
        self.validate_gpid_table(orig_gpid_table,new_gpid_table)
        
        pass

    def test_versionatinitdb(self):
        orig_table = catalog.GpVersionAtInitdbTable(self.conn)
        test_table = catalog.GpVersionAtInitdbTable(self.conn)
        
        test_table.schemaversion=11
        test_table.productversion="Awesome Vaporware"
        test_table.update_db(self.conn)
        
        new_table = catalog.GpVersionAtInitdbTable(self.conn)
        
        self.validate_versionatinitdb_table(test_table,new_table)
        orig_table.update_db(self.conn)
        
        new_table = catalog.GpVersionAtInitdbTable(self.conn)        
        self.validate_versionatinitdb_table(orig_table,new_table)
        
    

#------------------------------- Helper --------------------------------
    def validate_gpid_table(self,orig_gpid_table,test_gpid_table):
        self.assertEquals(orig_gpid_table.gpname,test_gpid_table.gpname)
        self.assertEquals(orig_gpid_table.numsegments ,test_gpid_table.numsegments)
        self.assertEquals(orig_gpid_table.dbid ,test_gpid_table.dbid)
        self.assertEquals(orig_gpid_table.content ,test_gpid_table.content)
    
    def validate_versionatinitdb_table(self,orig_table,test_table):
        self.assertEquals(orig_table.schemaversion,test_table.schemaversion)
        self.assertEquals(orig_table.productversion,orig_table.productversion)
        



#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()    
