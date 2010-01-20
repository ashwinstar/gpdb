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

#
# An implementation of GpFileSystemProvider that passes operations through to the underlying
#  operating system
#
class GpFileSystemProviderUsingOs :
    def __init__(self):
        pass

    #
    # Shutdown the file system provider
    #
    def destroy(self):
        pass

    #
    # Create a temporary file
    #
    # returns self
    #
    # todo MIRRORING I don't quite understand the description of NamedTemporaryFile's limitation with regard
    #      to opening the file again
    #
    def createNamedTemporaryFile( self ) :
        return NamedTemporaryFile('w', delete=True)
