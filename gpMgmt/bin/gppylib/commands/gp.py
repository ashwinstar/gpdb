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


logger = get_default_logger()

#TODO:  need a better way of managing environment variables.
GPHOME=os.environ.get('GPHOME')

class PySync(Command):
    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None):
        psync_executable=GPHOME + "/bin/lib/pysync.py"
        self.cmdStr="%s %s %s:%s" % (psync_executable,srcDir,dstHost,dstDir)        
        Command.__init__(self,name,self.cmdStr,ctxt,remoteHost)

    
      