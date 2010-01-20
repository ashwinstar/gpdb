#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

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
from gppylib.interface.configurationInterface import *
from gppylib.db import dbconn

logger = get_default_logger()

class ConfigurationProviderForTesting(ConfigurationProvider) :

    __testSegments=None

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
    # Returns a new ConfigurationForSystem object
    #
    def loadSystemConfig( self ) :
        result = ConfigurationForSystem()
        for segment in self.__testSegments:
            result.addSegment( segment.copy())
        return result

    #
    # Update the configuration for the given segments in the underlying configuration store
    #   to match the current values
    #
    def updateSystemConfig( self, systemConfiguration ) :
        self.__testSegments = []
        for segment in systemConfiguration.getSegmentList():
            self.addTestSegment(segment)

    #
    # add a test segment.  The segment is COPIED before adding
    #
    def addTestSegment(self, configForSegment):
        self.__testSegments.append(configForSegment.copy())

