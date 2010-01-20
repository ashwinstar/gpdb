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
from tempfile import NamedTemporaryFile

logger = get_default_logger()

class GpTempFileForTesting:

    def __init__(self, path):
        checkNotNone("path", path)

        self.name = path
        self.__underlyingFile = NamedTemporaryFile('w', delete=True)

    def flush(self):
        checkNotNone("file", self.__underlyingFile) # already closed?

        self.__underlyingFile.flush()

    def write(self, data):
        checkNotNone("file", self.__underlyingFile) # already closed?

        self.__underlyingFile.write(data)

    def close(self):
        self.__underlyingFile.close()
        self.__underlyingFile = None

#
# An implementation of GpFileSystemProvider that passes operations through to the underlying
#  operating system
#
class GpFileSystemProviderForTest :
    def __init__(self):
        self.__tempNamedFileCounter = 0
        pass

    #
    # Create a temporary file
    #
    # returns self
    #
    def createNamedTemporaryFile( self ) :
        path = "/tmp/temporaryNamedFile%s" % self.__tempNamedFileCounter
        self.__tempNamedFileCounter += 1
        return GpTempFileForTesting( path)
