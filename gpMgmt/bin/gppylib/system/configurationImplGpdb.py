#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
"""
This file defines the interface that can be used to fetch and update system
configuration information.
"""
import os, copy

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.configurationInterface import *
from gppylib.gparray import GpArray, GpDB
from gppylib.db import dbconn

logger = get_default_logger()

class GpConfigurationProviderUsingGpdbCatalog(GpConfigurationProvider) :
    """
    An implementation of GpConfigurationProvider will provide functionality to
    fetch and update gpdb system configuration information (as stored in the
    database)

    Note that the client of this is assuming that the database data is not
    changed by another party between the time segment data is loaded and when it
    is updated
    """

    def __init__(self):
        self.__masterDbUrl = None
        self.__useUtilityMode = False
        pass

    def initializeProvider( self, masterPort, useUtilityMode ) :
        """
        Initialize the provider to get information from the given master db, if
        it chooses to get its data from the database
     
        returns self
        """

        checkNotNone("masterPort", masterPort)
        checkNotNone("useUtilityMode", useUtilityMode)

        dbUrl = dbconn.DbURL(port=masterPort, dbname='template1')

        self.__masterDbUrl = dbUrl
        self.__useUtilityMode = useUtilityMode
        return self

    def loadSystemConfig( self ) :
        """
        Load all segment information from the configuration source.

        Returns a new GpArray object
        """

        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl) 

        logger.info("Obtaining Segment details from master...")

        return GpArray.initFromCatalog(self.__masterDbUrl, self.__useUtilityMode)

    def updateSystemConfig( self, gpArray, textForConfigTable ) :
        """
        Update the configuration for the given segments in the underlying
        configuration store to match the current values
    
        Also resets any dirty bits on saved/updated objects
        """

        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl) 

        conn = dbconn.connect(self.__masterDbUrl, self.__useUtilityMode)
        dbconn.execSQL(conn, "BEGIN")

        dbIdToOriginal = {}
        for seg in gpArray.getSegmentsAsLoadedFromDb():
            dbIdToOriginal[seg.getSegmentDbId()] = seg

        #
        # for now, we simply update all fields.  This could be optimized later
        #   -- to avoid overwriting status if it's unchanged in-memory, for
        #   example
        #
        # It is assumed that external code has paused the fault prober while
        # this runs, as necessary
        #
        for seg in gpArray.getDbList():
            originalSeg = dbIdToOriginal.get(seg.getSegmentDbId())

            if originalSeg is None:

                logger.debug("Inserting dbid %s" % seg.getSegmentDbId())

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
                dbconn.executeUpdateOrInsert(conn, sql, 1)

                # insert into filespaces
                sql = "INSERT INTO pg_catalog.pg_filespace_entry (fsefsoid, fsedbid, fselocation)\n" + \
                      "SELECT fs.oid, " + \
                       self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n" + \
                       self.__toSqlTextValue(seg.getSegmentDataDirectory()) + "\n" + \
                       "FROM pg_catalog.pg_filespace fs WHERE fs.fsname = 'pg_system'"
                dbconn.executeUpdateOrInsert(conn, sql, 1)

                changeDescription = textForConfigTable + ": inserted segment configuration"

            else:

                if seg == originalSeg:
                    changeDescription = None
                else:
                    logger.debug("Updating dbid %s" % seg.getSegmentDbId())

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
                    dbconn.executeUpdateOrInsert(conn, sql, 1)

                    sql = "UPDATE pg_catalog.pg_filespace_entry\n" + \
                            "  SET fselocation = " + self.__toSqlTextValue(seg.getSegmentDataDirectory()) + "\n" \
                            "WHERE fsedbid = " + self.__toSqlIntValue(seg.getSegmentDbId()) + "\n" \
                            "  AND fsefsoid = (SELECT oid FROM pg_catalog.pg_filespace WHERE fsname='pg_system')"
                    dbconn.executeUpdateOrInsert(conn, sql, 1)
                    
                    changeDescription = textForConfigTable + ": updated segment configuration"


            if changeDescription is not None:

                logger.debug("Change description is %s" % changeDescription)

                # now update change history
                sql = "INSERT INTO gp_configuration_history (time, dbid, \"desc\") VALUES(\n" \
                            "now(),\n  " + \
                            self.__toSqlIntValue(seg.getSegmentDbId()) + ",\n  " + \
                            self.__toSqlCharValue(changeDescription) + "\n)"

                dbconn.executeUpdateOrInsert(conn, sql, 1)

        if gpArray.getStrategyAsLoadedFromDb() != gpArray.getFaultStrategy():
            logger.debug("Setting fault strategy to " + gpArray.getFaultStrategy())
            sql = "UPDATE gp_fault_strategy\n" + \
                  "SET fault_strategy = " + self.__toSqlCharValue(gpArray.getFaultStrategy()) + "\n"
            dbconn.executeUpdateOrInsert(conn, sql, 1)

        logger.debug("Committing configuration table changes")
        dbconn.execSQL(conn, "COMMIT")
        conn.close()

        gpArray.setStrategyAsLoadedFromDb( [gpArray.getFaultStrategy()])
        gpArray.setSegmentsAsLoadedFromDb([seg.copy() for seg in gpArray.getDbList()])
        
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
