#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2009. All Rights Reserved.
#

"""

This file defines the interface that can be used to
   fetch and update system configuration information,
   as well as the data object returned by the

"""
import os

from gppylib.gplog import *
from gppylib.utils import checkNotNone

logger = get_default_logger()

#
# todo MIRRORING: use enumerations (does python have them) for mode, status, etc. 
#
# An implementation of GpConfigurationProvider will provide functionality
#   to fetch and update gpdb system configuration information (as stored in the database)
#
class GpConfigurationProvider :
    def __init__(self):
        pass

    #
    # Initialize the provider to get information from the given master db, if it chooses to
    #   get its data from the database
    #
    # returns self
    #
    def initializeProvider( self, masterDbUrl, useUtilityMode ) :
        return self

    #
    # Load all segment information from the configuration source.
    #
    # Returns a new GpArray object
    #
    def loadSystemConfig( self ) :
        pass

    #
    # Update the configuration for the given segments in the underlying configuration store
    #   to match the current values
    #
    # Also resets any dirty bits on saved/updated objects
    #
    def updateSystemConfig( self, systemConfiguration ) :
        pass

#
# Management of registered configuration provider.  Right now it
#   is a singleton, so initializeDatabase calls _could_ mess up
#   the singleton for other parts of code.  Perhaps switch later
#   to a factory 
#
gConfigurationProvider = None
def registerConfigurationProvider(provider):
    global gConfigurationProvider
    gConfigurationProvider = checkNotNone("New global configuration provider", provider)

def getConfigurationProvider():
    global gConfigurationProvider
    return checkNotNone("Global configuration provider", gConfigurationProvider)
