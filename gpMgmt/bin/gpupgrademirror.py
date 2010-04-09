#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#

import copy
import datetime
import os
import sys
import signal
import traceback
import pprint
import threading
import tempfile
import bisect
from time import strftime, sleep
from gppylib.commands.base import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE


sys.path.append(os.path.join(os.path.dirname(__file__), "lib"))

import lib.pysync

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
FULL_EXECNAME = os.path.abspath( __file__ )

DESCRIPTION = ("""Upgrades 3.3.x.x mirrors to 4.0 mirrors.""")

_help  = [""" TODO add help """]

_usage = """ TODO add usage """

TEN_MEG = 10485760
ONE_GIG = 128  * TEN_MEG
TEN_GIG = 1024 * TEN_MEG

INFO_DIR = "gpupgrademirrorinfodir"
INFO_DIR_MINION = "gpupgrademirrorinfodirminion"

PID_FILE = "gpupgrademirror.pid"

GPUPGRADEMIRROR_TEMP_DIR = "gpupbrademirrortempdir"



#-------------------------------------------------------------------------------
def findRelFileDotNodes(relFileNode, sortedList):
    """
      This function will take a relfilenode without a . prefix, and fill all of its
      . prefix files. Just the prefixes are returned.
      relFileNode and sortedList as assumed to contain full path lists.
    """
    retValue = []
    relFileNodeWithDot = relFileNode + "."
    index = bisect.bisect_left(sortedList, relFileNode)
    index = index + 1

    ###print "relFileNode = " + relFileNode
    while index < len(sortedList) and sortedList[index].startswith(relFileNodeWithDot):
       fullName = sortedList[index]
       dotIndex = len(relFileNodeWithDot)
       suffix = fullName[dotIndex:]
       ###print "  suffix = " + str(suffix)
       retValue.append(suffix)
       index = index + 1
    
    return retValue


#-------------------------------------------------------------------------------
def makeRelFileNodeDictionary(theList):
    """ 
    This function will return a dictionary given a set of file names with a .* suffix.
    The key will be the file name without the .* suffix, and the value will be a list
    of files that have the .* suffix (e.g. { 1 : [1.1, 1.2, 1.3] }
    """
    retValue = {}
    
    for file in theList:
        dotIndex = file.rfind(".")
        fileWithoutSuffix = file[:dotIndex]
        filesList = retValue.get(fileWithoutSuffix, [])
        retValue[fileWithoutSuffix] = filesList.append(file)

    return retValue    



#-------------------------------------------------------------------------------
def sshBusy(cmd):
    """ 
      This funciton will check the results of a Command to see if ssh was too busy.
      It will return False if the command completed, successfully or not, 
      and a retry is not possible or necessary. 
    """
    retValue = False
    results = cmd.get_results()
    resultStr = results.printResult()
    if results.rc != 0:
       if resultStr.find("ssh_exchange_identification: Connection closed by remote host") != -1:
          retValue = True
       else:
          retValue = False
    else:
       retValue = False
    return retValue


#-------------------------------------------------------------------------------
def runAndCheckCommandComplete(cmd):
    """ 
      This function will run a Command and return False if ssh was too busy.
      It will return True if the command completed, successfully or not, 
      if ssh wasn't busy. 
    """
    retValue = True
    cmd.run(validateAfter = False)
    if sshBusy(cmd) == True:
       """ Couldn't make the connection. put in a delay, and return"""
       self.logger.debug("gpupgrademirror ssh is busy... need to retry the command: " + str(cmd))
       time.sleep(1)
       retValue = False
    else:
       retValue = True
    return retValue


#-------------------------------------------------------------------------------
                                                                
def parseargs():
    parser = OptParser( option_class = OptChecker
                      , description  = ' '.join(DESCRIPTION.split())
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
    parser.add_option('-i', '--ids', default='',
                      help='comma separated list of dbids for use in upgrade')
    parser.add_option('--usage', action="briefhelp")
    

    """
     Parse the command line arguments
    """
    (options, args) = parser.parse_args()

    if len(args) > 0:
        logger.error('Unknown argument %s' % args[0])
        parser.exit()

    try:
        if options.ids == None or len(options.ids) == 0:
           options.info_data_directory = get_gphome() + '/' + INFO_DIR
        else:
           options.info_data_directory = get_gphome() + '/' + INFO_DIR_MINION 
    except GpError, msg:
        logger.error(msg)
        parser.exit()

    if not os.path.exists(options.info_data_directory):
       MakeDirectory.local( name = 'gpupgrademirror make info dir: %s' % options.info_data_directory
                          , directory = options.info_data_directory
                          )
    return options, args

#-------------------------------------------------------------------------------
def sig_handler(sig, arg):
    print "Handling signal..."
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGHUP, signal.SIG_DFL)

    # raise sig
    os.kill(os.getpid(), sig)


#-------------------------------------------------------------------------------
def create_pid_file(info_data_directory):
    """Creates gpupgradmirror pid file"""
    try:
        fp = open(info_data_directory + '/' + PID_FILE, 'w')
        fp.write(str(os.getpid()))
    except IOError:
        raise
    finally:
        if fp: fp.close()


#-------------------------------------------------------------------------------
def remove_pid_file(info_data_directory):
    """Removes upgrademirror pid file"""
    try:
        os.unlink(info_data_directory + '/' + PID_FILE)
    except:
        pass


#-------------------------------------------------------------------------------
def is_gpupgrademirror_running(info_data_directory):
    """Checks if there is another instance of gpupgrademirror running"""
    is_running = False
    try:
        fp = open(info_data_directory + '/' + PID_FILE, 'r')
        pid = int(fp.readline().strip())
        fp.close()
        is_running = check_pid(pid)
    except IOError:
        pass
    except Exception, msg:
        raise

    return is_running




#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------

class InvalidStatusError(Exception): pass

#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class ValidationError(Exception): pass


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class GPHostCache:
    """ This class represents the information stored in the .gphostcache file """
    
    def __init__(self):
        self.hostcache = {}
        self.readfile()
        
    #-------------------------------------------------------------------------------
    def __str__(self):   
       tempStr = "self.hostcache = " + str(self.hostcache) + '\n'
       return tempStr
        
    #-------------------------------------------------------------------------------
    def readfile(self):
        FILEDIR   = os.path.expanduser("~")
        FILENAME  = ".gphostcache"
        CACHEFILE = FILEDIR + "/" + FILENAME

        try:
            file = open(CACHEFILE, 'r')
                
            for line in file:
                (interface,hostname) = line.split(':')
                self.hostcache[interface.strip()] = hostname.strip()                    
        except IOError:
            raise Exception(' file %s not found' % path)                    
        finally:
            file.close()        
    
    #-------------------------------------------------------------------------------
    def getInterfaces(self, hostName):
        retList = []
        
        for interface in self.hostcache:
            if self.hostcache[interface] == hostName:
               retList.append(interface)
        return retList
    
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
class RelFileNodes():
    """ This class represents a complete set of tables and dependencies for a given segment. """
    """ WARNING: This class isn't currently used, so it is not tested. Beware...."""

    def __init__(self, logger, mirrorInfo, database_list):
        self.info_data_directory = mirrorInfo.mirror_dbid
        self.mirrorInfo = mirrorInfo
        self.databaseList = database_list
        self.tableRelFileList = []
        
    #-------------------------------------------------------------------------------    
    def __str__(self): 
        retValue = ''
         
        for tableFileList in self.tableRelFileList:
            tempStr = ':'.join(tableFileList)
            retValue = retValue + tempStr + '\n'
        return retValue
        
#   #-------------------------------------------------------------------------------            
    def __repr__(self):
        return self.__str__()
        
    #-------------------------------------------------------------------------------
    def setup(self):
        
        """ Connect to each database on the segment, and get its list of tables and table dependencies. """
        for db in self.databaseList:
           if db.databaseName == "template0":
              continue
           segmentURL = dbconn.DbURL( hostname = self.mirrorInfo.primary_host
                                    , port = self.mirrorInfo.primary_host_port
                                    , dbname = db.databaseName 
                                    )
           conn   = dbconn.connect( dburl   = segmentURL
                                  , utility = True
                                  )
           
           tablesCursor = dbconn.execSQL(conn, TableRelFileNodesRow.query())
           aTable = []
           for row in tablesCursor:
               aTable = []
               """ KAS need to added code to look for *.1, *.2 files for this table """
               tableRelRow = TableRelFileNodesRow(row)
               aTable.append(str(db.databaseDirectory) + '/' + str(tableRelRow.table_file))
               if tableRelRow.toast_file != None:
                  aTable.append(str(db.databaseDirectory) + '/' + str(tableRelRow.toast_file))
               if tableRelRow.toast_index_file != None:
                  aTable.append(str(db.databaseDirectory) + '/' + str(tableRelRow.toast_index_file))
               if tableRelRow.ao_file != None:            
                  aTable.append(str(db.databaseDirectory) + '/' + str(tableRelRow.ao_file))
               if tableRelRow.ao_index_file != None:
                  aTable.append(str(db.databaseDirectory) + '/' + str(tableRelRow.ao_index_file))
               indexesCursor = dbconn.execSQL(conn, TableIndexRow.query(str(tableRelRow.table_file_oid)))
               """ KAS note to self. Do I need to worry about *.1, *.2... files? """
               for indexRow in indexesCursor:
                   indexEntry = TableIndexRow(indexRow)
                   aTable.append(str(db.databaseDirectory) + '/' + str(indexEntry.relfilenode))
               self.tableRelFileList.append(aTable)
           conn.close()

   #-------------------------------------------------------------------------------
    def _read_rel_file_node_file(self):
        """Reads in an existing relfile node file"""
        self.logger.debug("Trying to read in a pre-existing relfile node file" + str(self._rel_file_node_filename))
        try:
            self._fp = open(self._rel_file_node_filename, 'r')
            self._fp.seek(0)

            for line in self._fp:
                tableFileList = line.split(':')
                tableRelFileList.append(tableFileList)
                
        except IOErrormm, e:
           logger.error('Unable to read relfile node file: ' + str(e))
           logger.error('gpupgradmirror exiting')
           exit(1)

   #-------------------------------------------------------------------------------
    def create_rel_file_node_file(self):
        """Creates a new gpupgrademirror relfile node file"""
        try:                
            self._fp = open(self._rel_file_node_filename, 'w')
        except IOError, e:
            logging.error('Unable to create relfile node file %s.' % self._rel_file_node_filename)
            raise e

   #-------------------------------------------------------------------------------
    def remove_rel_file_node_file(self):
        """Closes and removes the gpupgrademirror relfile node file"""
        if self._fp:
            self._fp.close()
            self._fp = None
        if os.path.exists(self._rel_file_node_filename):
            os.unlink(self._rel_file_node_filename)


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------

class TableRelFileNodesRow:

    def __init__(self, row):
        self.table_file_oid   = row[0]
        self.table_file       = row[1]
        self.toast_file       = row[2]
        self.toast_index_file = row[3]
        self.ao_file          = row[4]
        self.ao_index_file    = row[5]
        
    #-------------------------------------------------------------------------------
    @staticmethod
    def query():    
        exeSql = """SELECT 
            t1.oid         AS table_file_oid 
          , t1.relfilenode AS table_file
          , t2.relfilenode AS toast_file
          , t3.relfilenode AS toast_index_file
          ,    t4.relfilenode AS ao_file
          ,    t5.relfilenode AS ao_index_file
           FROM    (((pg_class AS t1 left outer join pg_class AS t2 ON t2.oid = t1.reltoastrelid)
              left outer join  pg_class AS t3 ON t3.oid = t2.reltoastidxid)
                  left outer join pg_class AS    t4 ON t4.oid = t1.relaosegrelid)
                    left outer join pg_class AS t5 ON    t5.oid = t4.relaosegidxid
           WHERE   t1.relkind = 'r' or t1.relkind = 'x'
           ORDER BY t1.relfilenode
        """

        return exeSql


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------

class TableIndexRow:

    def __init__(self, row):
        self.relfilenode       = row[0]

        
#-------------------------------------------------------------------------------
    @staticmethod
    def query(table_oid):
        exeSql = """ SELECT t2.relfilenode
                     FROM   pg_catalog.pg_index AS t1
                             INNER JOIN
                            pg_catalog.pg_class AS t2
                     ON     t1.indexrelid = t2.oid
                     WHERE  t1.indrelid = """ + str(table_oid)

        return exeSql


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class MirrorNICRow:
    
    def __init__(self, row):
        self.nic = row[0]
        self.dbid = row[1]

    #-------------------------------------------------------------------------------
    @staticmethod
    def query():
        if options.phase2 == False:
           exeStr = '''  SELECT    hostname as nicaddress, dbid
                         FROM      pg_catalog.gp_configuration
                         WHERE     isprimary = 'f'
                         ORDER BY  hostname '''
        else:
           raise Exception('Internal Error. MirrorNICRow.qurey only for 3.x systems.')           
        return exeStr


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class MirrorHostsRow:
    
    def __init__(self, row):
        self.hostname = row[0]
        self.dbid     = row[1]

    #-------------------------------------------------------------------------------
    @staticmethod
    def query():
        if options.phase2 == False:
           raise Exception('Internal Error. MirrorHostsRow.qurey only for 3.3.x systems.')
        else:
           exeStr = ''' SELECT      hostname, dbid 
                        FROM        pg_catalog.gp_segment_configuration
                        WHERE       role = 'm'
                        ORDER BY    hostname '''
        return exeStr


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class MirrorHostInfoRow:
    
    def __init__(self, row):
        self.mirror_host            = row[0]
        self.mirror_host_port       = row[1]
        self.primary_host           = row[2]
        self.primary_host_port      = row[3]
        self.primary_data_directory = row[4]
        self.mirror_data_directory  = row[5]
        self.mirror_dbid            = row[6]
        self.primary_dbid           = row[7]
        if options.phase2 == True:
           self.nic_address = row[8]

    #-------------------------------------------------------------------------------
    def __repr__(self):
        return self.__str__()

    #-------------------------------------------------------------------------------
    def __str__(self):  
        tempStr = 'mirror_host='            + str(self.mirror_host)            + '\n' \
        +         'mirror_host_port='       + str(self.mirror_host_port)       + '\n' \
        +         'primary_host='           + str(self.primary_host)           + '\n' \
        +         'primary_host_port='      + str(self.primary_host_port)      + '\n' \
        +         'primary_data_directory=' + str(self.primary_data_directory) + '\n' \
        +         'mirror_data_directory='  + str(self.mirror_data_directory)  + '\n' \
        +         'mirror_dbid='            + str(self.mirror_dbid)            + '\n' \
        +         'primary_dbid='           + str(self.primary_dbid)           + '\n'
        if options.phase2 == True:
           tempStr = tempStr + 'nic_address='   + str(self.nic_address)        + '\n' 
        return tempStr

    #-------------------------------------------------------------------------------
    @staticmethod
    def readListFromFile(nvFileName):
        """ Static method to convert file with information in the __str__ method format """
        """ for this object into a list of tuples.                                      """
        retList = []
        
        logger.debug("Trying to read in a pre-existing gpupgrademirror MirrorHostInfoRow file. " + str(nvFileName))
        
        try:
            fp = open(nvFileName,'r')
            fp.seek(0)

            if options.phase2 == True:
              colsPerRow = 9
            else:
              colsPerRow = 8
            index = 0
            row = []
            for line in fp:
                (name, value) = line.strip().rsplit('=', 1)
                row.append(value)
                index = index + 1
                if index == colsPerRow:     
                   retList.append(MirrorHostInfoRow(row))
                   row = []
                   index = 0
            return retList
        except IOError:
            raise
        finally:
            try:
                if fp != None:
                   fp.close()
            except:
                pass
            

    #-------------------------------------------------------------------------------
    @staticmethod
    def writeListToFile(nvFileName, listOfRows):
        """ Static method to write out a list of tuples in the __str__ method format """

        logger.debug("Trying to write a list of MirrorHostInfoRow to a file. " + str(nvFileName))
       
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
                                     , t2.port     as mirror_host_port
                                     , t1.hostname as primary_host
                                     , t1.port     as primary_host_port
                                     , t1.datadir  as primary_data_directory
                                     , t2.datadir  as mirror_data_directory
                                     , t2.dbid     as mirror_dbid
                                     , t1.dbid     as primary_dbid
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
                                     , t2.port     as mirror_host_port
                                     , t1.hostname as primary_host
                                     , t1.port     as primary_host_port
                                     , t3.fselocation  as primary_data_directory
                                     , t4.fselocation  as mirror_data_directory
                                     , t2.dbid     as mirror_dbid
                                     , t1.dbid     as primary_dbid
                                     , t2.address as nic_address
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
class DatabaseRow:
    
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
class NonCatalogToastTablesRow:
    
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
class CatalogToastTablesRow:
    
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
class Phase1MirrorInfo:


    def __init__(self, url = None):
        self.dbUrl = url
        self.mirrorInfoFile = options.info_data_directory + '/' + 'gpupgrademirror.mirrorinfo'
        self.mirrorNodeList = []
        self.allMirrorInfoList = []
        self.gpHostCacheInfo = GPHostCache()
        conn = None
    
        if (options.rollback == True or options.continue_upgrade == True) and options.phase2 == False:
           """ We are in a rollback or continue state in Phase 1.           """
           """ Retrieve configuration information from previously saved file """
           self.allMirrorInfoList = MirrorHostInfoRow.readListFromFile(Phase1MirrorInfo.getMirrorInfoFile())

           """ for each NIC in the list of mirror NICs, make a list of distinct mirror nodes """
           for mirrorInfo in self.allMirrorInfoList:
               mirrorHostName = self.gpHostCacheInfo.getHost(mirrorInfo.mirror_host)
               if mirrorHostName in self.mirrorNodeList:
                  pass
               else:
                  self.mirrorNodeList.append(mirrorHostName)
        else:
           """ We are in normal Phase1 or normal Phase 2 (no rollback or continue) """
           conn   = dbconn.connect( dburl   = dbUrl
                                  , utility = True
                                  )
           
           ''' Get a list of mirror nodes '''
           if options.phase2 == True:
              mirrorNodeCursor = dbconn.execSQL(conn, MirrorHostsRow.query())
              for row in mirrorNodeCursor:
                  mirror = MirrorHostsRow(row)
                  if len(options.ids) == 0 or str(mirror.dbid) in options.ids:
                     if mirror.hostname not in self.mirrorNodeList:
                        self.mirrorNodeList.append(mirror.hostname)

                        ''' Get info on mirror segments for this host'''
                        mirrorSegInfoCursor = dbconn.execSQL(conn, MirrorHostInfoRow.query() % mirror.hostname)
                        for mirrorInfoRow in mirrorSegInfoCursor:
                            mirrorInfo = MirrorHostInfoRow(mirrorInfoRow)
                            if len(options.ids) == 0 or str(mirrorInfo.mirror_dbid) in options.ids:
                               self.allMirrorInfoList.append(mirrorInfo)
           else:
              ''' We are in phase1              '''
              ''' Get a list of all mirror NICs '''
              mirrorNicCursor = dbconn.execSQL(conn, MirrorNICRow.query())
           
              ''' Make a list of distinct nodes associated with the mirror NICs '''
              for row in mirrorNicCursor:
                  nicRow = MirrorNICRow(row)
                  host = self.gpHostCacheInfo.getHost(nicRow.nic)
                  if host in self.mirrorNodeList:
                     pass
                  else:
                     if len(options.ids) == 0 or str(nicRow.dbid) in options.ids:
                        self.mirrorNodeList.append(host)
           
              for nodeName in self.mirrorNodeList:
                  ''' for each node, use its NICs to find its segments '''
                  nicList = self.gpHostCacheInfo.getInterfaces(nodeName)
                  ''' Get info on mirror segments for this host'''
                  for nic in nicList:
                     mirrorSegInfoCursor = dbconn.execSQL(conn, MirrorHostInfoRow.query() % nic)
                     for mirrorInfoRow in mirrorSegInfoCursor:
                         mirrorInfo = MirrorHostInfoRow(mirrorInfoRow)
                         if len(options.ids) == 0 or str(mirrorInfo.mirror_dbid) in options.ids:
                            self.allMirrorInfoList.append(mirrorInfo)

           if conn != None:
              conn.close()  
            
           MirrorHostInfoRow.writeListToFile(Phase1MirrorInfo.getMirrorInfoFile(), self.allMirrorInfoList)   

    #-------------------------------------------------------------------------------
    def __str__(self):    
        tempStr = '  dbUrl                  = ' + str(self.dbUrl)             + '\n' \
        +         '  mirrorInfoFile         = ' + str(self.mirrorInfoFile)    + '\n' \
        +         '  self.mirrorNodeList    = \n' + str(self.mirrorNodeList)  + '\n' \
        +         '  self.allMirrorInfoList = ' + str(self.allMirrorInfoList) + '\n' \
        +         '  self.gpHostCacheInfo   = ' + str(self.gpHostCacheInfo)   + '\n'
        return tempStr 
        
    #-------------------------------------------------------------------------------
    def getAllMirrorInfoList(self):
        return self.allMirrorInfoList
        
    #-------------------------------------------------------------------------------
    def getMirrorNodeList(self):
        return self.mirrorNodeList
    
    
    #-------------------------------------------------------------------------------
    def getMirrorInfo(self, mirrorName):
        retList = []
        
        if options.phase2 == True:
           for mirrorSeg in self.allMirrorInfoList:
               if mirrorSeg.mirror_host == mirrorName:
                   retList.append(mirrorSeg)
        else:
           nicNameList = self.gpHostCacheInfo.getInterfaces(mirrorName)
           for nicName in nicNameList:
               for mirrorSeg in self.allMirrorInfoList:
                   if nicName == mirrorSeg.mirror_host:
                       retList.append(mirrorSeg)
        
        return retList

    #-------------------------------------------------------------------------------
    def getInterfaceList(self, node_name):
        retList = []
        
        if options.phase2 == True:
           for mirrorSeg in self.allMirrorInfoList:
               if mirrorSeg.mirror_host == node_name:
                  retList.append(mirrorSeg.nic_address)
        else:
           allHostCacheInterfaces = self.gpHostCacheInfo.getInterfaces(node_name)
           """ The host cache contains entries that are not actual nics, so """
           """ we must only return the real nics that are referenced from   """
           """ from the database cluster.                                   """                        
           for interface in allHostCacheInterfaces:
               for seg in self.allMirrorInfoList:
                   if seg.mirror_host not in retList and interface == seg.mirror_host:
                       retList.append(interface)
           
        return retList

    #-------------------------------------------------------------------------------
    @staticmethod
    def getMirrorInfoFile():
        return options.info_data_directory + '/' + 'gpupgrademirror.mirrorinfo'


    #-------------------------------------------------------------------------------
    @staticmethod
    def removeMirrorInfoFile():
        try:
           os.remove(Phase1MirrorInfo.getMirrorInfoFile())
        except:
           pass
           


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                    
class MakeDirectoryWithMode(Command):
    
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None,mode=None):        
        self.directory=directory
        if mode == None:
            pMode = ''
        else:
            pMode = "-m " + str(mode)
        cmdStr="%s -p %s %s" % (findCmdInPath('mkdir'),pMode,directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
    
    @staticmethod
    def local(name,directory,mode=None):    
        mkdirCmd=MakeDirectoryWithMode(name,directory,mode = mode)
        mkdirCmd.run(validateAfter=True)
            
    @staticmethod
    def remote(name,remote_host,directory,mode=None):
        mkdirCmd=MakeDirectoryWithMode(name,directory,ctxt=REMOTE,remoteHost=remote_host,mode=mode)
        mkdirCmd.run(validateAfter=True)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                    
class CreateDirIfNecessaryWithMode(Command):
    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None,mode=None):
        if mode == None:
           cmdStr="""python -c "import os; os.mkdir('%s')" """ % (directory)
        else:
           if len(str(mode)) != 4:
               raise Exception("Internal error: os.mkdir mode parameter must have length of 4")
           pythonPg = """\"\"\"import os
if os.path.exists('%s') == False:
  os.mkdir('%s', %s)\"\"\"
""" % (directory, directory, str(mode))
           cmdStr = """python -c %s""" % pythonPg
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        
    @staticmethod
    def remote(name,remote_host,directory,mode=None):
        cmd=CreateDirIfNecessaryWithMode(name,directory,ctxt=REMOTE,remoteHost=remote_host,mode=mode)
        cmd.run(validateAfter=False)
                

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class CopyToLocal(Command):
    def __init__(self, name, srcHost, srcDirectory, dstDirectory, ctxt=LOCAL):
        self.srcDirectory=srcDirectory
        self.srcHost=srcHost
        self.dstDirectory=dstDirectory
        cmdStr="%s -r %s:%s %s" % (findCmdInPath('scp'), srcHost, srcDirectory, dstDirectory)
        Command.__init__(self, name, cmdStr, ctxt=LOCAL)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                    
class RemoteCopyPreserve(Command):
    def __init__(self,name,srcDirectory,dstHost,dstDirectory,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory=srcDirectory
        self.dstHost=dstHost
        self.dstDirectory=dstDirectory        
        cmdStr="%s -rp %s %s:%s" % (findCmdInPath('scp'),srcDirectory,dstHost,dstDirectory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                    
class GPumDiskFree(Command):
    """ This method checks for available space in a directory   """
    """ The same method, without the GPum prefix is in gppylib, """
    """ but it doesn't work correctly in 3.3.x.                 """

    def __init__(self,name,directory,ctxt=LOCAL,remoteHost=None):
        self.directory=directory
        dfCommand = SYSTEM.getDiskFreeCmd()
        if options.phase2 == False and isinstance(SYSTEM, SolarisPlatform):
           """ This is a work around for a bug in 3.3.x (see MPP-6647) """
           dfCommand = dfCommand + 'k'
        if options.phase2 == False and isinstance(SYSTEM, LinuxPlatform):
           """ This is a work around for a bug in 3.3.x (see MPP-6647) """
           dfCommand = dfCommand + 'Pk'
        cmdStr="%s %s" % (dfCommand,directory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def get_size(name,remote_host,directory):
        dfCmd=GPumDiskFree( name = name
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
class CreateHardLink(Command):
    """ This class create a hard link or links. """

    def __init__(self, name, srcFile, softLink, ctxt = LOCAL, remoteHost = None, argsFile = None):
        if argsFile == None:
           self.srcFile = srcFile
           self.softLink = softLink
           cmdStr="%s %s %s" % (findCmdInPath('ln'), srcFile, softLink)
           Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        else:
           cmdStr="%s %s | %s -n 2 %s" % (findCmdInPath("cat"), argsFile, findCmdInPath("xargs"), findCmdInPath('ln'))
           Command.__init__(self,name,cmdStr,ctxt,remoteHost)
           

#-------------------------------------------------------------------------                                                                                            
#-------------------------------------------------------------------------                                                                                            
class GPumMoveDirectory(Command):
    """ This class moves a local directory."""

    def __init__(self,name,srcDirectory,dstDirectory,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory=srcDirectory
        self.dstDirectory=dstDirectory
        cmdStr="%s -f %s %s" % (findCmdInPath('mv'),srcDirectory,dstDirectory)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class GPumMoveDirectoryContents(Command):
    """ This class moves a local directory."""

    def __init__(self,name,srcDirectory,dstDirectory,ctxt=LOCAL,remoteHost=None):
        self.srcDirectory = srcDirectory
        self.srcDirectoryFiles = self.srcDirectory + "." + 'dirfilelist' 
        self.dstDirectory = dstDirectory
        ls = findCmdInPath("ls")
        cat = findCmdInPath("cat")
        xargs = findCmdInPath("xargs")
        mv = findCmdInPath("mv")
        cmdStr = "%s -1 %s > %s" % (ls, self.srcDirectory, self.srcDirectoryFiles)
        cmdStr = cmdStr + ";%s %s" % (cat, self.srcDirectoryFiles)
        cmdStr = cmdStr + " | %s -I xxx %s %s/xxx %s" % (xargs, mv, self.srcDirectory, self.dstDirectory)
        cmdStr = cmdStr + "; rm %s" % (self.srcDirectoryFiles)
        """
        ls -1 temp1 > temp1.list;cat temp1.list | xargs -I xxx mv temp1/xxx temp2
        cmdStr = "%s -1 %s > %s;%s %s | %s -I xxx %s %s/xxx %s"
        cmdStr="%s -f %s %s" % (findCmdInPath('mv'),srcDirectory,dstDirectory)
        """
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        
        
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class RemoveFileDirectoryFromList(Command):
    
    def __init__(self,name,fileListLocation,ctxt=LOCAL,remoteHost=None):
        self.fileListLocation = fileListLocation
        self.ctxt = ctxt
        self.remoteHost = remoteHost
        cmdStr="%s %s | %s %s -rf" % \
                    ( findCmdInPath("cat")
                    , fileListLocation
                    , findCmdInPath("xargs")
                    , findCmdInPath('rm')
                    )
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    def setupListFile(self, tempDir, theList):
        fd, fdName = tempfile.mkstemp(prefix='tmpRFDFL', dir=tempDir)

        fd = open(fdName, "w")
        for row in theList:
            fd.write(row +"\n")
        fd.close()
        rmCmd = RemoteCopy( name = "gpupgrademirror copy RFDFL: %s to %s:%s" % (fdName, self.remoteHost, self.fileListLocation)
                          , srcDirectory = fdName
                          , dstHost = self.remoteHost
                          , dstDirectory = self.fileListLocation
                          , ctxt = LOCAL
                          , remoteHost = None
                          )
        rmCmd.run(validateAfter=True)
        os.unlink(fdName)
                
#-------------create tar------------------
class CreateTarFromList(Command):
    def __init__(self, name, fileListLocation, dstTarFile, srcDirectory, ctxt=LOCAL, remoteHost=None):
        self.fileListLocation = fileListLocation
        self.dstTarFile = dstTarFile
        self.srcDirectory = srcDirectory
        self.ctxt = ctxt
        self.remoteHost = remoteHost
        tarCmd = SYSTEM.getTarCmd()
        cmdStr="%s %s | %s %s rvPpf %s -C %s  " % \
                       ( findCmdInPath("cat")
                       , fileListLocation
                       , findCmdInPath('xargs')
                       , tarCmd
                       , self.dstTarFile
                       , srcDirectory
                       )
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    def setupListFile(self, tempDir, theList):
        fd, fdName = tempfile.mkstemp(prefix='tmpCTFL', dir=tempDir)

        fd = open(fdName, "w")
        for row in theList:
            fd.write(row +"\n")
        fd.close()
        rmCmd = RemoteCopy( name = "gpupgrademirror copy CTFL: %s to %s:%s" % (fdName, self.remoteHost, self.fileListLocation)
                          , srcDirectory = fdName
                          , dstHost = self.remoteHost
                          , dstDirectory = self.fileListLocation
                          , ctxt = LOCAL
                          , remoteHost = None
                          )
        rmCmd.run(validateAfter=True)
        os.unlink(fdName)                

        
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class RemoteCopyPreserveFromList(Command):
    
    def __init__(self,name,fileListLocation,ctxt=LOCAL,remoteHost=None):
        self.fileListLocation = fileListLocation
        self.ctxt = ctxt
        self.remoteHost = remoteHost
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

               

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class FileDirectoryList(Command):
    """ This method gets list of files based on a pattern used by ls. """

    def __init__(self, name, filePattern, ctxt=LOCAL, remoteHost=None):
        self.filePattern = filePattern
        cmdStr="%s -1 %s" % (findCmdInPath('ls'), filePattern)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def get_list(name, remote_host, file_pattern):
        lsCmd = FileDirectoryList(name, filePattern = file_pattern, ctxt = REMOTE, remoteHost = remote_host)
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
class FindDotFiles(Command):
    """ Finds a file pattern"""
    
    _all_file_suffix = "allfiles"
    _search_file_suffix = "searchFiles"

    def __init__(self, name, topDirectory, filePrefixList, tempDirectory, segid, ctxt=LOCAL, remoteHost=None):
        self.name           = name
        self.topDirectory   = topDirectory
        self.filePrefixList = filePrefixList
        self.tempDirectory  = tempDirectory
        self.segid          = segid
        self.ctxt           = ctxt
        self.remoteHost     = remoteHost
        
        self.tempFile = tempDirectory + "/" + _all_file_suffix
        self.searchFile = tempDirectory + "/" + _search_file_suffix
        self.srcFile =  options.info_data_directory + "/" + _search_file_suffix + str(segid)
        
        find  = findCmdInPath('find')
        cat   = findCmdInPath('cat')
        grep  = findCmdInPath('grep')
        rm    = findCmdInPath('rm')
        
        cmdStr = '%s %s > %s;%s %s | %s -I xxx %s xxx.; rm %s' \
                              % ( find
                                , self.topDirectory
                                , self.tempFile
                                , cat
                                , self.searchFile
                                , xargs
                                , grep
                                , self.tempFile
                                )           
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    def setupSearchFile(self):
        """ This method sets up the file containing the original files. """
        srcFileFD = open(self.srcFile, 'w')
        for filePrefix in self.filePrefixList:
            srcFileFD.write(filePrefix + "\n")
        srcFileFD.close()
        
        rcCmd = RemoteCopy( name = "gpupgrademirror copy dot file source: %s to %s:%s" % (self.srcFile, self.remoteHost, self.searchFile)
                          , srcDirectory = self.srcFile
                          , dstHost = self.remoteHost
                          , dstDirectory = self.tempFile
                          , ctxt = LOCAL
                          , remoteHost = None
                          )
        rcCmd.run(validateAfter = True)
        
    def removeSearchFile(self):
        rmCmd = RemoveDirectory( name = "gpupgrademirror remove dot file: %s:%s" % (self.remoteHost, self.searchFile)
                               , directory = self.tempFile
                               , ctxt = REMOTE
                               , remoteHost = self.remoteHost
                               )
                                   
    def get_result_list(self):
        retList = []
        retList = self.results.stdout.split('\n')
        if retList != None and len(retList) > 0 and retList[-1] == '':
           ''' Remove any trailing empty string '''
           retList.pop()
        return retList

    def resolveIntoSourceTargetList(self): 
        retList = []
        

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class Find(Command):
    """ Finds a file pattern"""

    def __init__(self, name, directory, filePattern, ctxt=LOCAL, remoteHost=None):
        self.directory = directory
        self.filePattern = filePattern
        find  = findCmdInPath('find')
        cmdStr= '%s %s -name "%s"' % (find, directory, filePattern)            
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    
    def get_result_list(self):
        retList = []
        retList = self.results.stdout.split('\n')
        if retList != None and len(retList) > 0 and retList[-1] == '':
           ''' Remove any trailing empty string '''
           retList.pop()
        return retList
    

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class FilesInDir(Command):
    """ This method gets a list of files in a directory """

    def __init__(self, name, filePattern, ctxt=LOCAL, remoteHost=None, remoteTempFile=None):
        self.filePattern = filePattern
        self.remoteHost = remoteHost
        self.remoteTempFile = remoteTempFile
        if remoteTempFile != None:
           remoteTempFilePart = " > %s" % remoteTempFile
        else:
           remoteTempFilePart = ""
        find  = findCmdInPath('find')
        xargs = findCmdInPath('xargs')
        du    = findCmdInPath('du') 
        sort  = findCmdInPath('sort')
        cmdStr= "%s %s -type f %s" % (find, filePattern, remoteTempFilePart)
        self.cmdStr = cmdStr
        ###print "*** cmdStr = " + str(cmdStr)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        ###print "after call too **** Command.__init__ ***"
    
    def get_result_list(self, localTempFile = None):
        retList = []

        if localTempFile != None:
           rcCmd = CopyToLocal( name = "gpupgrademirror get result list copy: %s:%s to %s" % (self.remoteHost, self.remoteTempFile, localTempFile)
                           , srcHost = self.remoteHost
                           , srcDirectory = self.remoteTempFile
                           , dstDirectory = localTempFile
                           )
           ###print "*** rcCmd = " + str(rcCmd)
           rcCmd.run(validateAfter = True)
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove remote temp file: %s:%s' % (self.remoteHost, self.remoteTempFile)
                              , directory = self.remoteTempFile
                              , ctxt = REMOTE
                              , remoteHost = self.remoteHost
                              )
           rmCmd.run(validateAfter = True)

           fd = open(localTempFile, "r")
           fd.seek(0)
           fileList = []
           for line in fd:
               line = line.rstrip('\n')
               fileList.append(line)
           fd.close()
        else:
           fileList = self.results.stdout.split('\n')

        """
        print ""
        print str(self.cmdStr)
        print "fileList = " + str(fileList) 
        print ""
        print 
        """
        
        """
        print ""
        print str(self.cmdStr)
        print "fileList = " 
        for sdd in fileList:
            print str(sdd)
        """

        if fileList != None and len(fileList) > 0 and fileList[-1] == '':
           ''' Remove any trailing empty string '''
           fileList.pop()
        retList = fileList
 
        return retList

 

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class FileAndSize(Command):
    """ This method gets a list of files in a directory and their sizes in k-bytes """

    def __init__(self, name, filePattern, ctxt=LOCAL, remoteHost=None, zeroSize=False, remoteTempFile=None):
        self.filePattern = filePattern
        self.remoteHost = remoteHost
        self.remoteTempFile = remoteTempFile
        if remoteTempFile != None:
           remoteTempFilePart = " > %s" % remoteTempFile
        else:
           remoteTempFilePart = ""
        if zeroSize == True:
           sizeArg = "-size 0c"
        else:
           sizeArg = ''
        find  = findCmdInPath('find')
        xargs = findCmdInPath('xargs')
        du    = findCmdInPath('du') 
        sort  = findCmdInPath('sort')
        cmdStr= "%s %s -type f %s | %s %s -k | %s -n %s" % (find, filePattern, sizeArg, xargs, du, sort, remoteTempFilePart)
        self.cmdStr = cmdStr
        ###print "*** cmdStr = " + str(cmdStr)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
        ###print "after call too **** Command.__init__ ***"
    
    def get_result_list(self, localTempFile = None):
        retList = []

        if localTempFile != None:
           rcCmd = CopyToLocal( name = "gpupgrademirror get result list copy: %s:%s to %s" % (self.remoteHost, self.remoteTempFile, localTempFile)
                           , srcHost = self.remoteHost
                           , srcDirectory = self.remoteTempFile
                           , dstDirectory = localTempFile
                           )
           ###print "*** rcCmd = " + str(rcCmd)
           rcCmd.run(validateAfter = True)
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove remote temp file: %s:%s' % (self.remoteHost, self.remoteTempFile)
                              , directory = self.remoteTempFile
                              , ctxt = REMOTE
                              , remoteHost = self.remoteHost
                              )
           rmCmd.run(validateAfter = True)

           fd = open(localTempFile, "r")
           fd.seek(0)
           fileList = []
           for line in fd:
               fileList.append(line)
           fd.close()
        else:
           fileList = self.results.stdout.split('\n')

        """
        print ""
        print str(self.cmdStr)
        print "fileList = " + str(fileList) 
        print ""
        print 
        """
        
        """
        print ""
        print str(self.cmdStr)
        print "fileList = " 
        for sdd in fileList:
            print str(sdd)
        """

        if fileList != None and len(fileList) > 0 and fileList[-1] == '':
           ''' Remove any trailing empty string '''
           fileList.pop()
        for file in fileList:
            sizef, filef = file.split()
            sizef = sizef.strip()
            filef = filef.strip()
            ###print "sizef = " + str(sizef)
            ###print "filef = >>>" + str(filef) + "<<<"
            retList.append([sizef, filef])
        return retList

    
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------                                                       
class DirectoryList(Command):
    """ This method gets a list of top level directories in a directory  """

    def __init__(self, name, dirLocation, ctxt=LOCAL, remoteHost=None):
        self.dirLocation = dirLocation
        find  = findCmdInPath('find')
        sort  = findCmdInPath('sort')
        cmdStr= "%s %s -type d | %s " % (find, dirLocation, sort)         
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def get_list(name, remote_host, dir_location):
        theCmd = DirectoryList(name, dirLocation = dir_location, ctxt = REMOTE, remoteHost = remote_host)
        theCmd.run(validateAfter=False)
        return theCmd.get_result_list()
    
    def get_result_list(self):
        dirList = self.results.stdout.split('\n')
        if dirList != None and len(dirList) > 0 and dirList[-1] == '':
           ''' Remove any trailing empty string '''
           dirList.pop()
        if dirList != None and len(dirList) > 0:
            ''' The first element is the dirLcation itself. Remove it'''
            dirList.pop(0)
        return dirList 
    
    
class SpecialFileHandling():
    
    def __init__(self, sourceList, seg):
        self.sourceList = sourceList
        self.seg = seg
        self.fullPathLinkDir = seg.tempDir + "/speciallinkdir"

        
    def createLinks(self):
        """ 
        Create a file containing a list of ln parameters locally, and the copy it 
                 to the primary node where we will run ln and create the links.
        """
        linkFile = "gpupgrademirror%s.linkspecialfile" % str(self.seg.primary_dbid)
        sourceFullPathLinkFile = options.info_data_directory + "/" + linkFile
        targetFullPathLinkFile = self.seg.tempDir + "/" + linkFile
        linkFP = open(sourceFullPathLinkFile, 'w')
        linkFP.seek(0)
        for sfile in self.sourceList:
            fullPathSrcFile = self.seg.primary_data_directory + "/" + sfile
            fullPathTargetFile = self.fullPathLinkDir + "/" + sfile
            linkFP.write("%s %s\n" % (fullPathSrcFile, fullPathTargetFile))
        linkFP.flush()
        linkFP.close()

        """ Copy the link file to the primary temp dir """
        cmdComplete = False
        while cmdComplete == False:
          linkCopy = RemoteCopyPreserve( name = "gpupgrademirror copy ln file for primary: %s:%s" % (self.seg.primary_host, self.fullPathLinkDir)
                                       , srcDirectory = sourceFullPathLinkFile
                                       , dstHost = self.seg.primary_host
                                       , dstDirectory = targetFullPathLinkFile
                                       , ctxt = LOCAL
                                       )
          cmdComplete = runAndCheckCommandComplete(linkCopy)
        linkCopy.validate()

        """ Create a directory to store all our links to the actual files """
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror special file link dir: %s:%s' % (self.seg.primary_host, self.fullPathLinkDir)
                             , directory = self.fullPathLinkDir
                             , ctxt = REMOTE
                             , remoteHost = self.seg.primary_host
                             , mode = 700
                             )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()

        """ Create any subdirectories for our links """
        subDirList = []
        for candidate in self.sourceList:
            candidateList = candidate.split("/")
            if len(candidateList) > 1:
               dirPart = "/".join(candidateList[:-1])
               if dirPart not in subDirList:
                  subDirList.append(dirPart)
        for subDir in subDirList:
            fullPathSubDir = self.fullPathLinkDir + "/" + subDir
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror special file link dir: %s:%s' % (self.seg.primary_host, fullPathSubDir)
                             , directory = fullPathSubDir
                             , ctxt = REMOTE
                             , remoteHost = self.seg.primary_host
                             , mode = 700
                             )
               cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()

        """ Create the links """               
        cmdComplete = False
        while cmdComplete == False:
            link = CreateHardLink( name = "gpupgrademirror creating links for special files %s:%s." % (self.seg.primary_host, str(self.seg.primary_data_directory))
                                    , srcFile = None
                                    , softLink = None
                                    , ctxt = REMOTE
                                    , remoteHost = self.seg.primary_host
                                    , argsFile = targetFullPathLinkFile
                                    )
            cmdComplete = runAndCheckCommandComplete(link)
        link.validate()

        cmdComplete = False
        while cmdComplete == False:
            rmCmd = RemoveFiles( name = 'gpupgrademirror remove links file: %s' % (targetFullPathLinkFile)
                                  , directory = targetFullPathLinkFile
                                  , ctxt = REMOTE
                                  , remoteHost = self.seg.primary_host
                                  )
            cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()              
        
    
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------    
class SegHardLinks(Command):
    """ 
      This class will generate groups of hard links to files in a segment directory.
      The overall design is to create a new temporary directory "softbase", which
      will contain a many to one relationship between its sub-directories and
      the real database directories (e.g. softbase/1.1, softbase/1.2 both contian
      links that point to base/1 files).
      
      We will special case any file that has zero length. This is becau
      
    """
    
    _max_files_per_pysync = 500
    _max_size_kb_per_pysync  = 50000000

    def __init__(self, mirror_info, logger):
        logger.debug("in SegHardLinks__init__() %s:%s primary seg id: %s" % (mirror_info.primary_host, mirror_info.primary_data_directory, str(mirror_info.primary_dbid)))
        self.logger = logger
        self.theMirror = mirror_info
        self.softBaseDir = self.theMirror.primary_data_directory + '/softbase'
        
    #-------------------------------------------------------------------------------
    def createLinks(self):
        self.logger.debug('in SegHardLinks.createLinks(). Primary seg id %s' % str(self.theMirror.primary_dbid))
        cmdComplete = False
        while cmdComplete == False:
           baseDir = DirectoryList( name = "gpupgrademirror get directory list under base for primary seg: " + str(self.theMirror.primary_dbid)
                                  , dirLocation = self.theMirror.primary_data_directory + '/base' 
                                  , ctxt = REMOTE
                                  , remoteHost = self.theMirror.primary_host
                                  )
           cmdComplete = runAndCheckCommandComplete(baseDir)
        baseDir.validate()   
        dirResultList = baseDir.get_result_list()
        
        """ Remove any reference to a dir called pgsql_tmp, which may exist in some database directories. """
        for aDir in dirResultList:
           dirSuffixIndex = aDir.rfind("/")
           candidateDir = aDir[dirSuffixIndex + 1:].rstrip()
           if candidateDir == "pgsql_tmp":
              dirResultList.remove(aDir)

        
        """ Create a directory to store all our links to the actual files """
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror make soft base: %s' % self.softBaseDir
                             , directory = self.softBaseDir
                             , ctxt = REMOTE
                             , remoteHost = self.theMirror.primary_host
                             , mode = 700
                             )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()
        
        for dir in dirResultList:
            """ For each database directory """
            self.logger.debug("working on dir %s:%s" % (self.theMirror.primary_host, str(dir)))
            
            """ Get a list of all files in the dir and their sizes in k-bytes """
            cmdComplete = False
            while cmdComplete == False:
               dirFileSizeAll = FileAndSize( name = 'gpupgradmirror get list of files and size in database dir'
                                     , filePattern = str(dir)
                                     , ctxt = REMOTE
                                     , remoteHost = self.theMirror.primary_host
                                     , remoteTempFile = self.theMirror.primary_data_directory + '/softbase/tempfileandsize'
                                     )
               cmdComplete = runAndCheckCommandComplete(dirFileSizeAll)
            dirFileSizeAll.validate()
            dbFileListAll = dirFileSizeAll.get_result_list(localTempFile = options.info_data_directory + "/tempfileandsize" + str(self.theMirror.primary_dbid))
            
            """ Get a sub-list of files that have zero length """ 
            cmdComplete = False
            while cmdComplete == False:
               dirFileSizeZero = FileAndSize( name = 'gpupgradmirror get list of files and size in database dir'
                                     , filePattern = str(dir)
                                     , ctxt = REMOTE
                                     , remoteHost = self.theMirror.primary_host
                                     , zeroSize = True
                                     , remoteTempFile = self.theMirror.primary_data_directory + '/softbase/tempfileandsize'
                                     )
               cmdComplete = runAndCheckCommandComplete(dirFileSizeZero)
            dirFileSizeZero.validate()           
            dbFileListZero = dirFileSizeZero.get_result_list(localTempFile = options.info_data_directory + "/tempfileandsize" + str(self.theMirror.primary_dbid))
            dbFileListZeroLength = len(dbFileListZero) 
               
            dbFileListZeroName = []
            for iii in range(len(dbFileListZero)):
                dbFileListZeroName.append(dbFileListZero[iii][1])
            
            """ Divide the two lists into distinct sub-lists """
            dbFileNonZeroList = []
            for candidate in dbFileListAll:
                if candidate[0] == str(0):
                   """ size is less than 1k """
                   if candidate[1] not in dbFileListZeroName:
                      dbFileNonZeroList.append(candidate)
                else:
                   dbFileNonZeroList.append(candidate)

            """ Recombine the lists, putting the zero length file list on the front """
            dbFileList = dbFileListZero + dbFileNonZeroList

            newDirSuffix = 0
            fileIndex    = 0
            dirIndex = dir.rfind('/')
            theDir = dir[dirIndex + 1:]
            while fileIndex < len(dbFileList):
               """ For each file in the database directory """
               dirPlusSuffix = self.softBaseDir + "/" + str(theDir) + "." + str(newDirSuffix)
               
               cmdComplete = False
               while cmdComplete == False:
                  rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror make soft base sub-database dir: %s' % dirPlusSuffix
                                    , directory = dirPlusSuffix
                                    , ctxt = REMOTE
                                    , remoteHost = self.theMirror.primary_host
                                    , mode = 700
                                    )
                  cmdComplete = runAndCheckCommandComplete(rmCmd)
               rmCmd.validate()               
               numFiles    = 0
               currentSize = 0
               srcFileList = []
               softLinkList = []
               while  (newDirSuffix == 0 and fileIndex < dbFileListZeroLength) \
                      or \
                      (   newDirSuffix != 0 and fileIndex < len(dbFileList) \
                          and numFiles < self._max_files_per_pysync \
                          and currentSize < self._max_size_kb_per_pysync \
                      ):
                  """ Make groups of the files and create links to the grouped files """
                  sizeSourceFile     = dbFileList[fileIndex][0]
                  fullPathSourceFile = dbFileList[fileIndex][1]
                  subfileIndex = fullPathSourceFile.rfind('/')
                  theFile = fullPathSourceFile[subfileIndex + 1:]
                  sLink = dirPlusSuffix + "/" + theFile
                  srcFileList.append(fullPathSourceFile)
                  softLinkList.append(sLink)
                  numFiles = numFiles + 1
                  currentSize = currentSize + int(sizeSourceFile)
                  fileIndex = fileIndex + 1
                  if newDirSuffix == 0 and fileIndex == dbFileListZeroLength:
                     zeroFilesFlag = False
               
               """ 
                 Create a file containing a list of ln parameters locally, and the copy it 
                 to the primary node where we will run ln and create the links.
               """
               linkFile = "gpupgrademirror%s.linkfile" % str(self.theMirror.primary_dbid)       
               sourceFullPathLinkFile = options.info_data_directory + "/" + linkFile
               targetFullPathLinkFile = self.softBaseDir + "/" + linkFile
               linkFP = open(sourceFullPathLinkFile, 'w')
               linkFP.seek(0)
               for lnIndex in range(len(srcFileList)):
                   linkFP.write("%s %s\n" % (srcFileList[lnIndex], softLinkList[lnIndex]))
               linkFP.flush()
               linkFP.close()
               cmdComplete = False
               while cmdComplete == False:
                  linkCopy = RemoteCopyPreserve( name = "gpupgrademirror copy ln file for primary dbid: " + str(self.theMirror.primary_dbid)
                                       , srcDirectory = sourceFullPathLinkFile
                                       , dstHost = self.theMirror.primary_host
                                       , dstDirectory = targetFullPathLinkFile
                                       , ctxt = LOCAL
                                       )
                  cmdComplete = runAndCheckCommandComplete(linkCopy)
               linkCopy.validate()
               
               cmdComplete = False
               while cmdComplete == False:
                  link = CreateHardLink( name = "gpupgrademirror creating links for dbid %s directory %s ." % (self.theMirror.primary_dbid, str(newDirSuffix))
                                    , srcFile = srcFileList
                                    , softLink = softLinkList
                                    , ctxt = REMOTE
                                    , remoteHost = self.theMirror.primary_host
                                    , argsFile = targetFullPathLinkFile
                                    )
                  cmdComplete = runAndCheckCommandComplete(link)
               link.validate()

               cmdComplete = False
               while cmdComplete == False:
                  rmCmd = RemoveFiles( name = 'gpupgrademirror remove links file: %s' % (targetFullPathLinkFile)
                                  , directory = targetFullPathLinkFile
                                  , ctxt = REMOTE
                                  , remoteHost = self.theMirror.primary_host
                                  )
                  cmdComplete = runAndCheckCommandComplete(rmCmd)
               rmCmd.validate()              
               newDirSuffix = newDirSuffix + 1
        
    #-------------------------------------------------------------------------------    
    def removeLinks(self):
        self.logger.debug("in SegHardLinks.removeLinks %s:%s" % (self.theMirror.primary_host, self.softBaseDir))
        
        cmdComplete = False
        while cmdComplete == False:
           cmd = RemoveFiles( name = 'gpupgrademirror remove softbase dir'
                         , remoteHost = self.theMirror.primary_host 
                         , directory = self.softBaseDir
                         , ctxt = REMOTE
                         )
           cmdComplete = runAndCheckCommandComplete(cmd)
        cmd.validate()
 
    #-------------------------------------------------------------------------------    
    def getHardDirList(self):
        """ 
          Returns a list of the fully qualified link dirs
          (e.g. [ '/db1/seg1/g0/base/2.1' , '/db1/seg1/g0/base/2.2' ,...] )
        """
        retList = [] 
        cmdComplete = False
        while cmdComplete == False:
           baseDir = DirectoryList( name = "gpupgrademirror get directory list under base for primary seg: " + str(self.theMirror.primary_dbid)
                               , dirLocation = self.softBaseDir 
                               , ctxt = REMOTE
                               , remoteHost = self.theMirror.primary_host
                               )
           cmdComplete = runAndCheckCommandComplete(baseDir)
        baseDir.validate()
        retList = baseDir.get_result_list()
        return retList
 
    #-------------------------------------------------------------------------------    
    @staticmethod
    def getUnqualifiedLinkDir(qualifiedDir):
        retValue = ''        
        startIndex = qualifiedDir.rfind("/")
        retValue = qualifiedDir[startIndex +1:]
        return retValue
 
     #-------------------------------------------------------------------------------    
    @staticmethod
    def getUnqualifiedRealDir(qualifiedDir):
        retValue = ''       
        startIndex = qualifiedDir.rfind("/")
        endIndex = qualifiedDir.rfind(".")
        retValue = qualifiedDir[startIndex +1:endIndex]       
        return retValue
 
#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class PySyncPlus(PySync):

    def __init__(self,name,srcDir,dstHost,dstDir,ctxt=LOCAL,remoteHost=None, options=None):
        self.namePlus = name
        self.srcDirPlus = srcDir
        self.dstHostPlus = dstHost
        self.dstDirPlus = dstDir
        self.ctxtPlus = ctxt
        self.remoteHostPlus = remoteHost
        self.optionsPlus = options
        PySync.__init__( self
                       , name = name
                       , srcDir = srcDir
                       , dstHost = dstHost
                       , dstDir = dstDir
                       , ctxt = ctxt
                       , remoteHost = remoteHost
                       , options = options 
                       )
        self.destinationHost = dstHost


#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class GPUpgradeMirrorMinion(Command):
    """ This class represents a gpugprademirror process. """

    def __init__(self, name, options, segs, ctxt=LOCAL, remoteHost=None):
        self.name = name
        if options != None:
           self.options = options
        else:
            options = ""
        self.segList = segs
        self.ctxt = ctxt
        self.remoteHost = remoteHost
        tempPythonPath = "PYTHONPATH=$PYTHONPATH:$GPHOME/bin"
        self.gpupgrademirror_executable = "$GPHOME/" + INFO_DIR_MINION + "/gpupgrademirror.py"
        self.cmdStr = "%s %s %s" % ( self.gpupgrademirror_executable
                                   , self.options
                                   , segs
                                   )
        self.cmdStr = tempPythonPath + ";" + self.cmdStr
        Command.__init__( self
                        , name = self.name
                        , cmdStr = self.cmdStr
                        , ctxt = self.ctxt
                        , remoteHost = self.remoteHost
                        )

    #-------------------------------------------------------------------------------
    def createInfoDirectory(self):
        """ Create a info directory for the minion """
        minionDir = "$GPHOME/" + INFO_DIR_MINION
        mkDirCmd = CreateDirIfNecessaryWithMode( name = 'Create the minion info dir: %s:%s' % (self.remoteHost, minionDir)
                             , directory = minionDir
                             , ctxt = REMOTE
                             , remoteHost = self.remoteHost
                             , mode = '0700'
                             )
        mkDirCmd.run(validateAfter=True)
    #-------------------------------------------------------------------------------
    def copyGpupgrademirrorToHost(self):
        source = FULL_EXECNAME
        target = self.gpupgrademirror_executable
        cpCmd = RemoteCopyPreserve( name = 'gpupgrademirror copy gpupgrademirror to minion : %s:%s' % (self.remoteHost, target)
                                  , srcDirectory = source
                                  , dstHost = self.remoteHost
                                  , dstDirectory = target
                                  , ctxt = LOCAL
                                  , remoteHost = None
                                  )
        cpCmd.run(validateAfter=True)

    #-------------------------------------------------------------------------------
    def copyStatusFileToHost(self):
        source = options.info_data_directory + "/*"
        target = "$GPHOME/" + INFO_DIR_MINION
        cpCmd = RemoteCopyPreserve( name = 'gpupgrademirror copy info dir to minion : %s:%s' % (self.remoteHost, target)
                                  , srcDirectory = source
                                  , dstHost = self.remoteHost
                                  , dstDirectory = target
                                  , ctxt = LOCAL
                                  , remoteHost = None
                                  )
        cpCmd.run(validateAfter=True)
        
        pidFileLoc = target + "/" + PID_FILE
        rmCmd = RemoveFiles( name = "gpupgrademirror remove the pid file %s:%s" % (self.remoteHost, pidFileLoc)
                           , remoteHost = self.remoteHost
                           , directory = pidFileLoc
                           , ctxt = REMOTE
                           )
        rmCmd.run(validateAfter=True)

    #-------------------------------------------------------------------------------
    def removeGpupgradeMirror(self):
        target = self.gpupgrademirror_executable
        rmCmd = RemoveFiles( name = "gpupgrademirror remove the gpupgrademirror file %s:%s" % (self.remoteHost, target)
                           , remoteHost = self.remoteHost
                           , directory = target
                           , ctxt = REMOTE
                           )
        rmCmd.run(validateAfter=True)

#-------------------------------------------------------------------------
#-------------------------------------------------------------------------
class GpUpgradeMirrorStatus():
    """Class that manages gpupgrademirror status file.

    The status file is placed in the master data directory on the master 
    It's used to keep track of where we are in the progression.
    
    There is one status file per mirror segment, and one overall status file.
    Each status file has the segment dbid as part of its suffix.
    """
    _safeModeStatusValues = { 'PHASE1_SAFE_MODE'            : 2
                            , 'START_SEG_SETUP'             : 3
                            , 'END_SEG_SETUP'               : 4
                            , 'START_PRIMARY_COPY'          : 5
                            , 'END_PRIMARY_COPY'            : 6
                            , 'START_NEW_MIRROR_FILE_FIXUP' : 7 
                            , 'END_NEW_MIRROR_FILE_FIXUP'   : 8
                            , 'START_MOVE_OLD_MIRROR'       : 9
                            , 'END_MOVE_OLD_MIRROR'         : 10
                            , 'START_MOVE_NEW_MIRROR'       : 11
                            , 'END_MOVE_NEW_MIRROR'         : 12
                            , 'START_REMOVE_OLD_MIRROR'     : 13
                            , 'END_REMOVE_OLD_MIRROR'       : 14
                            , 'PHASE1_DONE'                 : 15
                            , 'START_PHASE2'                : 16
                            , 'PHASE2_DONE'                 : 17
                            }

    _unsafeModeStatusValues  = { 'PHASE1_UNSAFE_MODE'                   : 2
                                , 'START_SEG_SETUP'                     : 3
                                , 'END_SEG_SETUP'                       : 4
                                , 'START_MOVE_OLD_MIRROR_SPECIAL_FILES' : 5
                                , 'END_MOVE_OLD_MIRROR_SPECIAL_FILES'   : 6
                                , 'START_REMOVE_OLD_MIRROR'             : 7
                                , 'END_REMOVE_OLD_MIRROR'               : 8
                                , 'START_PRIMARY_COPY'                  : 9
                                , 'END_PRIMARY_COPY'                    : 10
                                , 'START_NEW_MIRROR_FILE_FIXUP'         : 11
                                , 'END_NEW_MIRROR_FILE_FIXUP'           : 12
                                , 'START_MOVE_NEW_MIRROR'               : 13
                                , 'END_MOVE_NEW_MIRROR'                 : 14
                                , 'PHASE1_DONE'                         : 15
                                , 'START_PHASE2'                        : 16
                                , 'PHASE2_DONE'                         : 17
                                }
    
    """ Note, Both safe and un-safe mode values are valid, therefore their values are the same '2' """
    _overallStatusValues = { 'PHASE1_SAFE_MODE'       : 2
                           , 'PHASE1_UNSAFE_MODE'     : 2
                           , 'START_SETUP'            : 3
                           , 'SETUP_DONE'             : 4
                           , 'MINION_START_SETUP'     : 5
                           , 'MINION_SETUP_DONE'      : 6
                           , 'PHASE1_DONE'            : 7
                           , 'START_PHASE2'           : 8
                           , 'PHASE2_DONE'            : 9
                           }
    
    
    
    def __init__(self, logger, info_data_directory, segment_dbid, phase, overall = False):
        logger.debug("in __init__ for GpUpgradeMirrorStatus. dbid = %s" % str(segment_dbid))
        self.logger = logger
        self.overall = overall
        
        if overall == True:
           self._status_values = self._overallStatusValues
        elif options.mode == 'S':
           self._status_values = self._safeModeStatusValues
        else:
           self._status_values = self._unsafeModeStatusValues
            
        self._status = []
        self._status_info = []
        self._info_data_directory = info_data_directory
        self._phase = phase
        self.segment_dbid = segment_dbid
        self._status_filename = info_data_directory + '/gpupgrademirror' + str(segment_dbid) + '.status'
        self._fp = None
        self._fp_standby = None
        self._temp_dir = None
        self._input_filename = None
        self._original_primary_count = None
        self._gp_configuration_backup = None

        if os.path.exists(self._status_filename):
            self._read_status_file()
        logger.debug("__init__ for GpUpgradeMirrorStatus complete. dbid = %s" % str(segment_dbid))


   #-------------------------------------------------------------------------------
    def _read_status_file(self):
        """Reads in an existing gpupgrademirror status file"""
        self.logger.debug("Trying to read in a pre-existing gpupgrademirror status file %s" % (self._status_filename))
        try:
            self._fp = open(self._status_filename, 'a+')
            self._fp.seek(0)

            for line in self._fp:
                (status, status_info) = line.rstrip().split(':')
                ''' Determine if the previous run was phase 1 and was in safe or un-safe mode '''
                ''' re-set the mode based on the result'''
                if status == 'PHASE1_SAFE_MODE':
                    options.mode = 'S'
                    if self.overall == False:
                       self._status_values = self._safeModeStatusValues
                elif status == 'PHASE1_UNSAFE_MODE':
                    options.mode = 'U'
                    if self.overall == False:
                       self._status_values = self._unsafeModeStatusValues
                elif status == 'START_PHASE2':
                    options.phase2 = True
                self._status.append(status)
                self._status_info.append(status_info)
        except Exception, e:
           logger.warning('Unable to read segment status file: ' + str(e))
           logger.warning('gpupgradmirror exiting')
           """ KAS note to self. There is a reason for only putting out a warning here. Something
               to do with how upgrade wants me to handle the situation. Need to revisit this and
               figure out if this is still correct
           """
           exit(0)

        if not self._status_values.has_key(self._status[-1]):
            raise InvalidStatusError('Invalid status file.  Unknown status %s' % self._status)


   #-------------------------------------------------------------------------------
    def create_status_file(self):
        """Creates a new gpupgrademirror status file"""
        try:
            if options.phase2 == True:
                startStatus = 'PHASE1_DONE'
            elif options.mode == 'S':
                startStatus = 'PHASE1_SAFE_MODE'
            else:
                startStatus = 'PHASE1_UNSAFE_MODE'
                
            self._fp = open(self._status_filename, 'w')
            self._fp.write(startStatus + ':None\n')
            self._fp.flush()
            self._status.append(startStatus)
            self._status_info.append('None')
        except IOError, e:
            self.logger.error('Unable to create status file %s.' % self._status_filename)
            raise e

   #-------------------------------------------------------------------------------
    def set_status(self, status, status_info=None):
        """Sets the current status.  gpupgrademirror status must be set in
           proper order.  Any out of order status result in an
           InvalidStatusError exception"""
        self.logger.debug("Segment %s: Transitioning from %s to %s" % (str(self.segment_dbid), self._status[-1], status))

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


#-----------conb--------------------------------------------------------------------
#-------------------------------------------------------------------------------
class Phase1(threading.Thread):
    '''
     An object of this class represents a single node in a cluster. 
     It will upgrade all mirror segments on that node.
    '''

    def __init__(self, dburl, logger, info_data_directory, node_name, mirror_info_list, setup_completed_event, continue_event):
        threading.Thread.__init__(self)
        
        self.tempOldMirrorDirSuffix = 'gpupgrademirrors.originalmirror'
        self.tempNewMirrorDirSuffix = 'gpupgrademirrors.copyfromprimary'
        self.fixupFileList          = [ 'postgresql.conf' 
                                      , 'pg_hba.conf'
                                      , 'pg_ident.conf'
                                      ]
        self.dburl                  = dburl
        self.logger                 = logger
        self.info_data_directory    = info_data_directory
        self.nodeName               = node_name
        self.mirrorSegmentList      = mirror_info_list
        self.statusList             = []
        self.setupCompletedEvent    = setup_completed_event
        self.continueEvent          = continue_event
        self.startedRun             = False
        self.completedRun           = False

    #-------------------------------------------------------------------------------
    def  setup(self):
        self.logger.debug("Phase1 setup for mirror: " + str(self.nodeName))
        
        for seg in self.mirrorSegmentList:
            self.logger.debug("setup for segment: " + str(seg.mirror_dbid))

            """ Create a temp directory in each primary segment for our use"""
            seg.tempDir = seg.primary_data_directory + "/" + GPUPGRADEMIRROR_TEMP_DIR
            cmdComplete = False
            while cmdComplete == False:
                rmCmd = CreateDirIfNecessaryWithMode( name = 'gpupgrademirror make temp dir: %s' % seg.tempDir
                             , directory = seg.tempDir
                             , ctxt = REMOTE
                             , remoteHost = seg.primary_host
                             , mode = "0700"
                             )
                cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()
            
            ''' Make sure their is enough space for the largest segment '''
            if options.rollback == False and options.continue_upgrade == False:
               self.checkAvailableSpace(seg)            
            
            ''' Create a status object for each mirror segment '''
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , info_data_directory = self.info_data_directory
                                          , segment_dbid = seg.mirror_dbid
                                          , phase = 1
                                          )
            self.statusList.append(status)
            if options.rollback == True:
                ''' setup for rollback '''
                if status.get_current_status() == (None, None):
                    self.logger.warning('There is no upgrade mirror in progress for this segment: ' + str(seg.mirror_dbid))
                    """ Create a status file so that other logic works correctly """
                    status.create_status_file()
                if status.compare_status('START_SEG_SETUP') >= 0:
                   genLink = SegHardLinks( mirror_info = seg
                                         , logger = self.logger
                                         )
                   genLink.removeLinks()
                if     status.compare_status('START_REMOVE_OLD_MIRROR') >= 0 \
                   and status.compare_status('PHASE1_DONE') < 0:
                   ''' We are past the point of recovering. Must move forward at this point '''
                   self.logger.error('The upgrade of the system mirrors is past the point of recovery.')
                   self.logger.error('You must continue system mirror upgrade at this point.')
                   raise ValidationError('Unable to rollback')
            elif options.continue_upgrade == True:
                ''' Setup to continue upgrade '''
                if status.get_current_status() == (None, None):
                   status.create_status_file()
                if status.compare_status('END_SEG_SETUP') < 0:
                   genLink = SegHardLinks( mirror_info = seg
                                         , logger = self.logger
                                         )  
                   genLink.removeLinks()
                   status.set_status('START_SEG_SETUP')
                   genLink.createLinks()        
                   status.set_status('END_SEG_SETUP')                     
            else:
                if (status.get_current_status() != (None, None)):
                   self.logger.error("Upgrade mirrors already started.")
                   self.logger.error("You must either rollback or continue mirror upgrade") 
                   raise ValidationError("Unable to continue")
                else:
                   """ We are doing a normal phase 1 setup """ 
                   status.create_status_file()
                   genLink = SegHardLinks( mirror_info = seg
                                         , logger = self.logger
                                         )  
                   status.set_status('START_SEG_SETUP')
                   genLink.createLinks()        
                   status.set_status('END_SEG_SETUP')    

    #-------------------------------------------------------------------------------
    def renameOldMirrorToTemp(self, seg):
        self.logger.debug('in renameOldMirror %s:5s seg id = %s')
        
        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix

        if options.continue_upgrade == True:           
           cmdComplete = False
           while cmdComplete == False:
              cmd = RemoveFiles( name = 'gpupgrademirror remove any old directory'
                                , remoteHost = self.nodeName 
                                , directory = tempDataDir
                                , ctxt = REMOTE
                                )
              cmdComplete = runAndCheckCommandComplete(cmd)
           cmd.validate()
        
        if options.mode == 'S':
           cmdComplete = False
           while cmdComplete == False:
              rmCmd = GPumMoveDirectory( name = 'gpupgrademirror move old mirror to temp location: %s:%s' % (seg.mirror_data_directory, tempDataDir)
                                , srcDirectory = seg.mirror_data_directory
                                , dstDirectory = tempDataDir
                                , ctxt  = REMOTE
                                , remoteHost = self.nodeName
                                )
              cmdComplete = runAndCheckCommandComplete(rmCmd)
           rmCmd.validate()
        else:
           ''' Un-safe mode '''
           cmdComplete = False
           while cmdComplete == False:
              rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror make temp directory for old mirror special files: %s' % tempDataDir
                                , directory = tempDataDir
                                , ctxt = REMOTE
                                , remoteHost = self.nodeName
                                , mode = 700
                                )
              cmdComplete = runAndCheckCommandComplete(rmCmd)
           rmCmd.validate()
           for file in self.fixupFileList:
                fullPathFile = seg.mirror_data_directory + '/' + file
                fullPathTarget = tempDataDir + '/' + file
                cmdComplete = False
                while cmdComplete == False:
                   rmCmd = RemoteCopyPreserve( name = 'gpupgrademirror fixup file list fixup copy : %s:%s' % (fullPathFile, fullPathTarget)
                              , srcDirectory = fullPathFile
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget
                              , ctxt = REMOTE
                              , remoteHost = self.nodeName
                              )
                   cmdComplete = runAndCheckCommandComplete(rmCmd)
                rmCmd.validate()
                
                
    #-------------------------------------------------------------------------------
    def renameTempMirrorToOldMirror(self, seg, dirSuffix):
        self.logger.debug("in renameTempMirrorToOldMirror for irror data directory %s:%s" % (self.nodeName, seg.mirror_data_directory))
        
        tempDataDir = seg.mirror_data_directory + dirSuffix
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = GPumMoveDirectory( name = 'gpupgrademirror move temp mirror to mirror location: %s:%s' % (tempDataDir, seg.mirror_data_directory)
                                 , srcDirectory = tempDataDir
                                 , dstDirectory = seg.mirror_data_directory
                                 , ctxt  = REMOTE
                                 , remoteHost = self.nodeName
                                 )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()           

    #-------------------------------------------------------------------------------                                              
    def copyPrimaryToMirror(self, seg):
        self.logger.debug("in copyPrimaryToMirror for primary %s:%s" % (seg.primary_host, seg.primary_data_directory))

        mirrorTempDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix 
        cmdComplete = False
        """ Make the <seg data dir>gpupgrademirrors.copyfromprimary directory """
        while cmdComplete == False:
            mkDirCmd = CreateDirIfNecessaryWithMode( name = 'gpupgrademirror make temp directory for old mirror special files: %s' % mirrorTempDataDir
                             , directory = mirrorTempDataDir
                             , ctxt = REMOTE
                             , remoteHost = self.nodeName
                             , mode = '0700'
                             )
            cmdComplete = runAndCheckCommandComplete(mkDirCmd)
        mkDirCmd.validate()

        mirrorTempBaseDataDir = mirrorTempDataDir + "/base"
        cmdComplete = False
        """ Make the <seg data dir>gpupgrademirrors.copyfromprimary/base directory """
        while cmdComplete == False:
           mkDirCmd = CreateDirIfNecessaryWithMode( name = 'gpupgrademirror make temp directory for old mirror special files: %s' % mirrorTempBaseDataDir
                         , directory = mirrorTempBaseDataDir
                         , ctxt = REMOTE
                         , remoteHost = self.nodeName
                         , mode = '0700'
                         )
           cmdComplete = runAndCheckCommandComplete(mkDirCmd)
        mkDirCmd.validate()
        
        genLink = SegHardLinks(seg, self.logger)
        dirList = genLink.getHardDirList()
       
        for dir in dirList:
            targetTempDir = mirrorTempDataDir + "/base/" + SegHardLinks.getUnqualifiedLinkDir(dir)
            if dir.endswith(".0"):
               self.logger.debug("handle special directory *.0 for %s:%s" % (self.nodeName, targetTempDir))
               """ Special directory """
               cmdComplete = False
               while cmdComplete == False:
                  rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror special directory: %s:%s' % (self.nodeName, dir)
                              , directory = targetTempDir
                              , ctxt = REMOTE
                              , remoteHost = self.nodeName
                              )
                  cmdComplete = runAndCheckCommandComplete(rmCmd)
               rmCmd.validate()
               continue

            cmdComplete = False
            while cmdComplete == False:
               mkDirCmd = CreateDirIfNecessaryWithMode( name = 'gpupgrademirror make temp directory for old mirror special files: %s' % targetTempDir
                                 , directory = targetTempDir
                                 , ctxt = REMOTE
                                 , remoteHost = self.nodeName
                                 , mode = '0700'
                                 )
               cmdComplete = runAndCheckCommandComplete(mkDirCmd)
            mkDirCmd.validate()      
        
        nicList = p1MirrorInfo.getInterfaceList(self.nodeName)
        self.logger.debug("NICs for mirror %s are %s" % (self.nodeName, str(nicList)))
        workersPerNic = 2
        totalWorkers = len(nicList) * workersPerNic
        availableNicQueue = Queue(totalWorkers)
        
        """ Need to prime the queue """
        for wpn in range(workersPerNic):
            for nic in nicList:
                availableNicQueue.put(nic)
                
        wPool = WorkerPool(numWorkers = totalWorkers)
        
        """ copy everyting in the base directory """
        numCopiesStarted = 0
        numCopiesCompleted = 0
        for dir in dirList:
            """ 
              KAS note to self. Need to add code here to check to see if we need to stop.
              That is, the process that started this thread may tell us to die, probably
              because one of the other threads finished abnormally, so we are stopping the
              mirror upgrade.
            """
            sourceDir = dir
            specialSourceDir = sourceDir.endswith(".0")
            targetDir = mirrorTempDataDir + "/base/" + SegHardLinks.getUnqualifiedLinkDir(sourceDir)
            nicToUse = availableNicQueue.get()
            if specialSourceDir == True:
               self.logger.debug("handle special case directoryn %s:%s" % (seg.primary_host, str(dir)))
               """ Special case of directory with empty files """
               rmCopy = RemoteCopyPreserve( name = 'gpupgrademirror remotecopy special .0 file to mirror: %s:%s' % (self.nodeName, seg.mirror_data_directory)
                         , srcDirectory = sourceDir
                         , dstHost = nicToUse
                         , dstDirectory = targetDir
                         , ctxt = REMOTE
                         , remoteHost = seg.primary_host
                         )
               rmCopy.run(validateAfter = True)
               availableNicQueue.put(nicToUse)
               continue
            else:
               pysyncCmd = PySyncPlus( name = 'gpupgrademirror pysync primary to mirror: %s:%s' % (self.nodeName, seg.mirror_data_directory)
                             , srcDir = sourceDir
                             , dstHost = nicToUse
                             , dstDir = targetDir
                             , ctxt  = REMOTE
                             , remoteHost = seg.primary_host
                             )
            """ 
              KAS note to self. Need to do something here to handle the case where 
              the machine we are connecting too has is validating more connections
              than it can handle. We need to retry the command. In the mean time,
              I'm delaying every command 1 second and hoping that is enough.
            """
            ###time.sleep(1)
            wPool.addCommand(pysyncCmd)
            numCopiesStarted = numCopiesStarted + 1
            if availableNicQueue.empty() == True:
               while True:
                   completedCmdList = wPool.getCompletedItems()
                   if len(completedCmdList) > 0:
                      numCopiesCompleted = numCopiesCompleted + len(completedCmdList)
                      break;
               for compCmd in completedCmdList:
                   if sshBusy(compCmd) == True:  
                      time.sleep(1)
                      if isinstance(compCmd, PySyncPlus) == True:
                         redoCmd = PySyncPlus( name = compCmd.namePlus
                                                    , srcDir = compCmd.srcDirPlus
                                                    , dstHost = compCmd.dstHostPlus
                                                    , dstDir = compCmd.dstDirPlus
                                                    , ctxt = compCmd.ctxtPlus
                                                    , remoteHost = compCmd.remoteHostPlus
                                                    , options = compCmd.optionsPlus
                                                    )
                      else:
                         redoCmd = RemoteCopyPreserve( name = 'gpupgrademirror remotecopy special .0 file to mirror: %s:%s' % (compCmd.dstHost, compCmd.srcDirectory)
                                                     , srcDirectory = compCmd.srcDirectory
                                                     , dstHost = compCmd.dstHost
                                                     , dstDirectory = compCmd.dstDirectory
                                                     , ctxt = REMOTE
                                                     , remoteHost = seg.primary_host
                                                     )
                      wPool.addCommand(redoCmd)
                   else:
                      compCmd.validate()
                      availableNicQueue.put(compCmd.destinationHost)

        """ Collect all the uncompleted copies. """
        while numCopiesStarted > numCopiesCompleted:
            completedCmdList = wPool.getCompletedItems()
            if len(completedCmdList) > 0:
               numCopiesCompleted = numCopiesCompleted + len(completedCmdList)
               for compCmd in completedCmdList:
                   """ Go through the last commands and see if we need to retry """
                   retryPossible = sshBusy(compCmd)
                   if retryPossible == True:
                      cmdComplete = False
                   else:
                      cmdComplete = True     
                   while cmdComplete == False:
                      if isinstance(compCmd, PySyncPlus) == True:
                         compCmd = PySyncPlus( name = compCmd.namePlus
                                                    , srcDir = compCmd.srcDirPlus
                                                    , dstHost = compCmd.dstHostPlus
                                                    , dstDir = compCmd.dstDirPlus
                                                    , ctxt = compCmd.ctxtPlus
                                                    , remoteHost = compCmd.remoteHostPlus
                                                    , options = compCmd.optionsPlus
                                                    )
                      else:
                         compCmd = RemoteCopyPreserve( name = 'gpupgrademirror remotecopy special .0 file to mirror: %s:%s' % (compCmd.dstHost, compCmd.srcDirectory)
                                                     , srcDirectory = compCmd.srcDirectory
                                                     , dstHost = compCmd.dstHost
                                                     , dstDirectory = compCmd.dstDirectory
                                                     , ctxt = REMOTE
                                                     , remoteHost = seg.primary_host
                                                     )
                      cmdComplete = runAndCheckCommandComplete(compCmd)
                   compCmd.validate()                
            else:
               time.sleep(1)

        wPool.haltWork()
        wPool.joinWorkers()
        genLink.removeLinks()

        """
          Now we need to copy everything but the base directory. We assume these files are small in
          comparison to the base directory, so we will use scp instead of pysync. Also, there may
          be some zero-lenght files, which pysync can not handle.
        """
        cmdComplete = False
        while cmdComplete == False:
           fdlCmd = FileDirectoryList( name = "gpupgrademirror find all non-base dirs and files for %s:%s " % (seg.primary_host, seg.primary_data_directory)
                                     , filePattern = seg.primary_data_directory
                                     , ctxt = REMOTE
                                     , remoteHost = seg.primary_host
                                     )
           cmdComplete = runAndCheckCommandComplete(fdlCmd)
        fdlCmd.validate()           
        nonBaseList = fdlCmd.get_result_list()
        nonBaseList.remove("base")
        
        for nonBase in nonBaseList:
           cmdComplete = False
           primaryNonBase = seg.primary_data_directory + "/" + nonBase
           while cmdComplete == False:
              self.logger.debug('remotecopy everything but base: %s:%s' % (self.nodeName, primaryNonBase))
              rcCmd = RemoteCopyPreserve( name = 'gpupgrademirror remotecopy everything but base: %s:%s' % (self.nodeName, primaryNonBase)
                                , srcDirectory = primaryNonBase
                                , dstHost = seg.mirror_host
                                , dstDirectory = mirrorTempDataDir
                                , ctxt = REMOTE
                                , remoteHost = seg.primary_host
                                )
              cmdComplete = runAndCheckCommandComplete(rcCmd)
           rcCmd.validate()        

        """ Put the base directory back together the way it was originally (e.g. 1.1 and 1.2 go into 1) """
        dbDirs = DirectoryList( name = 'gpupgrademirror get database dirs for primary seg dbid +' + str(seg.primary_dbid)
                              , dirLocation = seg.primary_data_directory + "/base"
                              , ctxt = REMOTE
                              , remoteHost = seg.primary_host 
                              )
        dbDirs.run(validateAfter = True)
        dbDirList = dbDirs.get_result_list()
        for db in dbDirList:
            unqualDir = SegHardLinks.getUnqualifiedLinkDir(db)
            qualDir = mirrorTempDataDir + "/base/" + unqualDir
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = MakeDirectoryWithMode( name = 'gpupgrademirror make consolidation directory: %s' % qualDir
                                 , directory = qualDir
                                 , ctxt = REMOTE
                                 , remoteHost = seg.mirror_host
                                 , mode = 700
                                 )
               cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()
        for dir in dirList:
            unqualDir = SegHardLinks.getUnqualifiedLinkDir(dir)
            srcDirFiles = mirrorTempDataDir + "/base/" + unqualDir
            trgDir = mirrorTempDataDir + "/base/" + SegHardLinks.getUnqualifiedRealDir(dir)
            cmdComplete = False
            while cmdComplete == False:
               mvCmd = GPumMoveDirectoryContents( name = 'gpupgrademirror consolidate a directory: %s' % srcDirFiles 
                                     , srcDirectory = srcDirFiles
                                     , dstDirectory = trgDir
                                     , ctxt = REMOTE
                                     , remoteHost = seg.mirror_host
                                     )
               cmdComplete = runAndCheckCommandComplete(mvCmd)
            mvCmd.validate()
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = RemoveDirectory( name = 'gpupgrademirror remove unconsolidated dir: %s' % srcDirFiles 
                                          , directory = srcDirFiles
                                          , ctxt = REMOTE
                                          , remoteHost = seg.mirror_host 
                                          )
               cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()
            
    #------------------------------------------------------------------------------ 
    def newMirrorFileFixup(self, seg):
        self.logger.debug('in newMirrorFileFixup for %s:%s' % (self.nodeName, seg.mirror_data_directory))

        if options.mode == 'S':
           tempSourceDataDir = seg.mirror_data_directory 
        else:
           tempSourceDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        
        tempTargetDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix

        for file in self.fixupFileList:
            fullPathFile = tempSourceDataDir + '/' + file
            fullPathTarget = tempTargetDataDir + '/' + file
            
                               
            """ thetemplate
            cmdComplete = False
            while cmdComplete == False:
                Cmd = 
                cmdComplete = runAndCheckCommandComplete(
            Cmd.validate()
            """
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = RemoteCopyPreserve( name = 'gpupgrademirror fixup copy : %s to %s' % (fullPathFile, fullPathTarget)
                              , srcDirectory = fullPathFile
                              , dstHost = self.nodeName
                              , dstDirectory = fullPathTarget
                              , ctxt = REMOTE
                              , remoteHost = self.nodeName
                              )
               cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()  
        
    #-------------------------------------------------------------------------------
    def removeOldMirror(self, seg):
        self.logger.debug('in removeOldMirror %s:%s' % (self.nodeName, seg.mirror_data_directory))

        if options.mode == 'S' and options.rollback == False:
           ''' If in a safe mode upgrade, point to the temp locatio of the old mirror '''
           tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        else:
           ''' if in un-safe mode or in rollback, remove all files in the real mirror location'''
           tempDataDir = seg.mirror_data_directory
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror data directory: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()

        
    #-------------------------------------------------------------------------------
    def removeOldMirrorSpecialFiles(self, seg):
        self.logger.debug('in removeOldMirrorSpecialFiles %s:%s' % (self.nodeName, seg.mirror_data_directory))

        tempDataDir = seg.mirror_data_directory + self.tempOldMirrorDirSuffix
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove old mirror special files: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()

    #-------------------------------------------------------------------------------
    def removeCopyOfPrimary(self, seg):
        ''' This method is used during recovery '''
        self.logger.debug('in removeCopyOfPrimary %s:%s' % (self.nodeName, seg.mirror_data_directory))

        tempDataDir = seg.mirror_data_directory + self.tempNewMirrorDirSuffix
        cmdComplete = False
        while cmdComplete == False:
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove the copied mirror data directory: %s:%s' % (self.nodeName, tempDataDir)
                           , directory = tempDataDir
                           , ctxt = REMOTE
                           , remoteHost = self.nodeName
                           )
           cmdComplete = runAndCheckCommandComplete(rmCmd)
        rmCmd.validate()

    #-------------------------------------------------------------------------------
    def checkAvailableSpace(self, seg):
        self.logger.debug('in checkAvailableSpace %s:%s' % (seg.primary_host, seg.primary_data_directory))

        ''' Get size of primary '''
        cmdComplete = False
        while cmdComplete == False:            
           dirSizeCmd = DiskUsage( name = "gpupgrademirror check for disk usage on primary: " + str(seg.primary_data_directory)
                              , directory = seg.primary_data_directory
                              , ctxt = REMOTE
                              , remoteHost = seg.primary_host
                              )
           cmdComplete = runAndCheckCommandComplete(dirSizeCmd)
        dirSizeCmd.validate()
        dirSize = dirSizeCmd.get_bytes_used()
            
        ''' Get available space on mirror '''
        cmdComplete = False
        while cmdComplete == False:   
           mirrorFreeSizeCmd = GPumDiskFree( name = 'gpupgrademirror disk space available on mirror: ' + str(seg.mirror_data_directory)
                                           , directory = seg.mirror_data_directory
                                           , ctxt = REMOTE
                                           , remoteHost = self.nodeName
                                           )
           cmdComplete = runAndCheckCommandComplete(mirrorFreeSizeCmd)
        mirrorFreeSizeCmd.validate()
        mirrorFreeSize = mirrorFreeSizeCmd.get_bytes_free()
        if options.mode == 'U':
            ''' Add current mirror size to available mirror space '''
            cmdComplete = False
            while cmdComplete == False:  
               currentMirrorSizeCmd = DiskUsage( name = "gpupgrademirror check for disk usage on mirror: " + str(seg.mirror_data_directory) 
                                               , directory = seg.mirror_data_directory
                                               , ctxt = REMOTE
                                               , remoteHost = self.nodeName
                                               )
               cmdComplete = runAndCheckCommandComplete(currentMirrorSizeCmd)
            currentMirrorSizeCmd.validate()
            currentMirrorSize = currentMirrorSizeCmd.get_bytes_used()
            mirrorFreeSize = mirrorFreeSize + currentMirrorSize  

        ''' Compare available space to needed space (add some extra space just to make sure) '''
        requiredSpace = int(dirSize) + int(TEN_GIG)
        if mirrorFreeSize < requiredSpace:
           self.logger.error("Size of mirror directory is too small %s:%s." % (self.nodeName, seg.mirror_data_directory))          
           self.logger.error("The mirror directory must have at least %s bytes free. " % str(requiredSpace))
           self.logger.error("The mirror has %s bytes free." % str(mirrorFreeSize))
           raise ValidationError('Mirror data directory is too small')

    #-------------------------------------------------------------------------------
    def run(self):
        try:
            self.startedRun = True
            self.setup()
            """
            self.setupCompletedEvent.set()
            self.continueEvent.wait()
            """
            self.logger.debug("continuing run of phase1 for mirror: " + str(self.nodeName))
            statusIndex = 0
            for seg in self.mirrorSegmentList:
                status = self.statusList[statusIndex]
                
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
                statusIndex = statusIndex + 1
            self.shutdown()
            self.completedRun = True
        except Exception, e:
            self.logger.error('ERROR in processing mirror: %s Exception: %s' % (self.nodeName, str(e)))
            self.logger.error('gpupgradmirror exiting')
            traceback.print_exc()
            sys.exit(2)

    #-------------------------------------------------------------------------------
    def shutdown(self):
        for seg in self.mirrorSegmentList:
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = RemoveFiles( name = 'gpupgrademirror remove temp dir: %s:%s' % (seg.primary_host, seg.tempDir) 
                                      , directory = seg.tempDir
                                      , ctxt = REMOTE
                                      , remoteHost = seg.primary_host 
                                      )
               cmdComplete = runAndCheckCommandComplete(rmCmd)


#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
class Phase2(threading.Thread):

    def __init__(self, dburl, logger, info_data_directory, node_name):
        threading.Thread.__init__(self)
        
        self.dburl                  = dburl
        self.logger                 = logger
        self.info_data_directory  = info_data_directory
        self.nodeName               = node_name
        self.mirrorSegmentList      = []
        self.statusList             = []
        self.fixupFileList          = []
        self.startedRun             = False


    #-------------------------------------------------------------------------------
    def  setup(self, mirrorInfoList, fileList, specialDirectoryList):
        self.logger.debug("in Phase2.setup: " + str(self.nodeName))
        self.mirrorSegmentList = mirrorInfoList

        for seg in self.mirrorSegmentList:
            
            ''' Get status file from Phase 1 for each mirror segment '''
            status = GpUpgradeMirrorStatus( logger = self.logger
                                          , info_data_directory = self.info_data_directory
                                          , segment_dbid = seg.mirror_dbid
                                          , phase = 2
                                          )
            self.statusList.append(status)
            
            """ Create a temp directory in each primary segment for our use"""
            seg.tempDir = seg.primary_data_directory + "/" + GPUPGRADEMIRROR_TEMP_DIR
            cmdComplete = False
            while cmdComplete == False:
                rmCmd = CreateDirIfNecessaryWithMode( name = 'gpupgrademirror make temp dir: %s' % seg.tempDir
                             , directory = seg.tempDir
                             , ctxt = REMOTE
                             , remoteHost = seg.primary_host
                             , mode = "0700"
                             )
                cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()
            
            if options.continue_upgrade == True:
                ''' Setup to continue upgrade '''
            else:
                if (status.get_current_status() != (None, None)):
                   self.logger.error("Upgrade mirrors phase 2 already started.")
                   self.logger.error("You must continue mirror upgrade") 
                   raise ValidationError("Unable to run phase 2")
                else:  
                   status.create_status_file()
            self.logger.debug("completed Phase2.setup for mirror seg dbid: " + str(seg.mirror_dbid))
            
        self.fixupFileList = fileList
        self.specialDirectoryList = specialDirectoryList


    #------------------------------------------------------------------------------ 
    def newMirrorSpecialFileFixup(self, seg):
        self.logger.debug('in newMirrorSpecialFileFixup for seg dbid %s location: %s:%s' % (str(seg.mirror_dbid), seg.mirror_host, seg.mirror_data_directory))

        """ Delete all files on the delete list """
        for file in self.specialDirectoryList:
            fullPathTarget = seg.mirror_data_directory + '/' + file
            cmdComplete = False
            while cmdComplete == False:
                cmd = RemoveFiles( name = 'gpupgrademirror remove special old directory'
                                 , remoteHost = self.nodeName
                                 , directory = fullPathTarget
                                 , ctxt = REMOTE
                                 )
                cmdComplete = runAndCheckCommandComplete(cmd)

        """ Check for files with extensions (i.e. .1, .2, ...)   """
        """ First, get a list of all files in the base directory """
        fixupFileExtsList = []
        baseDir = seg.primary_data_directory + "/base"
        primaryTempFile =  seg.tempDir + '/tempfilesindir'
        cmdComplete = False
        while cmdComplete == False:
           fdCmd = FilesInDir( name = "gpupgrademirror files in dir %s:%s" % (seg.primary_host, baseDir)
                             , filePattern = baseDir
                             , ctxt = REMOTE
                             , remoteHost = seg.primary_host
                             , remoteTempFile = primaryTempFile
                             )
           cmdComplete = runAndCheckCommandComplete(fdCmd)
        fdCmd.validate()
        allBaseList = fdCmd.get_result_list(localTempFile = options.info_data_directory + "/tempallbase")

        allBaseList.sort()

        """ Searh the base directory list for all fixup files that have dots (i.e. 123.1, 123.2) """
        dotFiles = []
        for file in self.fixupFileList:
            fullPathFile = seg.primary_data_directory + '/' + file
            tempList = findRelFileDotNodes(fullPathFile, allBaseList)
            for suffix in tempList:
                dotFiles.append(file + "." + suffix)
        
        allFixupFileList = self.fixupFileList + dotFiles  
            
        sph = SpecialFileHandling( sourceList = allFixupFileList 
                                 , seg = seg 
                                 )
        sph.createLinks()
              
        cmdComplete = False
        while cmdComplete == False:
           fdlCmd = FileDirectoryList( name = "gpupgrademirror find all dirs and files for %s:%s " % (seg.primary_host, sph.fullPathLinkDir)
                                     , filePattern = sph.fullPathLinkDir
                                     , ctxt = REMOTE
                                     , remoteHost = seg.primary_host
                                     )
           cmdComplete = runAndCheckCommandComplete(fdlCmd)
        fdlCmd.validate() 
        linkDirList = fdlCmd.get_result_list()
              
        copyList = []
        for element in specialDirectoryList:
            elementList = element.split("/")
            if len(elementList) > 1:
                elementPrefix =  "/" + "/".join(elementList[:-1])
            else:
                elementPrefix = ""
            copyList.append([seg.primary_data_directory + "/" + element, seg.mirror_data_directory + elementPrefix ])
        for element in linkDirList:
            copyList.append([sph.fullPathLinkDir + "/" + element, seg.mirror_data_directory])
        
        for copyElement in copyList:
            cmdComplete = False
            while cmdComplete == False:
               rcCmd = RemoteCopyPreserve( name = 'gpupgrademirror phase 2 fixup copy : %s:%s to %s:%s' % (seg.primary_host, copyElement[0], seg.mirror_host, copyElement[1])
                              , srcDirectory = copyElement[0]
                              , dstHost = seg.mirror_host
                              , dstDirectory = copyElement[1]
                              , ctxt = REMOTE
                              , remoteHost = seg.primary_host
                              )
               cmdComplete = runAndCheckCommandComplete(rcCmd)
            rcCmd.validate()
        
        return

    #-------------------------------------------------------------------------------
    def run(self):
        self.startedRun = True
        try:
            i = 0
            for seg in self.mirrorSegmentList:
                self.logger.debug("started run for Phase 2 for seg mirror: %s" % str(seg.mirror_dbid))
                status = self.statusList[i]
                currentStatus = status.get_current_status()
                
                if status.compare_status('START_PHASE2') <= 0:
                   status.set_status('START_PHASE2')
                   ''' Copy catalog tables to mirror '''
                   self.newMirrorSpecialFileFixup(seg)
                if status.compare_status('PHASE2_DONE') <= 0:
                   status.set_status('PHASE2_DONE')
                i = i + 1
            self.shutdown()
        except Exception, e:
            self.logger.error('ERROR in processing mirror: %s Exception: %s' % (self.nodeName, str(e)))
            self.logger.error('gpupgradmirror exiting')
            traceback.print_exc()
            sys.exit(2)

    #-------------------------------------------------------------------------------
    def shutdown(self):
        for seg in self.mirrorSegmentList:
            cmdComplete = False
            while cmdComplete == False:
               rmCmd = RemoveFiles( name = 'gpupgrademirror remove temp dir: %s:%s' % (seg.primary_host, seg.tempDir) 
                                      , directory = seg.tempDir
                                      , ctxt = REMOTE
                                      , remoteHost = seg.primary_host 
                                      )
               ###print "rmCmd = "
               ###print str(rmCmd)
               cmdComplete = runAndCheckCommandComplete(rmCmd)
            rmCmd.validate()

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
phase2MirrorList = []


try:
  # setup signal handlers so we can clean up correctly
  signal.signal(signal.SIGTERM, sig_handler)
  signal.signal(signal.SIGHUP, sig_handler)
  logger = get_default_logger()
  setup_tool_logging(EXECNAME,getLocalHostname(),getUserName())

  options, args = parseargs()

  if len(options.ids) > 0:
     options.ids = options.ids.split(':')

  if options.verbose:
     enable_verbose_logging()

  if is_gpupgrademirror_running(options.info_data_directory):
     logger.error('gpupgrademirror is already running.  Only one instance')
     logger.error('of gpupgrademirror is allowed at a time.')
     remove_pid = False
     sys.exit(1)
  else:
     create_pid_file(options.info_data_directory)

  dbUrl = dbconn.DbURL(dbname = 'template1')
     
  """ 
      We will start the database in master only mode unless we are in phase 1 and 
      we are rolling back or continuing. If we are in phase 1 and re rolling back
      or continuing, we will get the information we need from a flat file we
      created on the first phase 1 attempt.   
  """
  gpstartStatus = None
  if options.phase2 == False and (options.rollback == True or options.continue_upgrade == True):
     pass
  else:
     GpStart.local('gpupgrademirror start database', masterOnly = True)
     gpstartStatus = "MASTER"

  if options.phase2 == True:
     phase = 2
  else:
     phase = 1

  overallStatus = GpUpgradeMirrorStatus( logger = logger
                                       , info_data_directory = options.info_data_directory
                                       , segment_dbid = 0
                                       , phase = phase
                                       , overall = True
                                       )
  if options.rollback == True:
     ''' setup for rollback '''
     if overallStatus.get_current_status() == (None, None):
        logging.warning('There is no upgrade mirror in progress')
        exit(0)
     elif overallStatus.compare_status("SETUP_DONE") < 0:
        """
           We have not gotten past the setup phase,             
           so remove the gpugprademirr info directory and exit. 
        """
        rmCmd = RemoveFiles( name = 'gpupgrademirror remove info directory: %s' % (options.info_data_directory)
                           , directory = options.info_data_directory
                           , ctxt = LOCAL
                           , remoteHost = None
                           )
        rmCmd.run(validateAfter = False)
        exit(0)
  elif options.continue_upgrade == True:
     ''' Setup to continue upgrade '''
     pass
  elif options.phase2 == False:
     if (overallStatus.get_current_status() != (None, None)):
        logging.error("Upgrade mirrors already started.")
        logging.error("You must either rollback or continue mirror upgrade") 
        raise ValidationError("Unable to continue")
     else:
        """  We are in normal phase 1 or phase 2 mode """
        overallStatus.create_status_file()

  if options.phase2 == True:
     """
        Phase 2 of upgrade mirrors 
        
        There are a number of special files that we need to copy from the primary in phase 2.
        Most of these files are catalog files.
     """
     databaseList = []
          
     if options.rollback == True:
        raise ValidationError('Rollback is not possible in phase 2 mirror upgrade.')
    
     conn   = dbconn.connect( dburl   = dbUrl
                            , utility = True
                            )

     ''' Get a list of databases '''
     databaseCursor = dbconn.execSQL(conn, DatabaseRow.query())
     for row in databaseCursor:
         dbrow = DatabaseRow(row)
         databaseList.append(dbrow)
     conn.close()
         
     ''' Connect to each database, and make a list of files we will use '''
     fileList = []
     specialDirectoryList = []
     ''' always copy the global directory '''
     specialDirectoryList.append('global')
     ''' always copy the pg_xlog directory '''
     specialDirectoryList.append('pg_xlog')     
     for db in databaseList:
         if db.databaseName == 'template0':
            ''' Special case where we copy the entire database '''
            specialDirectoryList.append(str('base/' + str(db.databaseDirectory)))
            continue
                            
         connectURL = dbconn.DbURL(dbname = db.databaseName)
         connectForDB = dbconn.connect( dburl   = connectURL
                                      , utility = True
                                      ) 
         ''' List of non-toast catalog tables '''
         DBcursor = dbconn.execSQL(connectForDB, NonCatalogToastTablesRow.query())
         for fileRow in DBcursor:
             nonToastRow = NonCatalogToastTablesRow(fileRow)
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(nonToastRow.filename))
             
         ''' List of toast catalog tables and their toast files '''
         DBcursor = dbconn.execSQL(connectForDB, CatalogToastTablesRow.query())
         for fileRow in DBcursor:
             toastTableRow = CatalogToastTablesRow(fileRow)
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(toastTableRow.filename))
             fileList.append('base/' + str(db.databaseDirectory) + '/' + str(toastTableRow.toastfilename))
         connectForDB.close()
    
     try:
        p1MirrorInfo = Phase1MirrorInfo(dbUrl)
     except Exception, e:
        logger.warning('Unable to obtain mirror information: ' + str(e))
        logger.warning('gpupgradmirror exiting')
        exit(0)

     ''' Get list of all segments '''
     allSegMirrorInfo = p1MirrorInfo.getAllMirrorInfoList()
     
     for mirrorInfo in allSegMirrorInfo:
        ''' Create a phase2 object for each mirror segment'''
        tempNode = Phase2(dbUrl, logger, options.info_data_directory, mirrorInfo.nic_address)
        
        ''' Setup the mirror node '''
        """ KAS note to self. Setup could be done in parallel. """
        tempNode.setup([mirrorInfo], fileList, specialDirectoryList)
        
        phase2MirrorList.append(tempNode)
    
     if gpstartStatus != None and gpstartStatus == "MASTER":
        GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)
        gpstartStatus = "STOPPED"

     """
       Start the threads 
     """
     for node in phase2MirrorList:
         node.start()

     """ Wait no more than 60 seconds for all the threads to start """
     numTries = 12
     for thisTry in range(numTries):
         started = True
         for node in phase2MirrorList:
             if node.startedRun == False:
                sleep(5)
                break
         if started == True:
            break
     if started == False:
        raise Exception("Unable to initialize threads for Phase 2 mirror upgrade")
        
     done = False
     completedNodeList = []
     notCompletedNodeList = list(phase2MirrorList)
     while done == False:
         justCompletedNodeList = []
         for node in notCompletedNodeList:
             node.join(10)
             logging.debug("checking to see if thread for mirror node is alive: " + str(node.nodeName))
             if node.isAlive() == False:
                """ The node has completed """
                for segStatus in node.statusList:
                    if segStatus.get_current_status()[0] != 'PHASE2_DONE':
                       done = True
                       break   
                if done == True:
                   break
                justCompletedNodeList.append(node)
         completedNodeList.extend(justCompletedNodeList)
         for completedNode in justCompletedNodeList:
             notCompletedNodeList.remove(completedNode)        
         if len(completedNodeList) == len(phase2MirrorList):
           done = True    
      

     """
     for node in phase2MirrorList:
         node.start()
     for node in phase2MirrorList:
         node.join()                 
     """
     
     """ Make sure all phase 2 threads complated successfully """
     p2Completed = True
     for node in phase2MirrorList:
         for segStatus in node.statusList:
             if segStatus.get_current_status()[0] != 'PHASE2_DONE':
                 logging.error('A segment has reported an error. Segment dbid: %s' % segStatus.segment_dbid)
                 p2Completed = False
                 break
         if p2Completed == False:
             break
         
     if p2Completed == False:
         logging.error('Unable to complete upgrade mirrors for phase 2')
         raise ValidationError('Unable to complete upgrade')
     else:
        Phase1MirrorInfo.removeMirrorInfoFile()
        overallStatus.remove_status_file()
        for node in phase2MirrorList:
            for segStatus in node.statusList:
                segStatus.remove_status_file()
        rmCmd = RemoveFiles( name = 'gpupgrademirror remove info directory: %s' % (options.info_data_directory)
                           , directory = options.info_data_directory
                           , ctxt = LOCAL
                           , remoteHost = None
                           )
        rmCmd.run(validateAfter = False)
  else:
    """
      Phase 1 
    """
    
    if overallStatus.compare_status("START_SETUP") < 0:
        overallStatus.set_status("START_SETUP")
    
    try:
       p1MirrorInfo = Phase1MirrorInfo(dbUrl)
    except Exception, e:
        """ KAS note to self. I don't remember why we only put out a warning here. It's probably because Caleb told me do do that? """
        logger.warning('Unable to obtain mirror information: ' + str(e))
        logger.warning('gpupgradmirror exiting')
        traceback.print_exc()
        exit(0)
    
    """
    print "*** p1MirrorInfo = " 
    print str(p1MirrorInfo)
    print "************"
    print " ==== p1MirrorInfo.getInterfaceList() = "
    print str(p1MirrorInfo.getInterfaceList())
    exit(0)
    """
    
    allSegMirrorInfo = p1MirrorInfo.getAllMirrorInfoList()
        
    if overallStatus.compare_status("SETUP_DONE") < 0:
       overallStatus.set_status("SETUP_DONE")

    """ 
      At this point, all setup that requires database access is done. We can continue from here
      without connecting to the database.
    """ 
    if gpstartStatus != None and gpstartStatus == "MASTER":
       GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)
       gpstartStatus = "STOPPED" 
     
     
    if len(options.ids) > 0:
        """ I am a minion, so skip the minion setup. """
        if overallStatus.compare_status("MINION_START_SETUP") < 0:
           overallStatus.set_status("MINION_START_SETUP") 
        if overallStatus.compare_status("MINION_SETUP_DONE") < 0:
           overallStatus.set_status("MINION_SETUP_DONE")

    """ KAS turn off this logic for now. """
    if overallStatus.compare_status("MINION_START_SETUP") < 0:
       overallStatus.set_status("MINION_START_SETUP") 
    if overallStatus.compare_status("MINION_SETUP_DONE") < 0:
       overallStatus.set_status("MINION_SETUP_DONE")

    """ KAS turn off this logic for now. """
    if True == False and len(options.ids) == 0:
       """ 
         We are the king of the gpupgrademirror processes, so we do not do any upgrading ourself. 
         Instead, we will create a bunch of gpupgrademirror processes on a set of our
         nodes, and slave them to this king process. 
       """
       
       allNodes = p1MirrorInfo.getMirrorNodeList()

       """
         We are limited to how many ssh connections we can make on a given node (estimate 64).
         Also, there may be issues with Python and a large number of threads.
         The number of ssh's at any given moment on a node should be about 8 (2 per NIC, 4 NICs). 
         We should be able to support about 8 mirror nodes per gpupgrademirror instance.
       """
       nodesPerMinion = 1  # 1 for testing should be 8
       minionList = []
       index = 0
       while index < len(allNodes):
          numNodes = 0
          segs = ""
          minionNode = allNodes[index]
          while numNodes < nodesPerMinion and index < len(allNodes):
              node = allNodes[index]
              nodeSegs = p1MirrorInfo.getMirrorInfo(node)
              for segInfo in nodeSegs:
                  if len(segs) > 0:
                      segs = segs + ":"
                  segs = segs + str(segInfo.mirror_dbid)
              index = index + 1
              numNodes = numNodes + 1
          minionOptions = " -c "
          if options.verbose == True:
              minionOptions = minionOptions + " -v "
          minion = GPUpgradeMirrorMinion( name = "gpupgrademirror create a minion"
                                        , options = minionOptions
                                        , segs = str(segs)
                                        , ctxt = REMOTE
                                        , remoteHost = str(minionNode)
                                        )
          minionList.append(minion)

       if overallStatus.compare_status("MINION_START_SETUP") < 0:
          overallStatus.set_status("MINION_START_SETUP")
                    
       """ Copy all the needed files to the minion hosts """
       for minion in minionList:
              minion.createInfoDirectory()
              minion.copyGpupgrademirrorToHost()
              minion.copyStatusFileToHost()
              
       if overallStatus.compare_status("MINION_SETUP_DONE") < 0:
          overallStatus.set_status("MINION_SETUP_DONE")
      
       """ All the minions are set up and ready to go. """
       minionWP = WorkerPool(numWorkers = len(minionList))
       for minion in minionList:
           minionWP.addCommand(cmd = minion)
       numDone = 0
       while numDone < len(minionList):
           resultList = minionWP.getCompletedItems()
           numDone = numDone + len(resultList)
       minionWP.haltWork()
       minionWP.joinWorkers()
       exit(0)
               
    

    """ Create a list of nodes and their segment information """
    ''' Get a list of mirror nodes '''
    mirrorNodeCursor = p1MirrorInfo.getMirrorNodeList()
    numMirrorNodes = len(mirrorNodeCursor)   
       
    for mirrorName in mirrorNodeCursor:
        ''' Get info on mirror segments for this host'''
        mirrorInfoList = p1MirrorInfo.getMirrorInfo(mirrorName)
                   
        ''' Create a gpupgrademirror object for each mirror node'''
        tempNode = Phase1( dburl = dbUrl
                         , logger = logger
                         , info_data_directory = options.info_data_directory
                         , node_name = mirrorName
                         , mirror_info_list = mirrorInfoList
                         , setup_completed_event = threading.Event()
                         , continue_event = threading.Event()
                         )
        MirrorNodeList.append(tempNode)

    if gpstartStatus != None and gpstartStatus == "MASTER":
       GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)   
       gpstartStatus = "STOPPED" 

    """ 
      Start the threads 
    """
    
    """
     KAS Note to self. There is an issue here if one of the treads dies.
     The call to wait will go for every becaseu it will never get an event.
    """
    
    for node in MirrorNodeList:
        node.start()
        
        
    """ Wait no more than 60 seconds for all the threads to start """
    numTries = 12
    for thisTry in range(numTries):
        started = True
        for node in MirrorNodeList:
            if node.startedRun == False:
               sleep(5)
               break
        if started == True:
           break
    if started == False:
       raise Exception("Unable to initialize threads for Phase 1 mirror upgrade")
            
    """        
    for node in MirrorNodeList:
        node.setupCompletedEvent.wait()
    for node in MirrorNodeList:
        node.continueEvent.set()
    """   
        
    """ 
      KAS note to self. Need to add code here to detect and notify all the threads if
      there is an issue with one of the threads, and all others must stop.
    """
        
    done = False
    completedNodeList = []
    notCompletedNodeList = list(MirrorNodeList)
    while done == False:
        justCompletedNodeList = []
        for node in notCompletedNodeList:
            node.join(10)
            logging.debug("checking to see if thread for mirror node is alive: " + str(node.nodeName))
            if node.isAlive() == False:
               """ The node has completed """
               for segStatus in node.statusList:
                   if segStatus.get_current_status()[0] != 'PHASE1_DONE':
                      done = True
                      break   
               if done == True:
                  break
               justCompletedNodeList.append(node)
        completedNodeList.extend(justCompletedNodeList)
        for completedNode in justCompletedNodeList:
            notCompletedNodeList.remove(completedNode)        
        if len(completedNodeList) == len(MirrorNodeList):
           done = True    
    
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
           overallStatus.set_status('PHASE1_DONE')
           rmCmd = RemoveFiles( name = 'gpupgrademirror remove info directory: %s' % (options.info_data_directory)
                           , directory = options.info_data_directory
                           , ctxt = LOCAL
                           , remoteHost = None
                           )
           rmCmd.run(validateAfter = False)
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
           RemoveFiles.local("gpupgrademirror remove %s" % options.info_data_directory
                           , options.info_data_directory
                           )
           """
           Phase1MirrorInfo.removeMirrorInfoFile()
           overallStatus.remove_status_file()
           for node in MirrorNodeList:
               for segStatus in node.statusList:
                   segStatus.remove_status_file()
           RemoveDirectory.local("gpupgrademirror remove %s" % options.info_data_directory
                                , options.info_data_directory
                                )
           """
    
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
            remove_pid_file(options.info_data_directory)
        try:
            if gpstartStatus != None and gpstartStatus == "MASTER":
               GpStop.local('gpupgrademirror stop databse', fast=True, masterOnly = True)
               gpstartStatus = "STOPPED"
        except Exception:
            pass         
    except NameError:
        pass
    logger.info("gpupgrademirror complete")
