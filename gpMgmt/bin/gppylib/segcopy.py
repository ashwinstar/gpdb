#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

import os
import socket

from gppylib.gplog import *
from gppylib.commands.unix import *
from gppylib.commands.base import *
from gppylib.commands.gp import *
from gppylib.db import dbconn

class SegcopyError(Exception):  pass

class GpSegCopy:
    
    def __init__(self,logger,srcHost,srcDir,dstHost,dstDir,clean,schema_only,method):
        self.logger=logger
        self.srcHost=srcHost
        self.srcDir=srcDir
        self.dstHost=dstHost
        self.dstDir=dstDir
        self.clean=clean
        self.schema_only=schema_only
        self.method=method
        self.masterDataDir = os.getenv("MASTER_DATA_DIRECTORY")    
        
    
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
        
        pass
    
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
            pysyncCmd=PySync(name,self.srcDir,self.dstHost,self.dstDir,ctxt=REMOTE,remoteHost=self.srcHost)
            pysyncCmd.run(validateAfter=True)
            res=pysyncCmd.get_results()
            
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
            
            startCmd=SegmentStart(name,self.dstDir, utilityMode=True, port=templatePort, ctxt=REMOTE, remoteHost=self.dstHost)
            stopCmd=SegmentStop(name, self.dstDir, ctxt=REMOTE, remoteHost=self.dstHost)
            
            startCmd.run(validateAfter=True)

            dburl = dbconn.DbURL(hostname=self.dstHost,port=templatePort)
            logger.info("connecting to database: %s" % dburl)
            conn = dbconn.connect(dburl, True)
            
            # TODO: add other catalog tables
            table_names = [
                           'pg_catalog.gp_configuration', 
                           'pg_catalog.gp_distribution_policy',
                           #'pg_catalog.gp_id',
                           'pg_catalog.gp_master_mirroring',
                           ]
            # Can't truncate system catalog table
            q = 'DELETE FROM %s'
            for table in table_names:
                logger.info("Cleaning up %s" % table)
                dbconn.execSQL(conn, q % table)
            conn.commit()
            conn.close()
            
            stopCmd.run(validateAfter=True)
            res=stopCmd.get_results()
            
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
            remote_bytes_avail=DiskFree.get_size(name,host,directory)
            
            
        #TODO:  this logic isn't quite correct in the 'sync' case since we might
        #       not need to copy anything.  
        if remote_bytes_avail <= bytesRequired:
            raise SegcopyError, "Not enough space on destination host: '%s' directory:"\
                                "'%s'.  Currently %d bytes free but need %d bytes." \
                                % (host,directory,remote_bytes_avail, bytesRequired)
        
    
        logger.info("srcDir has: %d bytes in it and dstDir's filesystem has: %d available bytes" \
                     % (bytesRequired,remote_bytes_avail))
