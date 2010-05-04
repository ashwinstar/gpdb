#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

"""

Note that this does NOT pay attention to the current pause state in the db.  It can't do that right now
  anyway because gp_fts_probe_pause does not reflect the shared memory variable?

"""
import os

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.faultProberInterface import GpFaultProber
from gppylib.db import dbconn

logger = get_default_logger()

class GpFaultProberImplGpdb(GpFaultProber):
    def __init__(self):
        self.__isPaused = False
        self.__masterDbUrl = None

    #
    # returns self
    #
    def initializeProber( self, masterPort ) :
        self.__masterDbUrl = dbconn.DbURL(port=masterPort, dbname='template1')
        return self

    def pauseFaultProber(self):
        assert not self.__isPaused
        assert self.__masterDbUrl is not None

        logger.debug("Pausing fault prober")
        
        conn = dbconn.connect(self.__masterDbUrl, True) # use utility mode so we don't do any segment connection stuff
        dbconn.execSQL( conn, "set gp_fts_probe_pause = on")
        conn.close()

        self.__isPaused = True

    def unpauseFaultProber(self):
        assert self.__isPaused
        assert self.__masterDbUrl is not None

        logger.debug("Unpausing fault prober")

        conn = dbconn.connect(self.__masterDbUrl, True) # use utility mode so we don't do any segment connection stuff
        dbconn.execSQL( conn, "set gp_fts_probe_pause = off")
        conn.close()
        self.__isPaused = False

    def isFaultProberPaused(self):
        return self.__isPaused
