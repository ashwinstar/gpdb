#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

"""
todo MIRRORING implement this!
"""
import os

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.faultProberInterface import GpFaultProber

logger = get_default_logger()

class GpFaultProberImplGpdb(GpFaultProber):
    def __init__(self):
        self.__isPaused = False

    def pauseFaultProber(self):
        assert not self.__isPaused
        self.__isPaused = True

    def unpauseFaultProber(self):
        assert self.__isPaused
        self.__isPaused = False

    def isFaultProberPaused(self):
        return self.__isPaused
