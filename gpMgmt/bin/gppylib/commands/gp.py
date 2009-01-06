#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

"""
TODO: docs!
"""
import os

from gppylib.gplog import *
from base import *
from unix import *



logger = get_default_logger()

#TODO:  need a better way of managing environment variables.
GPHOME=os.environ.get('GPHOME')

class PySync(Command):
    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None):
        psync_executable=GPHOME + "/bin/lib/pysync.py"
        self.cmdStr="%s %s %s:%s" % (psync_executable,srcDir,dstHost,dstDir)        
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

    
      

    
def distribute_tarball(queue,list,tarball):
        logger.debug("distributeTarBall start")        
        for db in list:
            (head,tail)=os.path.split(db.datadir)
            scp_cmd=RemoteCopy("copy master",tarball,db.get_hostname(),head)            
            queue.addCommand(scp_cmd)
        queue.join()        
        queue.check_results()        
        logger.debug("distributeTarBall finished")
        