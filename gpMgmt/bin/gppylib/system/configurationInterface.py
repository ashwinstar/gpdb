#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2009. All Rights Reserved.
#

"""

This file defines the interface that can be used to
   fetch and update system configuration information,
   as well as the data object returned by the

"""
import os, copy

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
    # Returns a new ConfigurationForSystem object
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
# This is the data structure that will be returned by a GpConfigurationProvider
#
#
class ConfigurationForSystem :

    # all of the segments, including master and QEs, etc. marked as dirty has been changed since last fetched from
    #   or updated in the configuration provider
    __segments=None

    #   we don't support deleting segments right now    __segmentsToDelete=None

    def __init__(self):
        self.__segments=[]
        pass

    def addSegment(self, segment) :
        self.__segments.append(segment)

    #
    # return a shallow copy of the segment list
    #
    def getSegmentList(self) :
        return [x for x in self.__segments]

    #
    #
    #
    def getPeersAndEnsureFoundPeers(self, segs):
        # todo MIRRORING
        pass
#
# Represents the configuration for a single segment
#
# todo MIRRORING add in san_mounts reading/writing
#
class ConfigurationForSegment :
    __dbId=None

    __contentId=None

    __role=None
    __preferredRole=None

    __mode=None
    __status=None

    __port=None
    __replicationPort=None
    __hostName=None
    __address=None

    __dataDirectory=None

    __isDirty = False
    __isCreatedInDb = False

    def __init__(self):
        pass

    def __markDirtyIfDifferent(self, existingValue, newValue):
        if existingValue <> newValue:
            __isDirty = True
        return newValue

    def __str__(self):
        return "%s:%s:content=%s:dbid=%s" % (self.getSegmentHostName(), self.getSegmentDataDirectory(), \
                self.getSegmentContentId(), self.getSegmentDbId())

    #############################
    # getters
    #
    def getSegmentDbId(self):
        return checkNotNone("dbId", self.__dbId)

    def getSegmentContentId(self):
        return checkNotNone("contentId", self.__contentId)

    def getSegmentRole(self):
        return checkNotNone("role", self.__role)

    def getSegmentPreferredRole(self):
        return checkNotNone("preferredRole", self.__preferredRole)

    def getSegmentMode(self):
        return checkNotNone("mode", self.__mode)

    def getSegmentStatus(self):
        return checkNotNone("status", self.__status)

    #
    # Return the postmaster __port
    #
    def getSegmentPort(self):
        return checkNotNone("port", self.__port)

    def getSegmentReplicationPort(self):
        # is Nullable (so can return None)
        return self.__replicationPort

    def getSegmentHostName(self):
        # is Nullable (so can return None)
        return self.__hostName

    def getSegmentAddress(self):
        # is Nullable (so can return None)
        return self.__address

    def getSegmentDataDirectory(self):
        return checkNotNone("dataDirectory", self.__dataDirectory)

    #############################
    # setSegmentters
    #
    def setSegmentDbId(self, dbId):
        checkNotNone("dbId", dbId)
        self.__dbId = self.__markDirtyIfDifferent(self.__dbId, dbId)

    def setSegmentContentId(self, contentId):
        checkNotNone("contentId", contentId)
        if contentId < -1:
            raise Exception("Invalid content id %s" % contentId)
        self.__contentId = self.__markDirtyIfDifferent(self.__contentId, contentId)

    def setSegmentRole(self, role):
        checkNotNone("role", role)

        if role != 'm' and role != 'p':
            raise Exception("Invalid role '%s'" % role)

        self.__role = self.__markDirtyIfDifferent(self.__role, role)

    def setSegmentPreferredRole(self, preferredRole):
        checkNotNone("preferredRole", preferredRole)

        if preferredRole != 'm' and preferredRole != 'p':
            raise Exception("Invalid preferredRole '%s'" % preferredRole)

        self.__preferredRole = self.__markDirtyIfDifferent(self.__preferredRole, preferredRole)

    def setSegmentMode(self, mode):
        checkNotNone("mode", mode)

        if mode != 's' and mode != 'c' and mode != 'r':
            raise Exception("Invalid mode '%s'" % mode)

        self.__mode = self.__markDirtyIfDifferent(self.__mode, mode)

    def setSegmentStatus(self, status):
        checkNotNone("status", status)

        if status != 'd' and status != 'u':
            raise Exception("Invalid status '%s'" % status)

        self.__status = self.__markDirtyIfDifferent(self.__status, status)

    #
    # Return the postmaster port
    #
    def setSegmentPort(self, port):
        checkNotNone("port", port)
        self.__port = self.__markDirtyIfDifferent(self.__port, port)

    def setSegmentReplicationPort(self, replicationPort):
        # None is allowed -- don't check
        self.__replicationPort = self.__markDirtyIfDifferent(self.__replicationPort, replicationPort)

    def setSegmentHostName(self, hostName):
        # None is allowed -- don't check
        self.__hostName = self.__markDirtyIfDifferent(self.__hostName, hostName)

    def setSegmentAddress(self, address):
        # None is allowed -- don't check
        self.__address = self.__markDirtyIfDifferent(self.__address, address)

    def setSegmentDataDirectory(self, dataDirectory):
        if self.__isCreatedInDb:
            raise Exception("Data directory may not be changed for rows in database")

        checkNotNone("dataDirectory", dataDirectory)
        self.__dataDirectory = dataDirectory

    #############################
    # Other functions
    #
    def resetSegmentDirtyBit(self):
        self.__isDirty = False

    def getSegmentIsDirty(self):
        return self.__isDirty

    def markSegmentCreatedInDb(self):
        self.__isCreatedInDb = True

    def isSegmentCreatedInDb(self):
        return self.__isCreatedInDb

    def isSegmentAMaster(self):
        return self.__contentId == -1

    def copy(self):
        return copy.copy(self) # a shallow copy is sufficent right now

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
