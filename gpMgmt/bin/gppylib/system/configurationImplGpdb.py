#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
# Copyright (c) EMC/Greenplum Inc 2011. All Rights Reserved.
#
"""
This file defines the interface that can be used to fetch and update system
configuration information.
"""
import os, copy

from gppylib.gplog import *
from gppylib.utils import checkNotNone
from gppylib.system.configurationInterface import *
from gppylib.system.ComputeCatalogUpdate import ComputeCatalogUpdate
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


    def updateSystemConfig( self, gpArray, textForConfigTable, dbIdToForceMirrorRemoveAdd, useUtilityMode, allowPrimary) :
        """
        Update the configuration for the given segments in the underlying
        configuration store to match the current values
    
        Also resets any dirty bits on saved/updated objects

        @param textForConfigTable label to be used when adding to segment configuration history
        @param dbIdToForceMirrorRemoveAdd a map of dbid -> True for mirrors for which we should force updating the mirror
        @param useUtilityMode True if the operations we're doing are expected to run via utility moed
        @param allowPrimary True if caller authorizes add/remove primary operations (e.g. gpexpand)
        """

        # ensure initializeProvider() was called
        checkNotNone("masterDbUrl", self.__masterDbUrl) 

        logger.debug("Validating configuration changes...")

        if not gpArray.is_array_valid():
            logger.critical("Configuration is invalid")
            raise InvalidSegmentConfiguration(gpArray)

        conn = dbconn.connect(self.__masterDbUrl, useUtilityMode)
        dbconn.execSQL(conn, "BEGIN")

        # compute what needs to be updated
        update = ComputeCatalogUpdate(gpArray, dbIdToForceMirrorRemoveAdd, useUtilityMode, allowPrimary)
        update.validate()

        # remove mirror segments (e.g. for gpexpand rollback)
        for seg in update.mirror_to_remove:
            self.__updateSystemConfigRemoveMirror(conn, seg, textForConfigTable)

        # remove primary segments (e.g for gpexpand rollback)
        for seg in update.primary_to_remove:
            self.__updateSystemConfigRemovePrimary(conn, seg, textForConfigTable)

        # add new primary segments
        for seg in update.primary_to_add:
            self.__updateSystemConfigAddPrimary(conn, gpArray, seg, textForConfigTable)

        # add new mirror segments
        for seg in update.mirror_to_add:
            self.__updateSystemConfigAddMirror(conn, gpArray, seg, textForConfigTable)

        # remove and add mirror segments necessitated by catalog attribute update
        for seg in update.mirror_to_remove_and_add:
            self.__updateSystemConfigRemoveAddMirror(conn, gpArray, seg, textForConfigTable)

        # apply updates to existing segments
        for seg in update.segment_to_update:
            originalSeg = update.dbsegmap.get(seg.getSegmentDbId())
            self.__updateSystemConfigUpdateSegment(conn, gpArray, seg, originalSeg, textForConfigTable)

        # apply update to fault strategy
        if gpArray.getStrategyAsLoadedFromDb() != gpArray.getFaultStrategy():
            self.__updateSystemConfigFaultStrategy(conn, gpArray)

        # commit changes
        logger.debug("Committing configuration table changes")
        dbconn.execSQL(conn, "COMMIT")
        conn.close()

        gpArray.setStrategyAsLoadedFromDb( [gpArray.getFaultStrategy()])
        gpArray.setSegmentsAsLoadedFromDb([seg.copy() for seg in gpArray.getDbList()])


    def __updateSystemConfigRemoveMirror(self, conn, seg, textForConfigTable):
        """
        Remove a mirror segment currently in gp_segment_configuration 
        but not present in the goal configuration and record our action 
        in gp_configuration_history.
        """
        dbId   = seg.getSegmentDbId()
        self.__callSegmentRemoveMirror(conn, seg)
        self.__insertConfigHistory(conn, dbId, "%s: removed mirror segment configuration" % textForConfigTable)


    def __updateSystemConfigRemovePrimary(self, conn, seg, textForConfigTable):
        """
        Remove a primary segment currently in gp_segment_configuration 
        but not present in the goal configuration and record our action 
        in gp_configuration_history.
        """
        dbId = seg.getSegmentDbId()
        self.__callSegmentRemove(conn, seg)
        self.__insertConfigHistory(conn, dbId, "%s: removed primary segment configuration" % textForConfigTable)


    def __updateSystemConfigAddPrimary(self, conn, gpArray, seg, textForConfigTable):
        """
        Add a primary segment specified in our goal configuration but
        which is missing from the current gp_segment_configuration table
        and record our action in gp_configuration_history.
        """
        (dbId, contentId) = self.__callSegmentAdd(conn, gpArray, seg)
        seg.setSegmentDbId(dbId)

        # Set the new content id for the primary as well as any mirror.
        pair = gpArray.getExpansionSegmentContainingDb(seg)
        for db in pair.get_dbs():
            db.setSegmentContentId(contentId)

        self.__updateSegmentModeStatus(conn, seg)
        self.__insertConfigHistory(conn, dbId, "%s: inserted primary segment configuration with contentid %s" % (textForConfigTable, contentId))

    def __updateSystemConfigAddMirror(self, conn, gpArray, seg, textForConfigTable):
        """
        Add a mirror segment specified in our goal configuration but
        which is missing from the current gp_segment_configuration table
        and record our action in gp_configuration_history.
        """
        dbId = self.__callSegmentAddMirror(conn, gpArray, seg)
        seg.setSegmentDbId(dbId)
        self.__updateSegmentModeStatus(conn, seg)
        self.__insertConfigHistory(conn, dbId, "%s: inserted mirror segment configuration" % textForConfigTable)


    def __updateSystemConfigRemoveAddMirror(self, conn, gpArray, seg, textForConfigTable):
        """
        We've been asked to update the mirror in a manner that require
        it to be removed and then re-added.   Perform the tasks
        and record our action in gp_configuration_history.
        """
        origDbId = seg.getSegmentDbId()
        self.__callSegmentRemoveMirror(conn, seg)

        dbId = self.__callSegmentAddMirror(conn, gpArray, seg)
        seg.setSegmentDbId(dbId)

        # now update mode/status since this is not done by gp_add_segment_mirror
        self.__updateSegmentModeStatus(conn, seg)
        self.__insertConfigHistory(conn, seg.getSegmentDbId(), 
                                   "%s: inserted segment configuration for full recovery or original dbid %s" \
                                   % (textForConfigTable, origDbId))


    def __updateSystemConfigUpdateSegment(self, conn, gpArray, seg, originalSeg, textForConfigTable):

        # update mode and status
        # when adding a mirror, the replication port may change as well
        #
        what = "%s: segment mode and status"
        self.__updateSegmentModeStatus(conn, seg)

        if seg.getSegmentReplicationPort() != originalSeg.getSegmentReplicationPort():
            what = "%s: segment mode, status, and replication port"
            self.__updateSegmentReplicationPort(conn, seg)

        self.__insertConfigHistory(conn, seg.getSegmentDbId(), what % textForConfigTable)


    def __updateSystemConfigFaultStrategy(self, conn, gpArray):
        """
        Update the fault strategy.
        """
        fs  = gpArray.getFaultStrategy()
        sql = "UPDATE gp_fault_strategy\n SET fault_strategy = " + self.__toSqlCharValue(fs) + "\n"
        logger.debug(sql)
        dbconn.executeUpdateOrInsert(conn, sql, 1)


    def __callSegmentRemoveMirror(self, conn, seg):
        """
        Call gp_remove_segment_mirror() to remove the mirror.
        """
        sql = "SELECT gp_remove_segment_mirror(%s::int2)" % (self.__toSqlIntValue(seg.getSegmentContentId()))
        logger.debug(sql)
        result = self.__fetchSingleOutputRow(conn, sql)
        assert result[0] # must return True


    def __callSegmentRemove(self, conn, seg):
        """
        Call gp_remove_segment() to remove the primary.
        """
        sql = "SELECT gp_remove_segment(%s::int2)" % (self.__toSqlIntValue(seg.getSegmentDbId()))
        logger.debug(sql)
        result = self.__fetchSingleOutputRow(conn, sql)
        assert result[0]


    def __callSegmentAdd(self, conn, gpArray, seg):
        """
        Call gp_add_segment() to add the primary.
        Select the contentId of the newly added segment.
        Return a tuple containing the new segment's dbid and contentId.
        """
        filespaceMapStr = self.__toSqlFilespaceMapStr(gpArray, seg)

        sql = "SELECT gp_add_segment(%s, %s, %s, %s)" \
            % (
                self.__toSqlTextValue(seg.getSegmentHostName()), 
                self.__toSqlTextValue(seg.getSegmentAddress()), 
                self.__toSqlIntValue(seg.getSegmentPort()), 
                self.__toSqlTextValue(filespaceMapStr)
              )
        logger.debug(sql)

        sqlResult = self.__fetchSingleOutputRow(conn, sql)
        dbId = int(sqlResult[0])

        # Get the new segment's content id.
        sql = "select content from pg_catalog.gp_segment_configuration where dbId = %s" % self.__toSqlIntValue(seg.getSegmentDbId())
        logger.debug(sql)

        sqlResult = self.__fetchSingleOutputRow(conn, sql)
        contentId = int(sqlResult[0])

        # We must also set the primary's replication port, as gp_add_segment() doesn't do it
        sql = "update pg_catalog.gp_segment_configuration set replication_port = %s where dbid = %s" \
            % (
                self.__toSqlIntValue( seg.getSegmentReplicationPort() ), 
                self.__toSqlIntValue( dbId )
              )
        logger.debug(sql)
        dbconn.executeUpdateOrInsert(conn, sql, 1)
        return (dbId, contentId)


    def __callSegmentAddMirror(self, conn, gpArray, seg):
        """
        Call gp_add_segment_mirror() to add the mirror.
        Return the new segment's dbid.
        """
        filespaceMapStr = self.__toSqlFilespaceMapStr(gpArray, seg)

        sql = "SELECT gp_add_segment_mirror(%s::int2, %s, %s, %s, %s, %s)" \
            % (
                self.__toSqlIntValue(seg.getSegmentContentId()),
                self.__toSqlTextValue(seg.getSegmentHostName()),
                self.__toSqlTextValue(seg.getSegmentAddress()),
                self.__toSqlIntValue(seg.getSegmentPort()),
                self.__toSqlIntValue(seg.getSegmentReplicationPort()),
                self.__toSqlTextValue(filespaceMapStr)
              )

        logger.debug(sql)
        sqlResult = self.__fetchSingleOutputRow(conn, sql)
        dbId = int(sqlResult[0])
        return dbId


    def __updateSegmentReplicationPort(self, conn, seg):
        # run an update
        sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
            "  SET\n" + \
            "  replication_port = " + self.__toSqlIntValue(seg.getSegmentReplicationPort()) + "\n" \
            "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
        logger.debug(sql)
        dbconn.executeUpdateOrInsert(conn, sql, 1)


    def __updateSegmentModeStatus(self, conn, seg):
        # run an update
        sql = "UPDATE pg_catalog.gp_segment_configuration\n" + \
            "  SET\n" + \
            "  mode = " + self.__toSqlCharValue(seg.getSegmentMode()) + ",\n" \
            "  status = " + self.__toSqlCharValue(seg.getSegmentStatus()) + "\n" \
            "WHERE dbid = " + self.__toSqlIntValue(seg.getSegmentDbId())
        logger.debug(sql)
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
        # now update change history
        sql = "INSERT INTO gp_configuration_history (time, dbid, \"desc\") VALUES(\n" \
                    "now(),\n  " + \
                    self.__toSqlIntValue(dbId) + ",\n  " + \
                    self.__toSqlCharValue(msg) + "\n)"
        logger.debug(sql)
        dbconn.executeUpdateOrInsert(conn, sql, 1)


    def __toSqlFilespaceMapStr(self, gpArray, seg):
        """
        Return a string representation of the filespace map suitable
        for inclusion into the call to gp_add_segment_mirror().
        """
        filespaceArrayString = []
        for fs in gpArray.getFilespaces():
            path = seg.getSegmentFilespaces()[ fs.getOid() ]
            filespaceArrayString.append("{%s,%s}" % \
                        (self.__toSqlArrayStringValue(fs.getName()), \
                         self.__toSqlArrayStringValue(path)))

        filespaceMapStr = "{" + ",".join(filespaceArrayString) + "}"
        return filespaceMapStr

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
