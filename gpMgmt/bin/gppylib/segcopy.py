#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

from __future__ import with_statement
import os
import socket
import sys
import threading

sys.path.append(os.path.join(os.path.dirname(__file__), "..", "..", "..", "bin", "lib"))

from gppylib.gplog import *
from gppylib.commands.unix import *
from gppylib.commands.base import *
from gppylib.commands.gp import *
from gppylib.db import dbconn, catalog
import pysync

class SegcopyError(Exception):  pass


class _SyncProgress:
    
    _markLock = threading.Lock()
    
    def __init__(self, logfunc, gpSegCopy):
        self.logfunc = logfunc
        self.dstHost = gpSegCopy.dstHost
        self.dstDir = gpSegCopy.dstDir
        self._tag = "%s:%s" % (self.dstHost, self.dstDir)
    
    def mark_sync_progress(self, message):
        with _SyncProgress._markLock:
            self.logfunc("%s %s" % (self._tag, message))


class GpSegCopy:
    
    def __init__(self,logger,srcHost,srcDir,dstHost,dstDir,clean,schema_only,method, progressBytes=None, progressTime=None, verbose=False):
        self.logger=logger
        self.srcHost=srcHost
        self.srcDir=srcDir
        self.dstHost=dstHost
        self.dstDir=dstDir
        self.clean=clean
        self.schema_only=schema_only
        self.method=method
        self.masterDataDir = os.getenv("MASTER_DATA_DIRECTORY")
        self.progressBytes = progressBytes
        self.progressTime = progressTime
        self.verbose = verbose
        
    
    def pre_check(self):
        '''Perform up front checks on hosts,directories and other things that might trip us up.

           throws a SegcopyError if any problems occur during validation that require aborting
           the actual copy.
           
           TODO:  should gpsegcopy enforce the fact that the database should be offline? 
                     --am currently thinking no since in the future we may support some online
                       operations of this with some added backend features.
        '''
        Ping.local("srcPing",self.srcHost)
        Ping.local("dstPing",self.dstHost)
                      
        srcDirSize=DiskUsage.get_size("srcDir",self.srcHost,self.srcDir)   
        
        if self.clean:
            RemoveFiles.remote("dstDir",self.dstHost,self.dstDir)
                     
        self.verify_remotedir("dstDir",self.dstHost,self.dstDir, srcDirSize)


    def get_status(self):
        self.logger.critical("TODO: get_status")
        pass
        
    
    def copy(self):
        name="segcopy"
        srcDirSize=DiskUsage.get_size(name,self.srcHost,self.srcDir)
        
        self.logger.info("Starting copy from %s:%s to %s:%s" %\
                         (self.srcHost,self.srcDir,self.dstHost,self.dstDir))
        
        # schema_only sanity check
        if self.schema_only:
            if not self.masterDataDir:
                raise SegcopyError, "Schema only copy requires MASTER_DATA_DIRECTORY environment variable to be set"
            if self.masterDataDir != self.srcDir:
                raise SegcopyError, "Schema only copy requires source directory be master data directory"
        
        if self.method == 'copy':
            #TODO:  may want to change this to either always use pysync or to use a 'tar | ssh tar'
            scpCmd=RemoteCopy(name,self.srcDir,self.dstHost,self.dstDir,ctxt=REMOTE,remoteHost=self.srcHost)
            scpCmd.run(validateAfter=True)
            res=scpCmd.get_results()
        else:

            # Call pysync.LocalPysync directly
            pysyncOptions = []
            if self.verbose:
                pysyncOptions += ["-v"]
            if self.progressBytes:
                pysyncOptions += ["--progress-bytes", self.progressBytes]
            if self.progressTime:
                pysyncOptions += ["--progress-time", self.progressTime]
            pysyncCmd = [pysync.__name__] + pysyncOptions + [self.srcDir, "%s:%s" % (self.dstHost, self.dstDir)]
            sync = pysync.LocalPysync(pysyncCmd, 
                                      recordProgressCallback=_SyncProgress(self.logger.info, self).mark_sync_progress)
            ok = (sync.run() == 0)
            if not ok:
                raise SegcopyError, ("Copy data from (%s:%s) to (%s:%s) failed" 
                                     % (self.srcHost, self.srcDir, self.dstHost, self.dstDir))
        
        #basic validation
        dstDirSize=DiskUsage.get_size(name,self.dstHost,self.dstDir)
        
        if srcDirSize != dstDirSize:
            raise SegcopyError, "Copy completed but byte count doesn't match!\n"\
                  "%s:%s has: %d bytes and %s:%s has: %d bytes"\
                    % (self.srcHost,self.srcDir,srcDirSize,self.dstHost,self.dstDir,dstDirSize)
            
        logger.info("Copy Complete.  %s:%s has: %d bytes and %s:%s has: %d bytes"\
                    % (self.srcHost,self.srcDir,srcDirSize,self.dstHost,self.dstDir,dstDirSize))
        logger.info("Copy complete.")

        if self.schema_only:
            logger.info("Cleaning up catalog for schema only copy on destination")
            # get an available port
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("",0))
            templatePort = s.getsockname()[1]
            s.close()
            logger.debug('Using port %d for segment template cleanup.' % templatePort)
            
            SegmentStart.local('segcopy schema only segment start', 
                               self.dstDir, utilityMode=True, port=templatePort)

            dburl = dbconn.DbURL(hostname=self.dstHost,port=templatePort)
            logger.info("connecting to database: %s" % dburl)
            conn = dbconn.connect(dburl, utility=True)
            
            db_list = catalog.getDatabaseList(conn)
            catalog_names = [
                           'pg_catalog.gp_configuration', 
                           'pg_catalog.gp_distribution_policy',
                           'pg_catalog.pg_partition',
                           'pg_catalog.pg_partition_rule',
                           'pg_catalog.gp_master_mirroring',
                           ]
            
            conn.close()
            for db in db_list:
                if db[0] in ['template0']:
                    continue
                # Can't truncate system catalog table
                dburl.pgdb = db[0]
                conn = dbconn.connect(dburl, utility=True)
                q = 'DELETE FROM %s'
                for table in catalog_names:
                    logger.debug("Cleaning up %s.%s" % (db[0],table))
                    dbconn.execSQL(conn, q % table)
                conn.commit()
                conn.close()
            
            SegmentStop.local(name, self.dstDir)
            
            logger.info("Schema only copy cleanup complete")
   
    #------------ Internal Helper Functions --------------------------------            

    def verify_remotedir(self,name,host,directory,bytesRequired):        
        remote_bytes_avail = 0
        
        if self.method == 'copy':
            #shouldn't be a remote dir there.  so we'll create one and
            #determine the amount of free disk space based on it.
            MakeDirectory.remote(name,host,directory)        
            remote_bytes_avail=DiskFree.get_size(name,host,directory)
            RemoveDirectory.remote(name,host,directory)            
        else:
            # If clean is specified, there will be no existing remote
            # directory; ensure there is one.
            if self.clean:
                MakeDirectory.remote(name, host, directory)
            remote_bytes_avail=DiskFree.get_size(name,host,directory)
            
            
        #TODO:  this logic isn't quite correct in the 'sync' case since we might
        #       not need to copy anything.  
        if remote_bytes_avail <= bytesRequired:
            raise SegcopyError, "Not enough space on destination host: '%s' directory:"\
                                "'%s'.  Currently %d bytes free but need %d bytes." \
                                % (host,directory,remote_bytes_avail, bytesRequired)
        
    
        logger.info("srcDir has: %d bytes in it and dstDir's filesystem has: %d available bytes" \
                     % (bytesRequired,remote_bytes_avail))
