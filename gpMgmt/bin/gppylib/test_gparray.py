#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

""" Unittesting for gplog module
"""
import logging
import unittest

from gparray import *
import gplog

logger = gplog.get_unittest_logger()
class gplogTestCase(unittest.TestCase):
    def setUp(self):
        pass
        
    def test_spreadmirror_layout(self):
        logger.info("test_spreadmirror_layout")
        mirror_type='spread'
        primary_portbase=5000
        mirror_portbase=6000
        hostlist=['host1']
        primary_list=['/db1']
        mirror_list=['/mir1']
        
        #need to have enough hosts otherwise we get exceptions
        self.failUnlessRaises(Exception,createSegmentRows,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        primary_list.append('/db2')
        mirror_list.append('/mir2')
        self.failUnlessRaises(Exception,createSegmentRows,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        hostlist.append('host2')
        self.failUnlessRaises(Exception,createSegmentRows,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        
        #now we have enough
        hostlist.append('host3')
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        
        
        #enough
        hostlist=['host1','host2','host3']
        primary_list=['/db1','/db2']
        mirror_list=['/mir1','/mir2']
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)

        #typical thumper
        hostlist=['sdw1','sdw2','sdw3','sdw4','sdw5']
        primary_list=['/dbfast1','/dbfast2','/dbfast3','/dbfast4']
        mirror_list=['/dbfast1/mirror','/dbfast2/mirror','/dbfast3/mirror','/dbfast4/mirror']
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)

        #typical Thor
        hostlist=['sdw1','sdw2','sdw3','sdw4','sdw5','sdw6','sdw7','sdw8','sdw9']
        primary_list=['/dbfast1','/dbfast2','/dbfast3','/dbfast4','/dbfast5','/dbfast6','/dbfast7','/dbfast8']
        mirror_list=['/dbfast1/mirror','/dbfast2/mirror','/dbfast3/mirror','/dbfast4/mirror',
                     '/dbfast5/mirror','/dbfast6/mirror','/dbfast7/mirror','/dbfast8/mirror']
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)

    def test_groupmirror_layout(self):
        logger.info("test_groupmirror_layout")
        mirror_type='grouped'
        primary_portbase=5000
        mirror_portbase=6000
        hostlist=['host1']
        primary_list=['/db1']
        mirror_list=['/mir1']
        
        #not enough
        self.failUnlessRaises(Exception,createSegmentRows,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        primary_list.append('/db2')
        mirror_list.append('/mir2')
        self.failUnlessRaises(Exception,createSegmentRows,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)
        
        #enough
        hostlist=['host1','host2']
        primary_list=['/db1','/db2']
        mirror_list=['/mir1','/mir2']
        
         #typical thumper
        hostlist=['sdw1','sdw2','sdw3','sdw4','sdw5']
        primary_list=['/dbfast1','/dbfast2','/dbfast3','/dbfast4']
        mirror_list=['/dbfast1/mirror','/dbfast2/mirror','/dbfast3/mirror','/dbfast4/mirror']
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)

        #typical Thor
        hostlist=['sdw1','sdw2','sdw3','sdw4','sdw5','sdw6','sdw7','sdw8','sdw9']
        primary_list=['/dbfast1','/dbfast2','/dbfast3','/dbfast4','/dbfast5','/dbfast6','/dbfast7','/dbfast8']
        mirror_list=['/dbfast1/mirror','/dbfast2/mirror','/dbfast3/mirror','/dbfast4/mirror',
                     '/dbfast5/mirror','/dbfast6/mirror','/dbfast7/mirror','/dbfast8/mirror']
        
        self.mirrorlayout_test(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase)


#------------------------------- non-test helper --------------------------------
    def mirrorlayout_test(self,hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase):
        master=(-1,True,0,True,True,'masterhost',5432,'/masterdir')
        allrows=[]
        allrows.append(master)                 
        rows=createSegmentRows(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase,'seg')
        
        
        for r in rows:
            newrow=(r[0],convertBool(r[1]),r[2],convertBool(r[3]),convertBool(r[4]),r[5],r[6],r[7])                    
            allrows.append(newrow)
        
        gparray=GpArray(allrows)
                
        self.validateArray(gparray)
    
        
    def validateArray(self,gparray): 
        portdict={}
        lastport=0
        for seg in gparray.segments:
            prim=seg.primaryDB            
            mir=seg.mirrorDBs[0]
            self.assertNotEqual(prim.hostname,mir.hostname)
            if prim.port not in portdict:
                portdict[prim.port]=1
            else:
                portdict[prim.port] = 1 + portdict[prim.port]
            lastport=prim.port
            
        expected_count = portdict[lastport]
            
        for (port,count) in portdict.items():
            self.assertEquals(expected_count,count)
            
    
def convertBool(val):
    if val == 't':
        return True
    else:
        return False   

#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()
