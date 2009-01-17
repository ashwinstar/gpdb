#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

from gppylib.gplog import *
from gppylib.commands.unix import *
from gppylib.commands.base import *
from gppylib.commands.gp import *

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
        
        if self.schema_only:            
            """ basic logic:
            1) start src DB
            2) interrogate schema on src DB.  
                - heap tables -AO tables - indexes
            3) create dummy heap,AO,index and store created file as a template.
            4) use info from #2 and #3 to create a local copy
            5) copy it whereever.                        
            """
            
            #TODO: impl. this!
            pass
        
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
        #TODO: Fix up this validation.  du returns blocks, not bytes.
        dstDirSize=DiskUsage.get_size(name,self.dstHost,self.dstDir)
        
        if srcDirSize != dstDirSize:
            raise SegcopyError, "Copy completed but byte count doesn't match!\n"\
                  "%s:%s has: %d bytes and %s:%s has: %d bytes"\
                    % (self.srcHost,self.srcDir,srcDirSize,self.dstHost,self.dstDir,dstDirSize)
            
        logger.info("Copy Complete.  %s:%s has: %d bytes and %s:%s has: %d bytes"\
                    % (self.srcHost,self.srcDir,srcDirSize,self.dstHost,self.dstDir,dstDirSize))
        logger.info("Copy complete.")
    
   
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
