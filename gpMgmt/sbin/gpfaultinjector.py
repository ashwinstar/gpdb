#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
#
# Used to inject faults into the file replication code
#

# todo: MILENA should this not be checked in here?

# import mainUtils early to get python version check
from gppylib.mainUtils import *
import os, sys

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
from tempfile import NamedTemporaryFile

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.utils import toNonNoneString
from gppylib import gplog
from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.commands import pg
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib import pgconf


description = ("""
This utility is NOT SUPPORTED and is for internal-use only.

Used to inject faults into the file replication code.
""")

_help = ["""

Return codes:
  0 - Fault injected
  1 - Failed to contact at least one segment for fault injection
  2 - Exception occurred

"""]


#-------------------------------------------------------------------------
class GpFaultInjector:
    ######
    def __init__(self,options):
        self.options=options
        pass

    #
    # Build the message.  Fault options themselves will NOT be validated by the
    #   client -- the server will do that when we send the fault
    #
    # todo: MILENA is this okay?  We could verify fault names here but then adding/changing them is a two-place change
    #
    def buildMessage(self) :

        # note that we don't validate these strings -- if they contain newlines
        # (and so mess up the transition protocol) then the server will error
        result = ["faultInject"]
        result.append( toNonNoneString(self.options.type))
        result.append( toNonNoneString(self.options.ddlStatement))
        result.append( toNonNoneString(self.options.databaseName))
        result.append( toNonNoneString(self.options.tableName))
        result.append( toNonNoneString(self.options.numOccurrences))
        return '\n'.join(result)


    def isMatchingRole(self, role, segment):
        if role == "primary":
            return segment.role == 'p';
        elif role == "mirror":
            return segment.role == 'm';
        elif role == "primary_mirror":
            return segment.role == 'm' or segment.role == 'p';
        else:
            raise ProgramArgumentValidationException("Invalid role specified: %s" % role)

        pass
    #
    # load the segments and filter to the ones we should target
    #
    def loadTargetSegments(self) :

        targetHost = self.options.targetHost
        targetRole = self.options.targetRole
        targetDbId = self.options.targetDbId

        if targetHost is None and targetDbId is None:
            raise ProgramArgumentValidationException("neither --host nor --seg_dbid specified.  Exactly one should be specified.")
        if targetHost is not None and targetDbId is not None:
            raise ProgramArgumentValidationException("both --host nor --seg_dbid specified.  Exactly one should be specified.")
        if targetHost is not None and targetRole is None:
            raise ProgramArgumentValidationException("--role not specified when --host is specified.  Role is required when targeted a host.")
        if targetDbId is not None and targetRole is not None:
            raise ProgramArgumentValidationException("--role specified when --seg_dbid is specified.  Role should not be specified when targeting a single dbid.")

        #
        # load from master db
        #
        logger.info("Obtaining Segment details from master...")
        dbUrl = dbconn.DbURL(port=self.options.masterPort, dbname='template1')
        conn = dbconn.connect(dbUrl, utility=False)
        rows = dbconn.getConfig(conn)
        gpArray = gparray.GpArray(rows)
        conn.close()

        segments = gpArray.getSegDbList()

        #
        # prune gpArray according to filter settings
        #
        if targetHost is not None:
            segments = [seg for seg in segments if seg.hostname == targetHost]

        if targetDbId is not None:
            segments = [seg for seg in segments if seg.dbid == targetDbId]

        if targetRole is not None:
            segments = [seg for seg in segments if self.isMatchingRole(targetRole, seg)]

        # todo: MILENA only Up segments? (enable following line)
        # segments = [seg for seg in segments if self.status == 'u']

        return segments

    def injectFaults(self, segments, messageText):

        # write the input file
        inputFile = NamedTemporaryFile('w', delete=True)
        inputFile.write(messageText)
        inputFile.flush()

        logger.info("Injecting fault on %d segments", len(segments))

        # run the command in serial to each target
        for segment in segments :
            logger.info("Injecting fault on %s", segment)
            
            # if there is an error then an exception is raised by command execution
            gp.SendFilerepTransitionMessage.local("Fault Injector", inputFile.name, segment.port, segment.hostname)
        pass

    ######
    def run(self):

        messageText = self.buildMessage()
        segments = self.loadTargetSegments()
        self.injectFaults(segments, messageText)
        return 0 # success -- exit code 0!

    def cleanup(self):
        pass

#-------------------------------------------------------------------------
def createParser():
    parser = OptParser(option_class=OptChecker,
                description=' '.join(description.split()),
                version='%prog version $Revision: #1 $')
    parser.setHelp(_help)
    parser.set_usage('%prog [--help] [options] ')
    parser.remove_option('-h')

    addTo = parser
    addTo.add_option('-h', '-?', '--help', action='help',
                      help='show this help message and exit')
    addTo.add_option('-v','--verbose', action='store_true',
                      help='debug output.')
    addTo.add_option('-q', '--quiet', action='store_true', 
                      help='suppress status messages')
    
    # these options are used to determine the target segments
    addTo = OptionGroup(parser, 'Target Segment Options: ')
    parser.add_option_group(addTo)
    addTo.add_option('-r', '--role', dest="targetRole", type='string',
                     help="Role of segments to target: primary, mirror, or primary_mirror")
    addTo.add_option("-s", "--seg_dbid", dest="targetDbId", type="string",
                     help="The segment  dbid on which fault should be set and triggered.")
    addTo.add_option("-H", "--host", dest="targetHost", type="string",
                     help="The hostname on which fault should be set and triggered.")
    addTo.add_option("-p", "--master_port", dest="masterPort",  type="int", default=5432,
                     help="The port number of the master database on localhost, used to fetch the segment configuration.")

    # these options are used to build the message for the segments
    addTo = OptionGroup(parser, 'Fault Options: ')
    parser.add_option_group(addTo)
    addTo.add_option('-y','--type', dest="type", type='string',
                     help='type of fault')
    addTo.add_option("-c", "--ddl_statement", dest="ddlStatement", type="string",
                     help="The DDL statement on which fault should be set and triggered (i.e. create database, drop database, create table, drop table)")
    addTo.add_option("-d", "--database_name", dest="databaseName", type="string",
                     help="The database name on which fault should be set and triggered.")
    addTo.add_option("-t", "--table_name", dest="tableName", type="string",
                     help="The table name on which fault should be set and triggered.")
    addTo.add_option("-o", "--occurrence", dest="numOccurrences", type="int", default=1,
                     help="The number of occurrence of the DDL statement with the database name and the table name before fault is triggered.")

    parser.set_defaults()
    return parser

def createCommand(options):
    return GpFaultInjector(options)


#-------------------------------------------------------------------------

def main():
    simple_main( createParser, createCommand)

#-------------------------------------------------------------------------
logger = gplog.get_default_logger()
if __name__ == '__main__':
    main()
