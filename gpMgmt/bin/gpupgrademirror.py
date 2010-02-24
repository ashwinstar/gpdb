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
from gppylib.commands.base import *

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

TenMeg = 10485760


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class mirror_hosts_row:
    
    def __init__(self, row):
        self.hostname = row[0]

    #-------------------------------------------------------------------------------
    @staticmethod
    def query():
        if options.phase2 == False:
           exeStr = '''  SELECT  DISTINCT hostname as nicaddress
                         FROM             pg_catalog.gp_configuration
                         WHERE            isprimary = 'f'
                         ORDER BY         hostname '''
        else:
           exeStr = ''' SELECT DISTINCT hostname 
                        FROM            pg_catalog.gp_segment_configuration
                        WHERE           role = 'm'
                        ORDER BY        hostname '''
        return exeStr


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class gp_host_cache:
    """ This class represents the information stored in the .gphostcache file """
    
    def __init__(self):
        self.hostcache = {}
        self.readfile()
        
    #-------------------------------------------------------------------------------
    def readfile(self):
        FILEDIR   = os.path.expanduser("~")
        FILENAME  = ".gphostcache"
        CACHEFILE = FILEDIR + "/" + FILENAME

        try:
            file = open(CACHEFILE, 'r')
                
            for line in file:
                (interface,hostname)=line.split(':')
                self.hostcache[interface.strip()]=hostname.strip()                    
        except IOError:
            raise Exception(' file %s not found' % path)                    
        finally:
            file.close()        
    
    #-------------------------------------------------------------------------------
    def getInterfaces(self, hostName):
        retList = []
        
        for (interface, host) in self.hostcache:
            if host == hostName:
               relList.append(interface)
        return relList
    
    #-------------------------------------------------------------------------------
    def getHost(self, interface):
        return self.hostcache[interface]
    
    #-------------------------------------------------------------------------------
    def getHostList(self):
        retList = []
        
        for (interface, host) in self.hostcache:
            if host in retList:
                continue
            else:
                retList.append(host)


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class mirror_host_info_row:
    
    def __init__(self, row):
        self.mirror_host  = row[0]
        self.primary_host = row[1]
        self.primary_data_directory = row[2]
        self.mirror_data_directory = row[3]
        self.dbid = row[4]

    #-------------------------------------------------------------------------------
    def __str__(self):
        tempStr = 'mirror_host='            + str(self.mirror_host)            + '\n' \
        +         'primary_host='           + str(self.primary_host)           + '\n' \
        +         'primary_data_directory=' + str(self.primary_data_directory) + '\n' \
        +         'mirror_data_directory='  + str(self.mirror_data_directory)  + '\n' \
        +         'dbid='                   + str(self.dbid)                   + '\n'
        return tempStr 

    #-------------------------------------------------------------------------------
    @staticmethod
    def readListFromFile(nvFileName):
        """ Static method to convert file with information in the __str__ method format """
        """ for this object into a list of tuples.                                      """
        retList = []
        
        logger.debug("Trying to read in a pre-existing gpupgrademirror mirror_host_info_row file.")
        
        try:
            fp = open(nvFileName,'r')
            fp.seek(0)

            index = 0
            row = []
            for line in fp:
                (name, value) = line.rstrip().rsplit('=', 1)
                row.append(value)
                index = index + 1
                if index == 5:     
                   retList.append(mirror_host_info_row(row))
                   row = []
                   index = 0
            return retList
        except IOError:
            raise

    #-------------------------------------------------------------------------------
    @staticmethod
    def writeListToFile(nvFileName, listOfRows):
        """ Static method to write out a list of tuples in the __str__ method format """

        logger.debug("Trying to write a list of mirror_host_info_row to a file.")
       
        try:
           fp = open(nvFileName, 'w')
           fp.seek(0)
           for row in listOfRows:
               fp.write(str(row))
           fp.flush()
           fp.close()
        except IOError:
           raise

    #-------------------------------------------------------------------------------
    @staticmethod
    def query():     
        if options.phase2 == False:
           exeStr =  '''SELECT         t2.hostname as mirror_host
                                     , t1.hostname as primary_host
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
        else: 
           exeStr = """SELECT          t2.hostname as mirror_host
                                     , t1.hostname as primary_host
                                     , t3.fselocation  as primary_data_directory
                                     , t4.fselocation  as mirror_data_directory
                                     , t2.dbid     as dbid
                            FROM   ((pg_catalog.gp_segment_configuration as t1
                                      INNER JOIN
                                    pg_catalog.gp_segment_configuration as t2
                                      ON     t1.content = t2.content
                                         AND t2.hostname = '%s'
                                         AND t1.content >= 0
                                         AND t1.role = 'p'
                                         AND t2.role = 'm'
                                    ) INNER JOIN pg_catalog.pg_filespace_entry as t3
                                          ON   t3.fsedbid = t1.dbid
                                   ) INNER JOIN pg_catalog.pg_filespace_entry as t4
                                      ON t4.fsedbid = t2.dbid
                            ORDER BY  primary_host"""
            
        return exeStr


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class phase1MirrorInfo:


    def __init__(self, url = None):
        self.dbUrl = url
        self.mirrorInfoFile = options.master_data_directory + '/' + 'gpupgrademirror.mirrorinfo'
        self.mirrorNodeList = []
        self.allMirrorInfoList = []
        self.gpHostCacheInfo = gp_host_cache()
    
        if (options.rollback == True or options.continue_upgrade == True) and options.phase2 == False:
           """ Retrieve configuration information from previously saved file """
           self.allMirrorInfoList = mirror_host_info_row.readListFromFile(phase1MirrorInfo.getMirrorInfoFile())
           
           for seg in self.allMirrorInfoList:
               if seg.mirror_host in self.mirrorNodeList:
                   pass
               else:
                   self.mirrorNodeList.append(seg.mirror_host)
        else:
           conn   = dbconn.connect( dburl   = dbUrl
                                  , utility = True
                                  )
           ''' Get a list of mirror nodes '''
           mirrorNodeCursor = dbconn.execSQL(conn, mirror_hosts_row.query())
           for row in mirrorNodeCursor:
               mirror = mirror_hosts_row(row)
               self.mirrorNodeList.append(mirror.hostname)

               ''' Get info on mirror segments for this host'''
               mirrorSegInfoCursor = dbconn.execSQL(conn, mirror_host_info_row.query() % mirror.hostname)
               for mirrorInfoRow in mirrorSegInfoCursor:
                   mirrorInfo = mirror_host_info_row(mirrorInfoRow)
                   self.allMirrorInfoList.append(mirrorInfo)
           conn.close()  
            
           mirror_host_info_row.writeListToFile(phase1MirrorInfo.getMirrorInfoFile(), self.allMirrorInfoList)   
        
        
    #-------------------------------------------------------------------------------
    def getMirrorNodeList(self):
        return self.mirrorNodeList
    
    #-------------------------------------------------------------------------------
    def getMirrorInfo(self, mirrorName):
        retList = []
        
        for mirrorSeg in self.allMirrorInfoList:
            if mirrorSeg.mirror_host == mirrorName:
                retList.append(mirrorSeg)
        
        return retList

    #-------------------------------------------------------------------------------
    @staticmethod
    def getMirrorInfoFile():
        return options.master_data_directory + '/' + 'gpupgrademirror.mirrorinfo'


    #-------------------------------------------------------------------------------
    @staticmethod
    def removeMirrorInfoFile():
        try:
           os.remove(self.mirrorInfoFile)
        except:
           pass


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
                      help='rollback failed expansion setup.', default=False)
    parser.add_option('-c', '--continue-upgrade', action='store_true',
                      help='continue expansion.', default=False)
    parser.add_option('-m', '--mode', default='S',
                      help='Valid values are S for safe mode and U for un-safe mode')
    parser.add_option('-P', '--phase2', action='store_true',
                      help='phase 2 for upgrade of mirrors.', default=False)
    parser.add_option('-v','--verbose', action='store_true',
                      help='debug output.', default=False)
    parser.add_option('-h', '-?', '--help', action='help',
                        help='show this help message and exit.', default=False)
    parser.add_option('--usage', action="briefhelp")

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
class gpumDiskFree(Command):

    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):
        self.directory=directory
        cmdStr="%s %s" % (SYSTEM.getDiskFreeCmd(),directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def get_size(name,remote_host,directory):
        dfCmd=DiskFree( name = name
                      , directory = directory
                      , ctxt = REMOTE
                      , remoteHost = remote_host
                      )
        dfCmd.run(validateAfter=True)
        return dfCmd.get_bytes_free()


    def get_bytes_free(self):
        '''expected output of the form:                                                                                                          
           Filesystem   512-blocks      Used Available Capacity  Mounted on                                                                      
           /dev/disk0s2  194699744 158681544  35506200    82%    /                                                                               
        '''
        rawIn=self.results.stdout.split('\n')[1]
        bytesFree=int(rawIn.split()[3])*1024
        return bytesFree


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class MoveDirectory(Command):

    def __init__(self,name,srcDirectory,dstDirectory,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory=srcDirectory
        self.dstDirectory=dstDirectory
        cmdStr="%s -f %s %s" % (findCmdInPath('mv'),srcDirectory,dstDirectory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class FileDirectoryList(Command):

    def __init__(self, name, filePattern, ctxt=LOCAL, remoteHost=None):
        self.filePattern = filePattern
        cmdStr="%s -1 %s" % (findCmdInPath('ls'), filePattern)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def get_list(name, remote_host, file_pattern):
        lsCmd = FileDirectoryList(name, filePattern = file_pattern, remoteHost = remote_host)
        lsCmd.run(validateAfter=False)
        return lsCmd.get_result_list()
    
    def get_result_list(self):
        files = self.results.stdout.split('\n')
        if files != None and len(files) > 0 and files[-1] == '':
           ''' Remove any trailing empty string '''
           files.pop()
        return files
    
    
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class GpUpgradeMirrorStatus():
    """Class that manages gpupgrademirror status file.

    The status file is placed in the master data directory on both the master and
    the standby master.  it's used to keep track of where we are in the progression.
    """
    _safeModeStatusValues = { 'PHASE1_SAFE_MODE'            : 2
                            , 'START_PRIMARY_COPY'          : 3
                            , 'END_PRIMARY_COPY'            : 4
                            , 'START_NEW_MIRROR_FILE_FIXUP' : 5 
                            , 'END_NEW_MIRROR_FILE_FIXUP'   : 6
                            , 'START_MOVE_OLD_MIRROR'       : 7
                            , 'END_MOVE_OLD_MIRROR'         : 8
                            , 'START_MOVE_NEW_MIRROR'       : 9
                            , 'END_MOVE_NEW_MIRROR'         : 10
                            , 'START_REMOVE_OLD_MIRROR'     : 11
                            , 'END_REMOVE_OLD_MIRROR'       : 12
                            , 'PHASE1_DONE'                 : 13
                            , 'START_PHASE2'                : 14
                            , 'PHASE2_DONE'                 : 15
                            }

    _unsafeModeStatusValues  = { 'PHASE1_UNSAFE_MODE'                  : 2
                                , 'START_MOVE_OLD_MIRROR_SPECIAL_FILES' : 3
                                , 'END_MOVE_OLD_MIRROR_SPECIAL_FILES'   : 4
                                , 'START_REMOVE_OLD_MIRROR'             : 5
                                , 'END_REMOVE_OLD_MIRROR'               : 6
                                , 'START_PRIMARY_COPY'                  : 7
                                , 'END_PRIMARY_COPY'                    : 8
                                , 'START_NEW_MIRROR_FILE_FIXUP'         : 9
                                , 'END_NEW_MIRROR_FILE_FIXUP'           : 10
                                , 'START_MOVE_NEW_MIRROR'               : 11
                                , 'END_MOVE_NEW_MIRROR'                 : 12
                                , 'PHASE1_DONE'                         : 13
                                , 'START_PHASE2'                        : 14
                                , 'PHASE2_DONE'                         : 15
                                }
    
    
    def __init__(self, logger, master_data_directory, segment_dbid, master_mirror, phase):
        self.logger = logger
        
        if options.mode == 'S':
           self._status_values = self._safeModeStatusValues
        else:
           self._status_values = self._unsafeModeStatusValues
            
        self._status = []
        self._status_info = []
        self._master_data_directory = master_data_directory
        self._master_mirror = master_mirror
        self._phase = phase
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


   #-------------------------------------------------------------------------------
    def _read_status_file(self):
        """Reads in an existing gpupgrademirror status file"""
        self.logger.debug("Trying to read in a pre-existing gpupgrademirror status file")
        try:
            self._fp = open(self._status_filename, 'a+')
            self._fp.seek(0)

            for line in self._fp:
                (status, status_info) = line.rstrip().split(':')
                ''' Determine if the previous run was phase 1 and was in safe or un-safe mode '''
                ''' re-set the mode based on the result'''
                if status == 'PHASE1_SAFE_MODE':
                    options.mode = 'S'
                    self._status_values = self._safeModeStatusValues
                elif status == 'PHASE1_UNSAFE_MODE':
                    options.mode = 'U'
                    self._status_values = self._unsafeModeStatusValues
                elif status == 'START_PHASE2':
                    options.phase2 == True
                self._status.append(status)
                self._status_info.append(status_info)
        except IOErrormm, e:
           logger.warning('Unable to read segment status file: ' + str(e))
           logger.warning('gpupgradmirror exiting')
           exit(0)

        if not self._status_values.has_key(self._status[-1]):
            raise InvalidStatusError('Invalid status file.  Unknown status %s' % self._status)


   #-------------------------------------------------------------------------------
    def create_status_file(self):
        """Creates a new gpupgrademirror status file"""
        try:
            if options.phase2:
                startStatus = 'PHASE1_DONE'
            elif options.mode == 'S':
                startStatus = 'PHASE1_SAFE_MODE'
            else:
                startStatus = 'PHASE1_UNSAFE_MODE'
                
            self._fp = open(self._status_filename, 'w')
            if self._master_mirror:
                self._fp_standby = open(self._status_standby_filename, 'w')
                self._fp_standby.write(startStatus + ':None\n')
                self._fp_standby.flush()
            self._fp.write(startStatus + ':None\n')
            self._fp.flush()
            self._status.append(startStatus)
            self._status_info.append('None')
        except IOError, e:
            logging.error('Unable to create status file %s.' % self._status_filename)
            raise e

        self._sync_status_file()


   #-------------------------------------------------------------------------------
    def _sync_status_file(self):
        """Syncs the gpupgrademirror status file with the master mirror"""
        if self._master_mirror:
            cpCmd = RemoteCopy('gpupgrademirror copying status file to master mirror',
                               self._status_standby_filename, self._master_mirror.getSegmentHostName(),
                               self._status_filename)
            cpCmd.run(validateAfter=True)


   #-------------------------------------------------------------------------------
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
        ''' KAS note to self. Should probably not skip checking if in rollback '''
        if self._status and options.rollback == False:
           if options.continue_upgrade == False and self._status_values[status] != self._status_values[self._status[-1]] + 1:
              raise InvalidStatusError('Invalid status transition from %s to %s' % (self._status[-1], status))
           elif options.continue_upgrade == True and \
              self._status_values[status] != self._status_values[self._status[-1]] and \
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


   #-------------------------------------------------------------------------------
    def get_current_status(self):
        """Gets the current status that has been written to the gpupgrademirror
           status file"""
        if (len(self._status) > 0 and len(self._status_info) > 0):
            return (self._status[-1], self._status_info[-1])
        else:
            return (None, None)


   #-------------------------------------------------------------------------------        
    def get_current_status_step_number(self):
        currentStatus = self.get_current_status()
        if currentStatus[0] == None:
            return -1
        return self._status_values[currentStatus[0]]
        

   #-------------------------------------------------------------------------------    
    def compare_status(self, status2):
        ''' return -1 status1 < status2
                    0 status1 = status2
                    1 status1 > status2
        '''
        status1 = self.get_current_status()[0]
        if status1 == None:
            if status2 == None:
                return 0
            return -1
        if status2 == None:
            return 1
            
        num1 = self._status_values[status1]
        num2 = self._status_values[status2]
        if num1 < num2:
            return -1
        elif num1 == num2:
            return 0
        else:
            return 1
    

   #-------------------------------------------------------------------------------
    def get_status_history(self):
        """Gets the full status history"""
        return zip(self._status, self._status_info)


   #-------------------------------------------------------------------------------
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


   #-------------------------------------------------------------------------------
    def is_standby(self):
        """Returns True if running on standby"""
        return os.path.exists(self._master_data_directory + self._status_standby_filename)


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class gpupgrademirror(threading.Thread):
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
            if options.rollback == False and options.continue_upgrade == False:
               self.checkAvailableSpace()            
            
            ''' Create a status object for each mirror segment '''
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , master_data_directory = self.master_data_directory
                                          , segment_dbid = seg.dbid
                                          , master_mirror = False
                                          , phase = 1
                                          )
            self.statusList.append(status)
            if options.rollback == True:
                ''' setup for rollback '''
                if status.get_current_status() == (None, None):
                    logging.error('There is no upgrade mirror in progress')
                    raise ValidationError('Unable to rollback')
                if     status.compare_status('START_REMOVE_OLD_MIRROR') >= 0 \
                   and status.compare_status('PHASE1_DONE') < 0:
                   ''' We are past the point of recovering. Must move forward at this point '''
                   logging.error('The upgrade of the system mirrors is past the point of recovery.')
                   logging.error('You must continue system mirror upgrade at this point.')
                   raise ValidationError('Unable to rollback')
            elif options.continue_upgrade == True:
                ''' Setup to continue upgrade '''
            else:
                if (status.get_current_status() != (None, None)):
                   logging.error("Upgrade mirrors already started.")
                   logging.error("You must either rollback or continue mirror upgrade") 
                   raise ValidationError("Unable to continue")
                else:  
                   status.create_status_file()
            

    #-------------------------------------------------------------------------------
    def renameOldMirrorToTemp(self, seg):
        self.logger.debug('in renameOldMirror')
        
        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix

        if options.continue_upgrade == True:
           cmd = RemoveFiles( name = 'gpupgrademirror remove any old directory'
                                , remoteHost = self.nodeName 
                                , directory = tempDataDir
                                )
           cmd.run(validateAfter = False)
        
        if options.mode == 'S':
           rmCmd = MoveDirectory( name = 'gpupgrademirror move old mirror to temp location: %s:%s' % (seg.mirror_data_directory, tempDataDir)
                                , srcDirectory = seg.mirror_data_directory
                                , dstDirectory = tempDataDir
                                , ctxt  = REMOTE
                                , remoteHost = self.nodeName
                                )
           rmCmd.run(validateAfter = True)
        else:
           ''' Un-safe mode '''
           rmCmd = MakeDirectory( name = 'gpupgrademirror make temp directory for old mirror special files: %s' % tempDataDir
                                , directory = tempDataDir
                                , remoteHost = self.nodeName
                                )
           rmCmd.run(validateAfter = True) 
           for file in self.fixupFileList:
                fullPathFile = seg.mirror_data_directory + '/' + file
                fullPathTarget = tempDataDir + '/' + file
                rmCmd = RemoteCopy( name = 'gpupgrademirror fixup file list fixup copy : %s:%s' % (fullPathFile, fullPathTarget)
                              , srcDirectory = fullPathFile
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget
                              , ctxt = REMOTE
                              , remoteHost = self.nodeName
                              )
                rmCmd.run(validateAfter = True)
           
                
    #-------------------------------------------------------------------------------
    def renameTempMirrorToOldMirror(self, seg, dirSuffix):
        self.logger.debug('in renameTempMirrorToOldMirror')
        
        tempDataDir = seg.mirror_data_directory + dirSuffix
        rmCmd = MoveDirectory( name = 'gpupgrademirror move temp mirror to mirror location: %s:%s' % (tempDataDir, seg.mirror_data_directory)
                          , srcDirectory = tempDataDir
                          , dstDirectory = seg.mirror_data_directory
                          , ctxt  = REMOTE
                          , remoteHost = self.nodeName
                          )
        rmCmd.run(validateAfter = True)

    #-------------------------------------------------------------------------------                                              
    def copyPrimaryToMirror(self, seg):
        self.logger.debug('in copyPrimaryToMirror')

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
        self.logger.debug('in newMirrorFileFixup')

        if options.mode == 'S':
           tempSourceDataDir = seg.mirror_data_directory 
        else:
           tempSourceDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        
        tempTargetDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix

        for file in self.fixupFileList:
            fullPathFile = tempSourceDataDir + '/' + file
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
        self.logger.debug('in removeOldMirror')

        if options.mode == 'S' and options.rollback == False:
           ''' If in a safe mode upgrade, point to the temp locatio of the old mirror '''
           tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        else:
           ''' if in un-safe mode or in rollback, remove all files in the real mirror location'''
           tempDataDir = seg.mirror_data_directory
        rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror data directory: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
        rmCmd.run(validateAfter = True)

        
    #-------------------------------------------------------------------------------
    def removeOldMirrorSpecialFiles(self, seg):
        self.logger.debug('in removeOldMirrorSpecialFiles')

        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix

        rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror special files: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
        rmCmd.run(validateAfter = True)
        

    #-------------------------------------------------------------------------------
    def removeCopyOfPrimary(self, seg):
        ''' This method is used during recovery '''
        print 'in removeCopyOfPrimary'

        tempDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix
        rmCmd = RemoveFiles( name = 'gpupgrademirror remove the copied mirror data directory: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
        rmCmd.run(validateAfter = True)

    #-------------------------------------------------------------------------------
    def checkAvailableSpace(self):
        print 'in checkAvailableSpace'

        for seg in self.mirrorSegmentList:
            ''' Get size of primary '''
            dirSize = DiskUsage.get_size( name = "primaryDir"
                                        , remote_host = seg.primary_host
                                        , directory = seg.primary_data_directory
                                        )
            
            ''' Get available space on mirror '''        
            mirrorFreeSize = gpumDiskFree.get_size( name = 'disk space available'
                                      , remote_host = self.nodeName
                                      , directory = seg.mirror_data_directory
                                      )
            if options.mode == 'U':
                ''' Add current mirror size to available mirror space '''
                currentMirrorSize = DiskUsage.get_size( name = "mirrorDir"
                                        , remote_host = self.nodeName
                                        , directory = seg.mirror_data_directory
                                        )
                mirrorFreeSize = mirrorFreeSize + currentMirrorSize
 
            ''' Compare available space to needed space (add 10 Meg. just to make sure) '''
            if mirrorFreeSize + TenMeg < dirSize:
               logger.error("Size of mirror directory %s is too small %s:%s." % (self.nodeName, seg.mirror_data_directory))          
               raise ValidationError('Mirror data directory is too small')

    #-------------------------------------------------------------------------------
    def run(self):
        i = 0
        for seg in self.mirrorSegmentList:
            status = self.statusList[i]
            
            if options.rollback == True:
               ''' Put the mirror's back if necessary, and remove temp files '''
               if options.mode == 'S':
                  self.removeCopyOfPrimary(seg)
                  if status.compare_status('END_MOVE_OLD_MIRROR') >= 0 \
                    and status.compare_status('PHASE1_DONE') < 0:
                     ''' We have already moved the old mirror to a temp location,'''
                     ''' Go ahead and remove anything that might be in the segment's datadir'''
                     self.removeOldMirror(seg)
                     ''' Move the original mirror back to it's original location '''
                     self.renameTempMirrorToOldMirror(seg, self.tempOldMirrorDirSuffix)
                  status.set_status('PHASE1_SAFE_MODE')
               else:
                 self.removeOldMirrorSpecialFiles(seg)
                 status.set_status('PHASE1_UNSAFE_MODE')
                                   
            elif options.mode == 'S':
                if status.compare_status('START_PRIMARY_COPY') <= 0:
                   ''' Copy the entire primary segment over to the mirror '''
                   status.set_status('START_PRIMARY_COPY')
                   self.copyPrimaryToMirror(seg)
                   status.set_status('END_PRIMARY_COPY')
                
                if status.compare_status('START_NEW_MIRROR_FILE_FIXUP') <= 0:            
                   ''' Fixup some of the new mirror files with the old mirror files '''
                   status.set_status('START_NEW_MIRROR_FILE_FIXUP')
                   self.newMirrorFileFixup(seg)
                   status.set_status('END_NEW_MIRROR_FILE_FIXUP')
                
                if status.compare_status('START_MOVE_OLD_MIRROR') <= 0:
                   ''' Rename the old mirror to a temporary name '''
                   status.set_status('START_MOVE_OLD_MIRROR')
                   self.renameOldMirrorToTemp(seg)
                   status.set_status('END_MOVE_OLD_MIRROR')
                
                if status.compare_status('START_MOVE_NEW_MIRROR') <= 0:
                   ''' Rename the new mirror the the name of the old mirror '''
                   status.set_status('START_MOVE_NEW_MIRROR')
                   self.renameTempMirrorToOldMirror(seg, dirSuffix = self.tempNewMirrorDirSuffix)
                   status.set_status('END_MOVE_NEW_MIRROR')
                
                if status.compare_status('START_REMOVE_OLD_MIRROR') <= 0:
                   ''' Remove the old mirror '''
                   status.set_status('START_REMOVE_OLD_MIRROR')
                   self.removeOldMirror(seg)
                   status.set_status('END_REMOVE_OLD_MIRROR')
               
                if status.compare_status('PHASE1_DONE') < 0:
                   status.set_status('PHASE1_DONE')
                   
            else:
                ''' Un-safe mode '''
                if status.compare_status('START_MOVE_OLD_MIRROR_SPECIAL_FILES') <= 0:
                   ''' Rename the old mirror special files to a temporary name '''
                   status.set_status('START_MOVE_OLD_MIRROR_SPECIAL_FILES')
                   self.renameOldMirrorToTemp(seg)
                   status.set_status('END_MOVE_OLD_MIRROR_SPECIAL_FILES')
                   
                if status.compare_status('START_REMOVE_OLD_MIRROR') <= 0:
                   ''' Remove the old mirror '''
                   status.set_status('START_REMOVE_OLD_MIRROR')
                   self.removeOldMirror(seg)
                   status.set_status('END_REMOVE_OLD_MIRROR')
                       
                if status.compare_status('START_PRIMARY_COPY') <= 0:
                   ''' Copy the entire primary segment over to the mirror '''
                   status.set_status('START_PRIMARY_COPY')
                   self.copyPrimaryToMirror(seg)
                   status.set_status('END_PRIMARY_COPY')
                
                if status.compare_status('START_NEW_MIRROR_FILE_FIXUP') <= 0:            
                   ''' Fixup some of the new mirror files with the old mirror files '''
                   status.set_status('START_NEW_MIRROR_FILE_FIXUP')
                   self.newMirrorFileFixup(seg)
                   status.set_status('END_NEW_MIRROR_FILE_FIXUP')
               
                if status.compare_status('START_MOVE_NEW_MIRROR') <= 0:
                   ''' Rename the new mirror the the name of the old mirror '''
                   status.set_status('START_MOVE_NEW_MIRROR')
                   self.renameTempMirrorToOldMirror(seg, dirSuffix = self.tempNewMirrorDirSuffix)
                   status.set_status('END_MOVE_NEW_MIRROR')
               
                if status.compare_status('PHASE1_DONE') < 0:
                   status.set_status('PHASE1_DONE')        
            i = i + 1


    #-------------------------------------------------------------------------------
    def shutdown(self):
        ''' Need to do '''



#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class phase2(threading.Thread):

    def __init__(self, dburl, logger, master_data_directory, node_name):
        threading.Thread.__init__(self)
        
        self.dburl                  = dburl
        self.logger                 = logger
        self.master_data_directory  = master_data_directory
        self.nodeName               = node_name
        self.mirrorSegmentList      = []
        self.statusList             = []
        self.fixupFileList          = []


    #-------------------------------------------------------------------------------
    def  setup(self, mirrorInfoList, fileList, fileListDeleteBeforeCopy):
        self.mirrorSegmentList = mirrorInfoList

        for seg in self.mirrorSegmentList:
            
            ''' Get status file from Phase 1 for each mirror segment '''
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , master_data_directory = self.master_data_directory
                                          , segment_dbid = seg.dbid
                                          , master_mirror = False
                                          , phase = 2
                                          )
            self.statusList.append(status)
            
            if options.continue_upgrade == True:
                ''' Setup to continue upgrade '''
            else:
                if (status.get_current_status() != (None, None)):
                   logging.error("Upgrade mirrors phase 2 already started.")
                   logging.error("You must continue mirror upgrade") 
                   raise ValidationError("Unable to run phase 2")
                else:  
                   status.create_status_file()
                
        self.fixupFileList = fileList
        self.deleteBeforeCopy = fileListDeleteBeforeCopy


    #------------------------------------------------------------------------------ 
    def newMirrorSpecialFileFixup(self, seg):
        print 'in newMirrorSpecialFileFixup'

        for file in self.fixupFileList:
            fullPathSource = seg.primary_data_directory + '/' + file
            fullPathTarget = seg.mirror_data_directory + '/' + file

            if file in self.deleteBeforeCopy:
               cmd = RemoveFiles( name = 'gpupgrademirror remove special old directory'
                                , remoteHost = self.nodeName 
                                , directory = fullPathTarget
                                )
               cmd.run(validateAfter = False)

            rmCmd = RemoteCopy( name = 'gpupgrademirror phase 2 fixup copy : %s:%s' % (fullPathSource, fullPathTarget)
                              , srcDirectory = fullPathSource
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget
                              , ctxt = REMOTE
                              , remoteHost = seg.primary_host
                              )
            rmCmd.run(validateAfter = True)
    
            ''' Check for files with extensions (i.e. .1, .2, ...) '''
            fileList = FileDirectoryList.get_list('Get list file extensions', seg.primary_host, fullPathSource + '.*')
            for fileExt in fileList:
                suffixIndex = fileExt.rfind('.')
                suffix = fileExt[suffixIndex:]
                rmCmd = RemoteCopy( name = 'gpupgrademirror phase 2 segment files fixup copy : %s:%s' % (fullPathSource, fullPathTarget)
                              , srcDirectory = fileExt
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget + suffix
                              , ctxt = REMOTE
                              , remoteHost = seg.primary_host
                              )
                rmCmd.run(validateAfter = True)

    #-------------------------------------------------------------------------------
    def run(self):
        i = 0
        for seg in self.mirrorSegmentList:
            status = self.statusList[i]
            currentStatus = status.get_current_status()
            
            if status.compare_status('START_PHASE2') <= 0:
               status.set_status('START_PHASE2')
               ''' Copy catalog tables to mirror '''
               self.newMirrorSpecialFileFixup(seg)
            if status.compare_status('PHASE2_DONE') <= 0:
               status.set_status('PHASE2_DONE')
            i = i + 1

    #-------------------------------------------------------------------------------
    def shutdown(self):
        ''' Need to do '''


#
#-------------------------------------------------------------------------------
#--------------------------------- Main ----------------------------------------
#-------------------------------------------------------------------------------
""" 

  This the the main body of code for gpupgrademirror. gpupgradmirror has two phases
  (phase 1 and phase 2) and two modes (safe and unsafe). 
  
"""


dburl        = None
conn         = None
remove_pid   = True
p1MirrorInfo = None
MirrorNodeList = []
MirrorInfoList = []


try:
  logger = get_default_logger()
  setup_tool_logging(EXECNAME,getLocalHostname(),getUserName())

  '''
  theList = FileDirectoryList.get_list( name = 'test'
                                      , remote_host = 'lt-ksell.local'
                                      , file_pattern = '/Users/ksell/temp/junkRelease33/database/temp/10.*'
                                      )
  print len(theList)
  for element in theList:
      print str(element)
      suffixIndex = element.rfind('.')
      suffix = element[suffixIndex:]
      print str(suffix)
  sys.exit(0)
  '''
  
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
     
  """ 
      We will start the database in master only mode if this this phase 1 and 
      we are not rolling back or continuing. Otherwise, we will retrieve all the
      information we need from a flat file we saved on the initial run.     
  """
  if (options.rollback == True or options.continue_upgrade == True) and options.phase2 == False:
     pass
  else:
     GpStart.local('gpupgrademirror start database', masterOnly = True)

  if options.phase2 == True:
     """
        Phase 2 of upgrade mirrors 
     """
     databaseList = []
          
     if options.rollback == True:
        raise ValidationError('Rollback is not possible in phase 2 mirror upgrade.')
    
     conn   = dbconn.connect( dburl   = dbUrl
                            , utility = True
                            )

     ''' Get a list of databases '''
     databaseCursor = dbconn.execSQL(conn, database_row.query())
     for row in databaseCursor:
         dbrow = database_row(row)
         databaseList.append(dbrow)
         
     ''' Connect to each database, and make a list of files we will use '''
     fileList = []
     fileListDeleteBeforeCopy = []
     ''' always copy the global directory '''
     fileList.append('global')
     fileListDeleteBeforeCopy.append('global')
     ''' always copy the pg_xlog directory '''
     fileList.append('pg_xlog')
     fileListDeleteBeforeCopy.append('pg_xlog')
     for db in databaseList:
         if db.databaseName == 'template0':
            ''' Special case where we copy the entire database '''
            fileList.append(str('base/' + str(db.databaseDirectory)))
            fileListDeleteBeforeCopy.append(str('base/' + str(db.databaseDirectory)))
            continue
                            
         connectURL = dbconn.DbURL(dbname = db.databaseName)
         connectForDB = dbconn.connect( dburl   = connectURL
                                      , utility = True
                                      ) 
         ''' List of non-toast catalog tables '''
         DBcursor = dbconn.execSQL(connectForDB, non_catalog_toast_tables_row.query())
         for fileRow in DBcursor:
             nonToastRow = non_catalog_toast_tables_row(fileRow)
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(nonToastRow.filename))
             
         ''' List of toast catalog tables and their toast files '''
         DBcursor = dbconn.execSQL(connectForDB, catalog_toast_tables_row.query())
         for fileRow in DBcursor:
             toastTableRow = catalog_toast_tables_row(fileRow)
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(toastTableRow.filename))
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(toastTableRow.toastfilename))
         connectForDB.close()
    

     try:
        p1MirrorInfo = phase1MirrorInfo(dbUrl)
     except Exception, e:
        logger.warning('Unable to obtain mirror information: ' + str(e))
        logger.warning('gpupgradmirror exiting')
        exit(0)

     ''' Get a list of mirror nodes '''
     mirrorNodeCursor = p1MirrorInfo.getMirrorNodeList()     
     
     numMirrorHosts = 0
     for hostname in mirrorNodeCursor:
        numMirrorHosts = numMirrorHosts + 1
        mirrorhost = hostname
        
        ''' Create a phase2 object for each mirror node'''
        tempNode = phase2(dbUrl, logger, options.master_data_directory, mirrorhost)

        ''' Get info on mirror segments for this host'''
        mirrorInfoList = p1MirrorInfo.getMirrorInfo(mirrorhost)

        phase2MirrorList = []
        
        ''' Setup the mirror node '''
        tempNode.setup(mirrorInfoList, fileList, fileListDeleteBeforeCopy)
        phase2MirrorList.append(tempNode)
     conn.close()
    
     GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)

     """
       Start the threads 
     """
     for node in phase2MirrorList:
         node.start()
     for node in phase2MirrorList:
         node.join()                 
     
     """ Make sure all phase 2 threads complated successfully """
     p2Completed = True
     for node in phase2MirrorList:
         for segStatus in node.statusList:
             if segStatus.get_current_status()[0] != 'PHASE2_DONE':
                 p2Completed = False
                 break
         if p2Completed == False:
             break
         
     if p2Completed == False:
         logging.error('Unable to complete upgrade mirrors for phase 2')
         raise ValidationError('Unable to complate upgrade')
     else:
        phase1MirrorInfo.removeMirrorInfoFile()
        for node in phase2MirrorList:
            for segStatus in node.statusList:
                segStatus.remove_status_file()
  else:
    """
      Phase 1 
    """
    
    try:
       p1MirrorInfo = phase1MirrorInfo(dbUrl)
    except Exception, e:
        logger.warning('Unable to obtain mirror information: ' + str(e))
        logger.warning('gpupgradmirror exiting')
        traceback.print_exc()
        exit(0)

    ''' Get a list of mirror nodes '''
    mirrorNodeCursor = p1MirrorInfo.getMirrorNodeList()
       
    numMirrorHosts = 0
    for mirrorName in mirrorNodeCursor:
        numMirrorHosts = numMirrorHosts + 1
        
        ''' Create a gpupgrademirror object for each mirror node'''
        ''' KAS Note to self. In unsafe mode, we can do the copies in parallel by '''
        '''     Creating a gpupgrademirror for each segment not just each node  '''
        tempNode = gpupgrademirror(dbUrl, logger, options.master_data_directory, mirrorName)

        ''' Get info on mirror segments for this host'''
        mirrorInfoList = p1MirrorInfo.getMirrorInfo(mirrorName)
        
        ''' Setup the mirror node '''
        tempNode.setup(mirrorInfoList)
        MirrorNodeList.append(tempNode)

    if (options.rollback == True or options.continue_upgrade == True) and options.phase2 == False:
       pass
    else:
       GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)    

    """ 
      Start the threads 
    """
    for node in MirrorNodeList:
        node.start()
    for node in MirrorNodeList:
        node.join()    
    
    if options.rollback == False:
       p1Completed = True
       for node in MirrorNodeList:
           for segStatus in node.statusList:
               if segStatus.get_current_status()[0] != 'PHASE1_DONE':
                  p1Completed = False
                  break
           if p1Completed == False:
              break
    
       if p1Completed == False:
          logging.error('Unable to complete upgrade mirrors for phase 1')
          raise ValidationError('Unable to complete upgrade')
    else:
       ''' We are in rollback mode '''
       p1RollbackCompleted = True
       for node in MirrorNodeList:
           for segStatus in node.statusList:
               if    (options.mode == 'S' and segStatus.get_current_status()[0] != 'PHASE1_SAFE_MODE') \
                  or (options.mode == 'U' and segStatus.get_current_status()[0] != 'PHASE1_UNSAFE_MODE'):
                  p1RollbackCompleted = False
                  break
           if p1RollbackCompleted == False:
              break
    
       if p1RollbackCompleted == False:
          logging.error('Unable to complete rollback of upgrade mirrors for phase 1')
          raise ValidationError('Unable to complete rollback')
       else:
          phase1MirrorInfo.removeMirrorInfoFile()
          for node in MirrorNodeList:
              for segStatus in node.statusList:
                  segStatus.remove_status_file()
    
  sys.exit(0)

except Exception,e:
    logger.error("gpupgrademirror failed: %s \n\nExiting..." % e )
    traceback.print_exc()
    sys.exit(3)

except KeyboardInterrupt:
    # Disable SIGINT while we shutdown.
    signal.signal(signal.SIGINT,signal.SIG_IGN)


    # Re-enabled SIGINT
    signal.signal(signal.SIGINT,signal.default_int_handler)

    sys.exit('\nUser Interrupted')

except Exception, e:
  print "FATAL Exception: " + str(e)
  traceback.print_exc()
  sys.exit(1)


finally:
    try:
        if remove_pid:
            remove_pid_file(options.master_data_directory)
        try:
            GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)
        except Exception:
            pass         
    except NameError:
        pass
    logger.info("gpupgrademirror complete")
