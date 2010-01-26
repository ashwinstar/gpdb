#!/usr/bin/env python
'''
gphostcachelookup.py -- look up the hostname for a list of interfaces

Usage: gphostcachelookup.py -i inputFile

'''

#
# THIS IMPORT MUST COME FIRST
# import mainUtils FIRST to get python version check
#
from gppylib.mainUtils import *

# now reset of imports
from gppylib.programs.clsHostCacheLookup import *

#-------------------------------------------------------------------------
if __name__ == '__main__':
    mainOptions = {}
    mainOptions['forceQuietOutput'] = True
    simple_main( GpHostCacheLookup.createParser, GpHostCacheLookup.createProgram, mainOptions)

