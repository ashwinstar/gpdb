#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2009. All Rights Reserved. 
# This script handles gpsuspend work on a single machine
# gpsuspend will call this script on every machine in a cluster
# The machines to process will be passed on STDIN
# This script will do everything locally, the idea is the main gpsuspend will open
# a single ssh session for each machine, to make gpsuspend run faster
#
try:
    
    import sys
    from optparse import Option, OptionParser
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.gparray import *
    from gppylib.commands.gp import *
    from gppylib.db import dbconn
    from gppylib.gpcoverage import GpCoverage

except ImportError, e:    
    sys.exit('Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e))

_help = ["""
"""]

def parseargs():

    parser = OptParser(option_class=OptChecker)

    parser.setHelp(_help)

    parser.remove_option('-h')
    parser.add_option('-h', '-?', '--help', action='help', help='show this help message and exit')

    parser.add_option('--pause', action='store_true', help='pause greenplum db')
    parser.add_option('--resume', action='store_true', help='resume greenplum db')

    parser.add_option('-v','--verbose', action='store_true', help='debug output.')

    # Parse the command line arguments
    (options, args) = parser.parse_args()

    # sanity check 
    if options.pause and options.resume:
        print "Select either --pause or --resume, see --help for more info"
        sys.exit(1)

    if (not options.pause) and (not options.resume):
        print "Select either --pause or --resume, see --help for more info"
        sys.exit(1)

    return options


def suspend_segments(options, dblist):
    for seg in dblist:
        pausePg(seg)

    
def resume_segments(options, dblist):
    for seg in dblist:
        resumePg(seg)
 

def getSegmentsFromStdin():

    dblist=[]

    line = sys.stdin.readline()

    while line:

        dblist.append( GpDB.initFromString(line) )

        line = sys.stdin.readline()

    return dblist

#------------------------------- Mainline --------------------------------
coverage = GpCoverage()
coverage.start()

try:
    options = parseargs()
    
    # segments is a list of GpDB objects
    dblist = getSegmentsFromStdin()
    
    if options.pause:
        suspend_segments(options, dblist)
    
    elif options.resume:
        resume_segments(options, dblist)
finally:
    coverage.stop()
    coverage.generate_report()