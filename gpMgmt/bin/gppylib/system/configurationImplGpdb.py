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

from gppylib.mainUtils import *
from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.configurationInterface import *
from gppylib.gparray import GpArray, GpDB
from gppylib.db import dbconn

logger = get_default_logger()

#
# An implementation of GpConfigurationProvider will provide functionality
#   to fetch and update gpdb system configuration information (as stored in the database)
#
#
# Note that the client of this is assuming that the database data is not changed by
#   another party between the time segment data is loaded and when it is updated
#
class GpConfigurationProviderUsingGpdbCatalog(GpConfigurationProvider) :
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
    # Returns a new GpArray object
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

        origSegments = []
        segments = []
        for row in configurationRows:
            dbId = int(row[0])
            contentId = int(row[1])
            role = row[2]
            preferredRole = row[3]
            mode = row[4]
            status = row[5]
            hostName = row[6]
            address = row[7]
            port = int(row[8])
            dataDirectory = row[9]
            replicationPort = None if row[10] is None else int(row[10])

            segment = GpDB(content=contentId,
                           preferred_role=preferredRole,
                           dbid=dbId,
                           role=role,
                           mode=mode,
                           status=status,
                           hostname=hostName,
                           address=address,
                           port=port,
                           datadir=dataDirectory,
                           replicationPort=replicationPort)

            segments.append(segment)
            origSegments.append(segment.copy())

        result = GpArray(segments, origSegments)

        return result

    #
    # Update the configuration for the given segments in the underlying configuration store
    #   to match the current values
    #
    # Also resets any dirty bits on saved/updated objects
    #
    def updateSystemConfig( self, gpArray ) :
        checkNotNone("masterDbUrl", self.__masterDbUrl) # ensure initialized called

        conn = dbconn.connect(self.__masterDbUrl, self.__useUtilityMode)
        dbconn.execSQL(conn, "BEGIN")

        dbIdToOriginal = {}
        for seg in gpArray.getSegmentsAsLoadedFromDb():
            dbIdToOriginal[seg.getSegmentDbId()] = seg

        #
        # for now, we simply update all fields.  This could be optimized later -- to avoid
        #   overwriting status if it's unchanged in-memory, for example
        # Need to figure out the interaction of scripts with fault prober
        #
        for seg in gpArray.getDbList():
            originalSeg = dbIdToOriginal.get(seg.getSegmentDbId())

            if originalSeg is None:

                logger.info("Inserting dbid %s" % seg.getSegmentDbId())

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
                        "null\n" + \
                      ")"
                dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

                # insert into filespaces
                sql = "INSERT INTO pg_filespace_entry (fsefsoid, fsedbid, fselocation)\n" + \
                      "SELECT fs.oid, " + \
                       self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n" + \
                       self.__toSqlTextValue(seg.getSegmentDataDirectory()) + ",\n" + \
                       "FROM pg_catalog.pg_filespace fs WHERE fs.fsname = 'pg_system'"
                dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

                changeDescription = "inserted by %s" % "todo:program" # getProgramName()

            else:

                if seg == originalSeg:
                    logger.info("NOT updating dbid %s" % seg.getSegmentDbId())
                    changeDescription = None
                else:
                    logger.info("Updating dbid %s" % seg.getSegmentDbId())
                    # run an update
                    sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
                            "  SET content = " + self.__toSqlIntValue(seg.getSegmentContentId()) + ",\n" \
                            "  role = " + self.__toSqlCharValue(seg.getSegmentRole()) + ",\n" \
                            "  preferred_role = " + self.__toSqlCharValue(seg.getSegmentPreferredRole()) + ",\n" \
                            "  mode = " + self.__toSqlCharValue(seg.getSegmentMode()) + ",\n" \
                            "  status = " + self.__toSqlCharValue(seg.getSegmentStatus()) + ",\n" \
                            "  port = " + self.__toSqlIntValue(seg.getSegmentPort()) + ",\n" \
                            "  hostname = " + self.__toSqlTextValue(seg.getSegmentHostName()) + ",\n" \
                            "  address = " + self.__toSqlTextValue(seg.getSegmentAddress()) + ",\n" \
                            "  replication_port = " + self.__toSqlIntValue(seg.getSegmentReplicationPort()) + "\n" \
                            "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
                    dbconn.execSQL(conn, sql)

                    # todo MIRRORING: update filespace information

                    changeDescription = "updated by %s" % "todo:program" # getProgramName()


            if changeDescription is not None:
                # now update change history
                sql = "INSERT INTO gp_configuration_history (time, dbid, \"desc\") VALUES(\n" \
                            "now(),\n  " + \
                            self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n  " + \
                            self.__toSqlCharValue(changeDescription) + "\n)"

                dbconn.execSQL(conn, sql) # todo: assert that 1 was inserted

        dbconn.execSQL(conn, "COMMIT")
        conn.close()

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
