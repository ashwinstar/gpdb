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
from gppylib.gparray import GpArray, GpDB, InvalidSegmentConfiguration
from gppylib import gparray
from gppylib.db import dbconn
from gppylib.commands.gp import get_local_db_mode

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
        pass

    def initializeProvider( self, masterPort ) :
        """
        Initialize the provider to get information from the given master db, if
        it chooses to get its data from the database
     
        returns self
        """

        checkNotNone("masterPort", masterPort)

        dbUrl = dbconn.DbURL(port=masterPort, dbname='template1')

        self.__masterDbUrl = dbUrl
        return self

    def loadSystemConfig( self, useUtilityMode ) :
        """
        Load all segment information from the configuration source.

        Returns a new GpArray object
        """

        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl) 

        logger.info("Obtaining Segment details from master...")

        array = GpArray.initFromCatalog(self.__masterDbUrl, useUtilityMode)
        
        if get_local_db_mode(array.master.getSegmentDataDirectory()) != 'UTILITY':
            logger.debug("Validating configuration...")
            if not array.is_array_valid():
                raise InvalidSegmentConfiguration(array)
            
        return array

    def sendPgElogFromMaster( self, msg, sendAlerts):
        """
        Send a message from the master database using select pg_elog ...
        """
        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl)

        conn = None
        try:
            conn = dbconn.connect(self.__masterDbUrl, utility=True)
            dbconn.execSQL(conn, "SELECT GP_ELOG(" +
                        self.__toSqlCharValue(msg) + "," +
                        ("true" if sendAlerts else "false") + ")")
        finally:
            if conn:
                conn.close()


    def updateSystemConfig( self, gpArray, textForConfigTable, dbIdToForceMirrorRemoveAdd, useUtilityMode ) :
        """
        Update the configuration for the given segments in the underlying
        configuration store to match the current values
    
        Also resets any dirty bits on saved/updated objects

        @param textForConfigTable label to be used when adding to segment configuration history
        @param dbIdToForceMirrorRemoveAdd a map of dbid -> True for mirrors for which we should force
                                          updating the mirror
        """

        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl) 

        logger.debug("Validating configuration changes...")

        if not gpArray.is_array_valid():
            logger.critical("Configuration is invalid")
            raise InvalidSegmentConfiguration(gpArray)

        conn = dbconn.connect(self.__masterDbUrl, useUtilityMode)
        dbconn.execSQL(conn, "BEGIN")

        dbIdToOriginal = {}
        for seg in gpArray.getSegmentsAsLoadedFromDb():
            dbIdToOriginal[seg.getSegmentDbId()] = seg

        #
        # for now, we simply update all fields.  This could be optimized later
        #   -- to avoid overwriting status if it's unchanged in-memory, for
        #   example
        #
        # Pausing of the prober is no longer needed as it is impossible to save
        # an invalid state of a primary/mirror pair.
        #
        for seg in gpArray.getDbList():
            originalSeg = dbIdToOriginal.get(seg.getSegmentDbId())

            if originalSeg is None:

                logger.debug("Inserting dbid %s" % seg.getSegmentDbId())

                if seg.getSegmentPreferredRole() != gparray.ROLE_MIRROR:
                    raise Exception("Internal error: Can only add new mirrors")
                if seg.getSegmentRole() != gparray.ROLE_MIRROR:
                    raise Exception("Internal error: Can only add new mirrors")

                dbId = self.__callSegmentAddMirror(conn, gpArray, seg)
                seg.setSegmentDbId(dbId)

                # now update mode/status since this is not done by gp_add_segment_mirror
                self.__updateSegmentModeStatus(conn, seg)

                self.__insertConfigHistory(conn, seg.getSegmentDbId(),
                                           textForConfigTable + ": inserted segment configuration")

            else:

                if seg == originalSeg:
                    #
                    # No need to update
                    #
                    pass
                elif not seg.getSegmentDbId() in dbIdToForceMirrorRemoveAdd and \
                     self.__equalIgnoringModeAndStatusAndReplicationPort(seg, originalSeg):
                    #
                    # Just update mode and status
                    #
                    logger.debug("Updating mode and status for dbid %s" % seg.getSegmentDbId())

                    self.__updateSegmentModeStatus(conn, seg)
                    if seg.getSegmentReplicationPort() != originalSeg.getSegmentReplicationPort():
                        # when adding a mirror, the replication port may change as well
                        self.__updateSegmentReplicationPort(conn, seg)
                        self.__insertConfigHistory(conn, seg.getSegmentDbId(),
                                               textForConfigTable + ": updated segment mode, status, and replication port")
                    else:
                        self.__insertConfigHistory(conn, seg.getSegmentDbId(),
                                               textForConfigTable + ": updated segment mode and status")
                else:
                    #
                    # filespace and other core has changed, or it's a mirror and we are recovering full
                    #     (in which case we want to call removeMirror and addMirror so we mark
                    #      the primary as full-resyncing)
                    #
                    if seg.isSegmentMaster(current_role=True) or seg.isSegmentStandby(current_role=True):
                        #
                        # Assertion here -- user should not be allowed to change master/standby info.
                        #
                        raise Exception("Internal error: Can only change core details of segments, not masters" \
                                    " (on segment %s) (seg %s vs original %s)" %
                                    (seg.getSegmentDbId(), repr(seg), repr(originalSeg)))

                    if not seg.isSegmentMirror(current_role=True):
                        #
                        # Assertion here -- user should not be allowed to change primary info.
                        #
                        raise Exception("Internal error: Can only change core details of mirrors, not primaries" \
                                        " (on segment %s) (seg %s vs original %s)" %
                                        (seg.getSegmentDbId(), repr(seg), repr(originalSeg)))

                    if useUtilityMode:
                        raise Exception("Cannot change core details of mirrors in utility mode")

                    origDbId = seg.getSegmentDbId()

                    self.__callSegmentRemoveMirror(conn, seg)

                    dbId = self.__callSegmentAddMirror(conn, gpArray, seg)
                    seg.setSegmentDbId(dbId)

                    # now update mode/status since this is not done by gp_add_segment_mirror
                    self.__updateSegmentModeStatus(conn, seg)

                    self.__insertConfigHistory(conn, seg.getSegmentDbId(),
                                "%s: inserted segment configuration for full recovery "
                                "for original dbid %s" %
                                (textForConfigTable, origDbId))

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

    def __equalIgnoringModeAndStatusAndReplicationPort(self, firstSeg, secondSeg):

        firstMode = firstSeg.getSegmentMode()
        firstStatus = firstSeg.getSegmentStatus()
        firstReplicationPort = firstSeg.getSegmentReplicationPort()
        try:

            # make the elements we don't want to compare match and see if they are then equal
            firstSeg.setSegmentMode(secondSeg.getSegmentMode())
            firstSeg.setSegmentStatus(secondSeg.getSegmentStatus())
            firstSeg.setSegmentReplicationPort(secondSeg.getSegmentReplicationPort())

            return firstSeg == secondSeg
        finally:
            #
            # restore mode and status after comaprison 
            #
            firstSeg.setSegmentMode(firstMode)
            firstSeg.setSegmentStatus(firstStatus)
            firstSeg.setSegmentReplicationPort(firstReplicationPort)

    def __getSegmentUpdateSql(self, seg):
        # run an update
        return "UPDATE pg_catalog.gp_segment_configuration\n" + \
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

    def __callSegmentRemoveMirror(self, conn, seg):
        sql = "SELECT gp_remove_segment_mirror(%s::int2)" % (self.__toSqlIntValue(seg.getSegmentContentId()))
        result = self.__fetchSingleOutputRow(conn, sql)
        assert result[0] # must return True

    def __callSegmentAddMirror(self, conn, gpArray, seg):

        filespaceArrayString = []
        for fs in gpArray.getFilespaces():
            path = seg.getSegmentFilespaces()[fs.getOid()]
            filespaceArrayString.append("{%s,%s}" % \
                        (self.__toSqlArrayStringValue(fs.getName()), \
                         self.__toSqlArrayStringValue(path)))

        filespaceMapStr = "{" + ",".join(filespaceArrayString) + "}"

        sql = "SELECT gp_add_segment_mirror(%s::int2, %s, %s, %s, %s, %s)" \
                    % \
                    (
                        self.__toSqlIntValue(seg.getSegmentContentId()),
                        self.__toSqlTextValue(seg.getSegmentHostName()),
                        self.__toSqlTextValue(seg.getSegmentAddress()),
                        self.__toSqlIntValue(seg.getSegmentPort()),
                        self.__toSqlIntValue(seg.getSegmentReplicationPort()),
                        self.__toSqlTextValue(filespaceMapStr)
                    )

        sqlResult = self.__fetchSingleOutputRow(conn, sql)
        dbId = int(sqlResult[0])
        return dbId

    def __updateSegmentReplicationPort(self, conn, seg):
        # run an update
        sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
            "  SET\n" + \
            "  replication_port = " + self.__toSqlIntValue(seg.getSegmentReplicationPort()) + "\n" \
            "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
        dbconn.executeUpdateOrInsert(conn, sql, 1)


    def __updateSegmentModeStatus(self, conn, seg):
        # run an update
        sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
            "  SET\n" + \
            "  mode = " + self.__toSqlCharValue(seg.getSegmentMode()) + ",\n" \
            "  status = " + self.__toSqlCharValue(seg.getSegmentStatus()) + "\n" \
            "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
        dbconn.executeUpdateOrInsert(conn, sql, 1)

    def __fetchSingleOutputRow(self, conn, sql):
        cursor = dbconn.execSQL(conn, sql)
        if cursor.rowcount != 1:
            raise Exception("SQL did not return expected row:\n%s" % (sql))
        res = None
        for row in cursor:
            assert res is None
            res = row
            assert res is not None
        cursor.close()
        return res

    def __insertConfigHistory(self, conn, dbId, msg ):
        logger.debug("Change description is %s" % msg)

        # now update change history
        sql = "INSERT INTO gp_configuration_history (time, dbid, \"desc\") VALUES(\n" \
                    "now(),\n  " + \
                    self.__toSqlIntValue(dbId) + ",\n  " + \
                    self.__toSqlCharValue(msg) + "\n)"
        dbconn.executeUpdateOrInsert(conn, sql, 1)


    def __toSqlIntValue(self, val):
        if val is None:
            return "null"
        return str(val)

    def __toSqlArrayStringValue(self, val):
        if val is None:
            return "null"
        return '"' + val.replace('"','\\"').replace('\\','\\\\') + '"'

    def __toSqlCharValue(self, val):
        return self.__toSqlTextValue(val)

    def __toSqlTextValue(self, val):
        if val is None:
            return "null"
        return "'" + val.replace("'","''").replace('\\','\\\\') + "'"
