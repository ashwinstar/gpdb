#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

"""

This file defines the interface that can be used to
   fetch and update system configuration information,
   as well as the data object returned by the

"""
import os

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.interface.configurationInterface import *
from gppylib.db import dbconn
from gppylib.mainUtils import *

logger = get_default_logger()

#
# An implementation of ConfigurationProvider will provide functionality
#   to fetch and update gpdb system configuration information (as stored in the database)
#
#
# Note that the client of this is assuming that the database data is not changed by
#   another party between the time segment data is loaded and when it is updated
#
class ConfigurationProviderUsingGpdbCatalog(ConfigurationProvider) :
    def __init__(self):
        pass

    #
    # Initialize the provider to get information from the given master db, if it chooses to
    #   get its data from the database
    #
    # returns self
    #
    def initializeProvider( self, masterPort, useUtilityMode ) :
        checkNotNone("masterPort", masterPort)
        checkNotNone("useUtilityMode", useUtilityMode)

        dbUrl = dbconn.DbURL(port=masterPort, dbname='template1')

        self.__masterDbUrl = dbUrl
        self.__useUtilityMode = useUtilityMode
        return self

    #
    # Load all segment information from the configuration source.
    #
    # Returns a new ConfigurationForSystem object
    #
    def loadSystemConfig( self ) :
        checkNotNone("masterDbUrl", self.__masterDbUrl) # ensure initialized called

        logger.info("Obtaining Segment details from master...")

        sql = '''
                SELECT dbid, content, role, preferred_role, mode, status, hostname, address, port,
                    fselocation AS datadir, replication_port
                FROM pg_catalog.gp_segment_configuration
                    JOIN pg_catalog.pg_filespace_entry ON (dbid = fsedbid)
                    JOIN pg_catalog.pg_filespace fs ON (fsefsoid = fs.oid AND fsname='pg_system')
                ORDER BY content, preferred_role DESC
            '''

        conn = dbconn.connect(self.__masterDbUrl, self.__useUtilityMode)
        configurationRows = dbconn.execSQL(conn, sql)
        conn.close()

        result = ConfigurationForSystem()
        for row in configurationRows:
            segment = ConfigurationForSegment()

            segment.setSegmentDbId(int(row[0]))
            segment.setSegmentContentId(int(row[1]))
            segment.setSegmentRole(row[2])
            segment.setSegmentPreferredRole(row[3])
            segment.setSegmentMode(row[4])
            segment.setSegmentStatus(row[5])
            segment.setSegmentHostName(row[6])
            segment.setSegmentAddress(row[7])
            segment.setSegmentPort(int(row[8]))
            segment.setSegmentDataDirectory(row[9])
            segment.setSegmentReplicationPort(None if row[10] is None else int(row[10]))

            segment.resetSegmentDirtyBit()
            segment.markSegmentCreatedInDb()

            result.addSegment(segment)

        return result

    #
    # Update the configuration for the given segments in the underlying configuration store
    #   to match the current values
    #
    # Also resets any dirty bits on saved/updated objects
    #
    def updateSystemConfig( self, systemConfiguration ) :
        checkNotNone("masterDbUrl", self.__masterDbUrl) # ensure initialized called

        toAddOrUpdate = [x for x in systemConfiguration.getSegmentList() if x.getSegmentIsDirty()]

        if len(toAddOrUpdate) == 0:
            return

        conn = dbconn.connect(self.__masterDbUrl, self.__useUtilityMode)
        dbconn.execSQL(conn, "BEGIN")

        #
        # for now, we simply update all fields.  This could be optimized later -- to avoid
        #   overwriting status if it's unchanged in-memory, for example
        # Need to figure out the interaction of scripts with fault prober
        #
        for seg in toAddOrUpdate:
            if seg.isCreatedInDb:
                # run an update
                sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
                        "  SET contentid = " + self.__toSqlIntValue(seg.getSegmentContentId()) + ",\n" \
                        "  SET role = " + self.__toSqlCharValue(seg.getSegmentRole()) + ",\n" \
                        "  SET preferred_role = " + self.__toSqlCharValue(seg.getSegmentPreferredRole()) + ",\n" \
                        "  SET mode = " + self.__toSqlCharValue(seg.getSegmentMode()) + ",\n" \
                        "  SET status = " + self.__toSqlCharValue(seg.getSegmentStatus()) + ",\n" \
                        "  SET port = " + self.__toSqlIntValue(seg.getSegmentPort()) + ",\n" \
                        "  SET hostname = " + self.__toSqlTextValue(seg.getSegmentHostName()) + ",\n" \
                        "  SET address = " + self.__toSqlTextValue(seg.getSegmentAddress()) + ",\n" \
                        "  SET replication_port = " + self.__toSqlIntValue(seg.getSegmentReplicationPort()) + ",\n" \
                        "  SET san_mounts = " + self.__toSqlIntValue(seg.getSegmentSanMounts()) + "\n" \
                        "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
                dbconn.execSQL(conn, sql)
                changeDescription = "updated by %s" % getProgramName()

            else:
                # run an insert
                sql = "INSERT INTO pg_catalog.gp_segment_configuration\n" + \
                      "(dbid, content, role, preferred_role, mode, status, port, hostname, address, replication_port, san_mounts)\n" + \
                      "VALUES(\n" + \
                        self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n  " + \
                        self.__toSqlIntValue(seg.getSegmentContentId()) + ",\n  " + \
                        self.__toSqlCharValue(seg.getSegmentRole()) + ",\n  " + \
                        self.__toSqlCharValue(seg.getSegmentPreferredRole()) + ",\n  " + \
                        self.__toSqlCharValue(seg.getSegmentMode()) + ",\n  " + \
                        self.__toSqlCharValue(seg.getSegmentStatus()) + ",\n  " + \
                        self.__toSqlIntValue(seg.getSegmentPort()) + ",\n  " + \
                        self.__toSqlTextValue(seg.getSegmentHostName()) + ",\n  " + \
                        self.__toSqlTextValue(seg.getSegmentAddress()) + ",\n  " + \
                        self.__toSqlIntValue(seg.getSegmentReplicationPort()) + ",\n  " + \
                        self.__toSqlIntValue(seg.getSegmentSanMounts()) + "\n" + \
                      ")"
                dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

                # insert into filespaces
                sql = "INSERT INTO pg_filespace_entry (fsefsoid, fsedbid, fselocation)\n" + \
                      "SELECT fs.oid, " + \
                       self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n" + \
                       self.__toSqlTextValue(seg.getSegmentDataDirectory()) + ",\n" + \
                       "FROM pg_catalog.pg_filespace fs WHERE fs.fsname = 'pg_system'"
                dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

                changeDescription = "updated by %s" % getProgramName()

            # now update change history
            sql = "INSERT INTO gp_configuration_history (time, dbid, desc) VALUES(\n" \
                        "now(),\n  " + \
                        self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n  " + \
                        self.__toSqlCharValue(changeDescription)

            dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

        dbconn.execSQL(conn, "COMMIT")
        conn.close()

        #
        # clear dirty bits in memory
        #
        for seg in toAddOrUpdate:
            seg.markSegmentCreatedInDb()
            seg.resetSegmentDirtyBit()

        return

    def __toSqlIntValue(self, val):
        if val is None:
            return "null"
        return str(val)

    def __toSqlCharValue(self, val):
        return self.__toSqlTextValue(val)

    def __toSqlTextValue(self, val):
        if val is None:
            return "null"
        return "'" + val.replace("'","''").replace('\\','\\\\') + "'"
