#!/usr/bin/env python

import os

from gppylib.gplog import *
from gppylib.db import dbconn
from gppylib.db import catalog
from gppylib.commands.base import *
from gppylib.commands.unix import *
from gppylib.commands.pg import * 
from gppylib import pgconf

logger = get_default_logger()
GPHOME=os.environ.get('GPHOME')
SRC_GPPATH=". %s/greenplum_path.sh;" % GPHOME
#SRC_GPPATH="/opt/gpcheckos/lib"
#-----------------------------------------------
class GpCheckosCmd(Command):
    def __init__(self, name, gphost, srcHost, verbose=False,ctxt=LOCAL, remoteHost=None):
        self.gphost=gphost
        self.srcHost=srcHost            

        if verbose:
            setverbose=" -v "
        else:
            setverbose=""
        cmdStr="%s/lib/python/gppylib/gpcheckos.py -s %s "%(GPHOME,self.srcHost)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-----------------------------------------------
