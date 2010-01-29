#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

import copy
import datetime
import os
import sys
import socket
import signal
import traceback
import pprint
import threading
from time import strftime, sleep

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
from subprocess import Popen, PIPE

try:
    from gppylib.commands.unix import *
    from gppylib.commands.gp import *
    from gppylib.commands.pg import PgControlData
    from gppylib.gparray import GpArray, get_host_interface
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.gplog import *
    from gppylib.db import dbconn
    from gppylib.db import catalog
    from gppylib.segcopy import GpSegCopy, SegcopyError
    from gppylib.userinput import *
    from pygresql.pgdb import DatabaseError
    from pygresql import pg
    import figleaf
except ImportError, e:
    sys.exit('ERROR: Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e))

#
# Constants
#

EXECNAME = os.path.split(__file__)[-1]

description = ("""Upgrades 3.3.x.x mirrors to 3.4 mirrors.""")

_help  = [""" TODO add help """]

_usage = """ TODO add usage """


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class gpconfig_row:

    def __init__(self, row):
        self.content        = row[0]
        self.definedprimary = row[1]
        self.dbid           = row[2]
        self.isprimary      = row[3]
        self.valid          = row[4]
        self.hostname       = row[5]
        self.port           = row[6]
        self.datadir        = row[7]

    def __str__(self):
        
        tempStr = '--- gpconfig_row ---' \
        + '\n      content        = ' + str(self.content) \
        + '\n      definedprimary = ' + str(self.definedprimary) \
        + '\n      dbid           = ' + str(self.dbid) \
        + '\n      isprimary      = ' + str(self.isprimary) \
        + '\n      valid          = ' + str(self.valid) \
        + '\n      hostname       = ' + str(self.hostname) \
        + '\n      port           = ' + str(self.port) \
        + '\n      datadir        = ' + str(self.datadir)
        return tempStr
    
    @staticmethod
    def query():
        return '''SELECT    content, definedprimary, dbid, isprimary,
                              valid  , hostname      , port, datadir
                    FROM      pg_catalog.gp_configuration
                    WHERE     content > -1
                    ORDER BY content, definedprimary DESC '''


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class mirror_hosts_row:
    
    def __init__(self, row):
        self.hostname = row[0]

    @staticmethod
    def query():
        return '''SELECT  DISTINCT hostname 
                        FROM             pg_catalog.gp_configuration
                        WHERE            isprimary = 'f'
                        ORDER BY         hostname '''



#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class primary_mirror_hosts_row:

    def __init__(self, row):
        self.primary_host = row[0]
        self.mirror_host  = row[1]
        
    @staticmethod
    def query():
        return '''SELECT t1.hostname as primary_host, t2.hostname as mirror_host 
                                FROM   pg_catalog.gp_configuration as t1 
                                        INNER JOIN  
                                       pg_catalog.gp_configuration as t2 
                                ON     t1.content = t2.content 
                                   AND t1.content >= 0 
                                   AND t1.isprimary = 't' 
                                   AND t2.isprimary = 'f' 
                                ORDER BY  mirror_host '''


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class mirror_host_info_row:
    
    def __init__(self, row):
        self.primary_host = row[0]
        self.primary_data_directory = row[1]
        self.mirror_data_directory = row[2]
        self.dbid = row[3]

    def __str__(self):
        tempStr = '--- mirror_host_info_row ---' \
        +         '\n    primary_host           = ' + str(self.primary_host) \
        +         '\n    primary_data_directory = ' + str(self.primary_data_directory) \
        +         '\n    mirror_data_directory  = ' + str(self.mirror_data_directory) \
        +         '\n    dbid                   = ' + str(self.dbid)
        return tempStr

    @staticmethod
    def query():        
        return '''SELECT t1.hostname as primary_host
                                     , t1.datadir  as primary_data_directory
                                     , t2.datadir  as mirror_data_directory
                                     , t2.dbid     as dbid
                                FROM   pg_catalog.gp_configuration as t1 
                                        INNER JOIN  
                                       pg_catalog.gp_configuration as t2 
                                ON     t1.content = t2.content 
                                   AND t2.hostname = '%s'
                                   AND t1.content >= 0 
                                   AND t1.isprimary = 't' 
                                   AND t2.isprimary = 'f' 
                                ORDER BY  primary_host '''


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class database_row:
    
    def __init__(self, row):
        self.databaseDirectory = row[0]
        self.databaseName = row[1]

    @staticmethod
    def query():
        return '''SELECT oid, datname
                  FROM pg_database
                  ORDER BY oid '''

#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class non_catalog_toast_tables_row:
    
    def __init__(self, row):
        self.filename = row[0]

    @staticmethod
    def query():
        return '''SELECT relfilenode
                  FROM pg_class
                  WHERE relnamespace  =  11
                     AND relisshared   =  'f'
                     AND (relkind  = 'r' OR relkind = 'i')
                     AND relstorage    =  'h'
                     AND reltoastrelid = 0
                  ORDER BY relfilenode '''


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class catalog_toast_tables_row:
    
    def __init__(self, row):
        self.filename = row[0]
        self.toastfilename = row[1]
        
    @staticmethod
    def query():
        return '''SELECT t1.relfilenode, t2.relfilenode as toast_filenode
                  FROM pg_class as t1, pg_class t2
                  WHERE t1.relnamespace  = 11
                    AND t1.relisshared   = 'f'
                    AND t1.reltoastrelid = t2.oid
                  ORDER BY t1.relfilenode '''

#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------

class InvalidStatusError(Exception): pass

#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class ValidationError(Exception): pass

#-------------------------------------------------------------------------------
                                                                
def parseargs():
    parser = OptParser( option_class = OptChecker
                      , description  = ' '.join(description.split())
                      , version      = '%prog version $Revision: #12 $'
                      )
    parser.setHelp(_help)
    parser.set_usage('%prog ' + _usage)
    parser.remove_option('-h')

    parser.add_option('-r', '--rollback', action='store_true',
                      help='rollback failed expansion setup.')
    parser.add_option('-S', '--safe-mode', action='store_true',
                      help='safe mode for upgrade of mirrors.')
    parser.add_option('-U', '--unsafe-mode', action='store_true',
                      help='unsafe mode for upgrade of mirrors.')
    parser.add_option('-v','--verbose', action='store_true',
                      help='debug output.')
    parser.add_option('-h', '-?', '--help', action='help',
                        help='show this help message and exit.')
    parser.add_option('--usage', action="briefhelp")

    parser.set_defaults( verbose = False
                       , filters = []
                       , slice   = (None, None)
                       )
    #
    # Parse the command line arguments
    #
    (options, args) = parser.parse_args()

    if len(args) > 0:
        logger.error('Unknown argument %s' % args[0])
        parser.exit()

    try:
        options.master_data_directory = get_masterdatadir()
        options.gphome = get_gphome()
    except GpError, msg:
        logger.error(msg)
        parser.exit()

    if not os.path.exists(options.master_data_directory):
         logger.error('Master data directory does not exist.')
         parser.exit()

    return options, args

#-------------------------------------------------------------------------------
def sig_handler(sig, arg):
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGHUP, signal.SIG_DFL)

    # raise sig
    os.kill(os.getpid(), sig)


#-------------------------------------------------------------------------------
def create_pid_file(master_data_directory):
    """Creates gpupgradmirror pid file"""
    try:
        fp = open(master_data_directory + '/gpupgrademirror.pid', 'w')
        fp.write(str(os.getpid()))
    except IOError:
        raise
    finally:
        if fp: fp.close()


#-------------------------------------------------------------------------------
def gpupgrademirror_status_file_exists(master_data_directory):
    """Checks if gpupgrademirror.pid exists"""
    return os.path.exists(master_data_directory + '/gpupgrademirror.status')


#-------------------------------------------------------------------------------
def remove_pid_file(master_data_directory):
    """Removes upgrademirror pid file"""
    try:
        os.unlink(master_data_directory + '/gpupgrademirror.pid')
    except:
        pass


#-------------------------------------------------------------------------------
def is_gpupgrademirror_running(master_data_directory):
    """Checks if there is another instance of gpupgrademirror running"""
    is_running = False
    try:
        fp = open(master_data_directory + '/gpupgrademirror.pid', 'r')
        pid = int(fp.readline().strip())
        fp.close()
        is_running = check_pid(pid)
    except IOError:
        pass
    except Exception, msg:
        raise

    return is_running


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class GpUpgradeMirrorStatus():
    """Class that manages gpupgrademirror status file.

    The status file is placed in the master data directory on both the master and
    the standby master.  it's used to keep track of where we are in the progression.
    """
    def __init__(self, logger, master_data_directory, segment_dbid, master_mirror):
        self.logger=logger

        self._status_values = { 'UNINITIALIZED'               : 1
                              , 'START_PRIMARY_COPY'          : 2
                              , 'END_PRIMARY_COPY'            : 3
                              , 'START_NEW_MIRROR_FILE_FIXUP' : 4 
                              , 'END_NEW_MIRROR_FILE_FIXUP'   : 5
                              , 'START_MOVE_OLD_MIRROR'       : 6
                              , 'END_MOVE_OLD_MIRROR'         : 7
                              , 'START_MOVE_NEW_MIRROR'       : 8
                              , 'END_MOVE_NEW_MIRROR'         : 9
                              , 'START_REMOVE_OLD_MIRROR'     : 10
                              , 'END_REMOVE_OLD_MIRROR'       : 11
                              , 'GPUPGRADEMIRROR_DONE'        : 12
                              }
        self._status = []
        self._status_info = []
        self._master_data_directory = master_data_directory
        self._master_mirror = master_mirror
        self.segment_dbid = segment_dbid
        self._status_filename = master_data_directory + '/gpupgrademirror' + str(segment_dbid) + '.status'
        self._status_standby_filename = master_data_directory + '/gpupgrademirror.standby.status'
        self._fp = None
        self._fp_standby = None
        self._temp_dir = None
        self._input_filename = None
        self._original_primary_count = None
        self._gp_configuration_backup = None

        if os.path.exists(self._status_filename):
            self._read_status_file()

    def _read_status_file(self):
        """Reads in an existing gpupgrademirror status file"""
        self.logger.debug("Trying to read in a pre-existing gpupgrademirror status file")
        try:
            self._fp = open(self._status_filename, 'a+')
            self._fp.seek(0)

            for line in self._fp:
                (status, status_info) = line.rstrip().split(':')
                if status == 'BUILD_SEGMENT_TEMPLATE_STARTED':
                    self._temp_dir = status_info
                elif status == 'BUILD_SEGMENTS_STARTED':
                    self._tar_filename = status_info
                elif status == 'EXPANSION_PREPARE_STARTED':
                    self._input_filename = status_info
                elif status == 'UPDATE_OLD_SEGMENTS_STARTED':
                    self._original_primary_count = int(status_info)
                elif status == 'UPDATE_MASTER_STARTED':
                    self._gp_configuration_backup = status_info

                self._status.append(status)
                self._status_info.append(status_info)
        except IOError:
            raise

        if not self._status_values.has_key(self._status[-1]):
            raise InvalidStatusError('Invalid status file.  Unknown status %s' % self._status)

    def create_status_file(self):
        """Creates a new gpupgrademirror status file"""
        try:
            self._fp = open(self._status_filename, 'w')
            if self._master_mirror:
                self._fp_standby = open(self._status_standby_filename, 'w')
                self._fp_standby.write('UNINITIALIZED:None\n')
                self._fp_standby.flush()
            self._fp.write('UNINITIALIZED:None\n')
            self._fp.flush()
            self._status.append('UNINITIALIZED')
            self._status_info.append('None')
        except IOError:
            raise

        self._sync_status_file()


    def _sync_status_file(self):
        """Syncs the gpupgrademirror status file with the master mirror"""
        if self._master_mirror:
            cpCmd = RemoteCopy('gpupgrademirror copying status file to master mirror',
                               self._status_standby_filename, self._master_mirror.getSegmentHostName(),
                               self._status_filename)
            cpCmd.run(validateAfter=True)

    def set_status(self, status, status_info=None):
        """Sets the current status.  gpupgrademirror status must be set in
           proper order.  Any out of order status result in an
           InvalidStatusError exception"""
        self.logger.debug("Transitioning from %s to %s" % (self._status[-1],status))

        if not self._fp:
            raise InvalidStatusError('The status file is invalid and cannot be written to')
        if not self._status_values.has_key(status):
            raise InvalidStatusError('%s is an invalid gpupgrademirror status' % status)
        # Only allow state transitions forward or backward 1
        if self._status and \
           self._status_values[status] != self._status_values[self._status[-1]] + 1:
                raise InvalidStatusError('Invalid status transition from %s to %s' % (self._status[-1], status))
        if self._master_mirror:
            self._fp_standby.write('%s:%s\n' % (status, status_info))
            self._fp_standby.flush()
            self._sync_status_file()
        self._fp.write('%s:%s\n' % (status, status_info))
        self._fp.flush()
        self._status.append(status)
        self._status_info.append(status_info)

    def get_current_status(self):
        """Gets the current status that has been written to the gpupgrademirror
           status file"""
        if (len(self._status) > 0 and len(self._status_info) > 0):
            return (self._status[-1], self._status_info[-1])
        else:
            return (None, None)

    def get_status_history(self):
        """Gets the full status history"""
        return zip(self._status, self._status_info)

    def remove_status_file(self):
        """Closes and removes the gpupgrademirror status file"""
        if self._fp:
            self._fp.close()
            self._fp = None
        if self._fp_standby:
            self._fp_standby.close()
            self._fp_standby = None
        if os.path.exists(self._status_filename):
            os.unlink(self._status_filename)
        if os.path.exists(self._status_standby_filename):
            os.unlink(self._status_standby_filename)
        if self._master_mirror:
            RemoveFiles.remote('gpupgrademirror master mirror status file cleanup',
                               self._master_mirror.getSegmentHostName(),
                               self._status_filename)


    def is_standby(self):
        """Returns True if running on standby"""
        return os.path.exists(self._master_data_directory + self._status_standby_filename)


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class gpupgrademirrorSafeMode(threading.Thread):
    '''
     An object of this class represents a single node in a cluster. 
     It will upgrade all mirror segments on that node.
    '''


    def __init__(self, dburl, logger, master_data_directory, node_name):
        threading.Thread.__init__(self)
        
        self.tempOldMirrorDirSuffix = 'gpupgrademirrors.originalmirror'
        self.tempNewMirrorDirSuffix = 'gpupgrademirrors.copyfromprimary'
        self.fixupFileList          = [ 'postgresql.conf' 
                                      , 'pg_hba.conf'
                                      , 'pg_ident.conf'
                                      ]
        self.dburl                  = dburl
        self.logger                 = logger
        self.master_data_directory  = master_data_directory
        self.nodeName               = node_name
        self.mirrorSegmentList      = []
        self.statusList             = []

    #-------------------------------------------------------------------------------
    def  setup(self, mirrorInfoList):
        self.mirrorSegmentList = mirrorInfoList

        for seg in self.mirrorSegmentList:
            ''' Make sure their is enough space for the largest segment '''
            self.checkAvailableSpace()            
            
            print 'seg = ' + str(seg)
            
            ''' Create a status file for each mirror segment '''
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , master_data_directory = self.master_data_directory
                                          , segment_dbid = seg.dbid
                                          , master_mirror = False)
            self.statusList.append(status)
            status.create_status_file()
            

    #-------------------------------------------------------------------------------
    def renameOldMirrorToTemp(self, seg):
        print 'in renameOldMirror'
        
        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        rmCmd = MoveDirectory( name = 'gpupgrademirror move old mirror to temp location: %s:%s' % (seg.mirror_data_directory, tempDataDir)
                             , srcDirectory = seg.mirror_data_directory
                             , dstDirectory = tempDataDir
                             , ctxt  = REMOTE
                             , remoteHost = self.nodeName
                             )
        rmCmd.run(validateAfter = True)
        
    #-------------------------------------------------------------------------------
    def renameNewMirrorToOldMirror(self, seg):
        print 'in moveNewMirror'
        
        tempDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix
        rmCmd = MoveDirectory( name = 'gpupgrademirror move new mirror to mirror location: %s:%s' % (tempDataDir, seg.mirror_data_directory)
                          , srcDirectory = tempDataDir
                          , dstDirectory = seg.mirror_data_directory
                          , ctxt  = REMOTE
                          , remoteHost = self.nodeName
                          )
        rmCmd.run(validateAfter = True)
        

    #-------------------------------------------------------------------------------                                              
    def copyPrimaryToMirror(self, seg):
        print 'in copyPrimaryToMirror'

        tempDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix
        rmCmd = RemoteCopy( name = 'gpupgrademirror copy primary to mirror: %s:%s' % (self.nodeName, seg.mirror_data_directory)
                          , srcDirectory = seg.primary_data_directory
                          , dstHost = self.nodeName
                          , dstDirectory = tempDataDir
                          , ctxt  = REMOTE
                          , remoteHost = seg.primary_host
                          )
        rmCmd.run(validateAfter = True)
    
    #------------------------------------------------------------------------------ 
    def newMirrorFileFixup(self, seg):
        print 'in newMirrorFileFixup'

        tempTargetDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix
        for file in self.fixupFileList:
            fullPathFile = seg.mirror_data_directory + '/' + file
            fullPathTarget = tempTargetDataDir + '/' + file
            rmCmd = RemoteCopy( name = 'gpupgrademirror fixup copy : %s:%s' % (fullPathFile, fullPathTarget)
                              , srcDirectory = fullPathFile
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget
                              , ctxt = REMOTE
                              , remoteHost = self.nodeName
                              )
            rmCmd.run(validateAfter = True)
        
        
    #-------------------------------------------------------------------------------
    def removeOldMirror(self, seg):
        print 'in removeOldMirror'

        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror data directory: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
        rmCmd.run(validateAfter = True)

    
    #-------------------------------------------------------------------------------
    def startMovement(self):
       print 'in startMovement' 


    #-------------------------------------------------------------------------------
    def checkAvailableSpace(self):
        print 'in checkAvailableSpace'

        for seg in self.mirrorSegmentList:
            dirSize = DiskUsage.get_size( name = "primaryDir"
                                        , remote_host = seg.primary_host
                                        , directory = seg.primary_data_directory
                                        )        
            mirrorSize = DiskFree.get_size( name = 'disk space available'
                                      , remote_host = self.nodeName
                                      , directory = seg.mirror_data_directory
                                      )
            ###print 'mirrorSize = ' + str(mirrorSize)
            if mirrorSize < dirSize:
               logger.error("Size of mirror directory %s is too small." % mirror[2])          


    #-------------------------------------------------------------------------------
    def run(self):
        i = 0
        for seg in self.mirrorSegmentList:
            status = self.statusList[i]
            
            ''' Copy the entire primary segment over to the mirror '''
            status.set_status('START_PRIMARY_COPY')
            self.copyPrimaryToMirror(seg)
            status.set_status('END_PRIMARY_COPY')
            
            ''' Fixup some of the new mirror files with the old mirror files '''
            status.set_status('START_NEW_MIRROR_FILE_FIXUP')
            self.newMirrorFileFixup(seg)
            status.set_status('END_NEW_MIRROR_FILE_FIXUP')
            
            ''' Rename the old mirror to a temporary name '''
            status.set_status('START_MOVE_OLD_MIRROR')
            self.renameOldMirrorToTemp(seg)
            status.set_status('END_MOVE_OLD_MIRROR')
            
            ''' Rename the new mirror the the name of the old mirror '''
            status.set_status('START_MOVE_NEW_MIRROR')
            self.renameNewMirrorToOldMirror(seg)
            status.set_status('END_MOVE_NEW_MIRROR')
            
            ''' Remove the old mirror '''
            status.set_status('START_REMOVE_OLD_MIRROR')
            self.removeOldMirror(seg)
            status.set_status('END_REMOVE_OLD_MIRROR')
            
            status.set_status('GPUPGRADEMIRROR_DONE')            
            i = i + 1


#
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class gpupgrademirror:

    dburl     = None
    safe_mode = None
    currentPMList      = []
    currentPrimaryList = []
    currentMirrorList  = []
    statusList         = []


    def __init__(self, dburl, logger, master_data_directory, safe_mode = True):
        self.dburl = dburl
        self.logger = logger
        self.master_data_directory = master_data_directory
        self.safe_mode = safe_mode
        self.parallel = 1

    #-------------------------------------------------------------------------------
    def  setup(self):
        conn   = dbconn.connect( dburl   = self.dburl
                               , utility = True
                               )
        cursor = dbconn.execSQL(conn, gpconfig_row.query())
        i      = 1

        for row in cursor:
            (content, definedprimary, dbid, isprimary, valid, hostname, port, datadir) = row
            if (isprimary == True):
               ###self.currentPrimaryList.append((content, hostname, datadir, dbid))
               self.currentPrimaryList.append((content, hostname, '/Users/ksell/temp/junkDatadir/primary' + str(i), dbid))
               self.currentPMList.append((content, hostname, '/Users/ksell/temp/junkDatadir/primary' + str(i), dbid))
            else:
               ###self.currentMirrorList.append((content, hostname, datadir))
               self.currentMirrorList.append((content, hostname, '/Users/ksell/temp/junkDatadir/mirror' + str(i), dbid))
               self.currentPMList.append((content, hostname, '/Users/ksell/temp/junkDatadir/mirror' + str(i), dbid))
               i = i + 1
            ###self.currentPMList.append((content, hostname, datadir, debid))
        cursor.close()
        conn.close()

        self.numworkers=len(self.currentPrimaryList)
        self.pool = WorkerPool(numWorkers = self.numworkers)


        print ''
        print str(self.currentPrimaryList)
        print ''
        print str(self.currentMirrorList)
        print ''

        for seg in self.currentPrimaryList:
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , master_data_directory = self.master_data_directory
                                          , segment_dbid = seg[3]
                                          , master_mirror = False)
            self.statusList.append(status)
            status.create_status_file()

   
    #-------------------------------------------------------------------------------
    def removeOldMirrors(self):
        print 'in removeOldMirrors'
        i = 1

        commandsSent = {}
        for mirror in self.currentMirrorList:
            hostname = mirror[1]
            datadir  = mirror[2]

            if not commandsSent.has_key(hostname):
                commandsSent[hostname] = 0

            rmCmd = RemoveFiles('gpupgrademirror remove old mirror data directory: %s:%s' % (hostname, datadir),
                                    datadir, ctxt=REMOTE, remoteHost=hostname)
            if commandsSent[hostname] != 0 and commandsSent[hostname] % 8 == 0:
                logger.debug('Sleeping for 1 second to allow sshd to catch up for host %s' % hostname)
                sleep(1)

            commandsSent[hostname] += 1
            self.pool.addCommand(rmCmd)

        self.pool.join()
        self.pool.check_results()


    #-------------------------------------------------------------------------------                                              
    def copyPrimaryToMirrors(self):
        print 'in copyPrimaryToMirrors'
        i = 0

        commandsSent = {}
        ###print 'len(self.currentPrimaryList) = ' + str(len(self.currentPrimaryList))
        ###print 'len(self.currentMirrorList)  = ' + str(len(self.currentPrimaryList))
        while i < len(self.currentPrimaryList):
            pHostname = self.currentPrimaryList[i][1]
            pDatadir  = self.currentPrimaryList[i][2]
            mHostname = self.currentMirrorList[i][1]
            mDatadir  = self.currentMirrorList[i][2]

            if not commandsSent.has_key(pHostname):
                commandsSent[pHostname] = 0

            rmCmd = RemoteCopy( name = 'gpupgrademirror copy primary to mirror: %s:%s' % (pHostname, pDatadir)
                              , srcDirectory = pDatadir
                              , dstHost = mHostname
                              , dstDirectory = mDatadir
                              , ctxt  = REMOTE
                              , remoteHost = pHostname
                              )
            if commandsSent[pHostname] != 0 and commandsSent[pHostname] % 8 == 0:
                logger.debug('Sleeping for 1 second to allow sshd to catch up for host %s' % pHostname)
                sleep(1)

            commandsSent[pHostname] += 1
            self.pool.addCommand(rmCmd)
            i = i + 1

        self.pool.join()
        self.pool.check_results()

   
        
    #-------------------------------------------------------------------------------
    def startMovement(self):
       print 'in startMovement' 



    #-------------------------------------------------------------------------------
    def findLargestPrimarySegment(self):

      largestSize = 0

      for primary in self.currentPrimaryList:
          dirSize = DiskUsage.get_size( name = "primaryDir"
                                      , remote_host = primary[1]
                                      , directory = primary[2]
                                      )
          if dirSize > largestSize:
             largestSize = dirSize
      return largestSize


    #-------------------------------------------------------------------------------
    def checkAvailableSpace(self):
        print 'in checkAvailableSpace'

        minSizeNeeded = self.findLargestPrimarySegment()
        ###print 'minSizeNeeded = ' + str(minSizeNeeded)
        for mirror in self.currentMirrorList:
            mirrorSize = DiskFree.get_size( name = 'disk space available'
                                          , remote_host = mirror[1]
                                          , directory = mirror[2]
                                          )
            ###print 'mirrorSize = ' + str(mirrorSize)
            if mirrorSize < minSizeNeeded:
               logger.error("Size of mirror directory %s is too small." % mirror[2])          


#
#-------------------------------------------------------------------------------
#--------------------------------- Main ----------------------------------------
#-------------------------------------------------------------------------------

dburl      = None
conn       = None
remove_pid = True
MirrorNodeList = []
MirrorInfoList = []
safeModeMirrorNodeList = []


try:
  logger = get_default_logger()
  setup_tool_logging(EXECNAME,getLocalHostname(),getUserName())

  options, args = parseargs()

  if options.verbose:
     enable_verbose_logging()

  if is_gpupgrademirror_running(options.master_data_directory):
     logger.error('gpupgrademirror is already running.  Only one instance')
     logger.error('of gpupgrademirror is allowed at a time.')
     remove_pid = False
     sys.exit(1)
  else:
     create_pid_file(options.master_data_directory)

  dbUrl = dbconn.DbURL()

  if options.safe_mode == True:
    ''' Run in safe mode '''
    conn   = dbconn.connect( dburl   = dbUrl
                           , utility = True
                           )
    
    ''' Get a list of mirrors '''
    mirrorNodeCursor = dbconn.execSQL(conn, mirror_hosts_row.query())
    numMirrorHosts = 0
    for row in mirrorNodeCursor:
        host = mirror_hosts_row(row)
        numMirrorHosts = numMirrorHosts + 1
        mirrorhost = host.hostname
        print 'mirrorhost = ' + str(mirrorhost)
        
        ''' Create a gpupgrademirrorSafeMode object for each mirror node'''
        tempNode = gpupgrademirrorSafeMode(dbUrl, logger, options.master_data_directory, mirrorhost)

        ''' Get info on mirror segments for this host'''
        mirrorSegInfoCursor = dbconn.execSQL(conn, mirror_host_info_row.query() % mirrorhost)
        mirrorInfoList = []
        for mirrorInfoRow in mirrorSegInfoCursor:
            mirrorInfo = mirror_host_info_row(mirrorInfoRow)
            mirrorInfoList.append(mirrorInfo)
        
        ''' Setup the mirror node '''
        tempNode.setup(mirrorInfoList)
        safeModeMirrorNodeList.append(tempNode)
    
    ''' Start the threads '''
    for node in safeModeMirrorNodeList:
        node.start()
    for node in safeModeMirrorNodeList:
        node.join()    
    
  else:
    ''' Run in UNsafe mode '''  
    testUM  = gpupgrademirror(dbUrl, logger, options.master_data_directory, safe_mode = options.safe_mode)
    testUM.setup()
    testUM.checkAvailableSpace()
    testUM.removeOldMirrors()
    testUM.copyPrimaryToMirrors()

  sys.exit(0)

except Exception, e:
  print "FATAL Exception: " + str(e)
  traceback.print_exc()
  sys.exit(1)


finally:
    try:
        if remove_pid:
            remove_pid_file(options.master_data_directory)
    except NameError:
        pass
    logger.info("gpupgrademieeor complete")
