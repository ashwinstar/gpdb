#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
from gppylib.gparray import GpArray

"""

This file defines the interface that can be used to
   fetch and update system configuration information
   for testing.  It does not use the database for
   getting and updating system configuration data

"""
import os

import copy

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.configurationInterface import *
from gppylib.db import dbconn

logger = get_default_logger()

class GpConfigurationProviderForTesting(GpConfigurationProvider) :

    def __init__(self):
        self.__testSegments=[]
        pass

    #
    # Initialize the provider to get information from the given master db, if it chooses to
    #   get its data from the database
    #
    # returns self
    #
    def initializeProvider( self, masterDbUrl, useUtilityMode ) :
        # verify here to match what the true one will require
        checkNotNone("masterDbUrl", masterDbUrl)
        checkNotNone("useUtilityMode", useUtilityMode)
        return self

    #
    # Load all segment information from the configuration source.
    #
    # Returns a new GpArray object
    #
    def loadSystemConfig( self ) :
        segments = []
        for segment in self.__testSegments:
            segments.append(segment.copy())
        return GpArray(segments)

    #
    # Update the configuration for the given segments in the underlying configuration store
    #   to match the current values
    #
    def updateSystemConfig( self, systemConfiguration ) :
        self.__testSegments = []
        for segment in systemConfiguration.getDbList():
            self.addTestSegment(segment)

    #
    # add a test segment.  The segment is COPIED before adding
    #
    def addTestSegment(self, configForSegment):
        self.__testSegments.append(configForSegment.copy())

