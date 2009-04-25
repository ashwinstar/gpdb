#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
'''
Greenplum hostcache file facilities.

This Module contains some helper functions for mapping network
interface names used in gp_configuration to a collapsed set
of hostnames.

example:  sdw1-1, sdw1-2, sdw1-3, and sdw1-4 are all located 
          on sdw1.
          
The results of this collapsing will be stored in a file: 
~/.gphostcache with entries of the form:

sdw1-1:sdw1
sdw1-2:sdw1
sdw1-3:sdw1

A big complication here is that we want to group all of the 
segment databases for sdw1-1 thru sdw1-4 together but we can't
use the name returned by `hostname` as its not guaranteed to
have a trusted ssh environment setup for it.  

'''
import os

from gppylib import gparray
from gppylib.commands import base
from gppylib.commands import unix
from gppylib import gplog
FILEDIR=os.path.expanduser("~")
FILENAME=".gphostcache"
CACHEFILE=FILEDIR + "/" + FILENAME

logger = gplog.get_default_logger()



class GpHost:
    def __init__(self,hostname):
        self.hostname=hostname
        self.dbs=[]
        pass    
    
    def addDB(self,db):
        self.dbs.append(db)
        
    def __str__(self):
        dirlist=[]
        for db in self.dbs:
            dirlist.append(db.datadir)
        return "Host %s has Datadirs: [%s]" % (self.hostname,','.join(dirlist))
                

class GpHostCache():
    
    def __init__(self,gparray,pool):
        self.gparray=gparray
        self.hostcache={}
        self.gphost_map={}
        
        self._readfile(pool)
        
        pass
    
    
    def _readfile(self,pool):
        
        if os.path.isfile(CACHEFILE):
            #file exists so we read it in.
            try:
                file = open(CACHEFILE, 'r')
                
                for line in file:
                    (interface,hostname)=line.split(':')
                    self.hostcache[interface.strip()]=hostname.strip()                    
            except IOError:
                raise Exception(' file %s not found' % path)                    
            finally:
                file.close()
        
        #now we go through our gparray and create a set of GpHost to store all of
        # the segment dbs in.
        pending_cmds={}  
        for seg in self.gparray.segments:
            for db in seg.get_dbs():
                #we process both valid and invalid db's                
                if db.hostname in self.hostcache:                        
                    normalized_hostname=self.hostcache[db.hostname]
                    self._add_hostname(normalized_hostname,db)
                else:
                    #we don't have an entry, so farm off a command to figure it out.
                    # we just queue up the commands here and let the pool start to 
                    # execute them.  later we'll process through and farm the results.
                    cmd=unix.Hostname('host lookup',ctxt=base.REMOTE,remoteHost=db.hostname)
                    pool.addCommand(cmd)
                    pending_cmds[cmd]=db
                    pass
        
                    
                    
        #go through all of the pending commands and process them.  
        #  we'll also append to our hostcache file as needed so that we don't have
        #  to do this work nexttime.
        hosts_added={}
        if len(pending_cmds) > 0:
            #make sure we let all the commands finish.
            pool.join()
            try:
                file=open(CACHEFILE, 'a')
                
                for key in pending_cmds.keys():
                    cmd=key
                    db=pending_cmds[key]
                    
                    #make sure the command completed succesfully.                    
                    cmd.validate()
                    
                    normalized_hostname = cmd.get_hostname()
                    self._add_hostname(normalized_hostname,db)      
                    if db.hostname not in hosts_added:              
                        file.write("%s:%s\n" % (db.hostname,normalized_hostname))
                        hosts_added[db.hostname]=True
            except IOError:
                raise Exception(' file %s not found' % path)                    
            finally:
                file.close()    
            
    ######
    def _add_hostname(self,normalized_hostname,db):
        if normalized_hostname in self.gphost_map:
            gphost=self.gphost_map[normalized_hostname]
            gphost.addDB(db)
            return False
        else:
            gphost=GpHost(normalized_hostname)
            gphost.addDB(db)
            self.gphost_map[normalized_hostname]=gphost
            return True
    
    ######
    def print_contents(self,logger):
        logger.debug("Construct host-->datadirs mapping:")
        
        entries=[]
        for key in self.gphost_map.keys():
            gphost=self.gphost_map[key]
            entries.append(gphost.__str__())
        
        logger.debug('\n'.join(entries))
    
    ######
    def get_hostnames(self):
        list=[]
        for key in self.gphost_map.keys():
            gphost=self.gphost_map[key]
            list.append(gphost.hostname)
        return list
    
    ######
    def get_hosts(self):
        return self.gphost_map.values()
    
    
    ######
    def get_host(self,hostname):
        if hostname in self.gphost_map:
            return self.gphost_map[hostname]
        else:
            raise Exception("map does not contain host: " % hostname)
   
    ######
    def remove_host(self,gphost):
        if self.gphost_map.has_key(gphost):             
            del self.hostdict[host]

    #####
    def ping_hosts(self,logger,pool,failed_segs):
        ''' go through all of the gphosts and try and ping all of the hosts.  if any fail
        then go back to using the interface names for those segments.  
        
        throws an Exception if still can't ping on the interface names. 
        '''
        
        for key in self.gphost_map.keys():
            gphost=self.gphost_map[key]
            p = unix.Ping('ping',gphost.dbs[0].hostname)
            pool.addCommand(p)
        
        pool.join()
        cmds=pool.getCompletedItems()
        
        
        for cmd in cmds:
            if cmd.get_results().rc != 0:
                #ping failed...
                hostname=cmd.hostToPing
                logger.warning("Ping to host: '%s' FAILED" % hostname)
                logger.debug("  ping details: %s" % cmd)
                
                gphost=self.get_host(hostname)
                dblist=gphost.dbs
                
                alternateHost=None
                
                for db in dblist:
                    pingCmd = unix.Ping("dbid: %d" % db.dbid,db.hostname)
                    pingCmd.run()
                    if pingCmd.get_results().rc == 0:
                        alternateHost=db.hostname
                        break
                    else:
                        logger.warning("Ping to host: '%s' FAILED" % db.hostname)
                        logger.debug("  ping details: %s" % pingCmd)
                
                if alternateHost:
                    gphost.hostname=alternateHost                    
                else:
                    #no interface to reach any of the segments
                    for db in dblist:
                        s = SegStartStatus(db,started=False,reason="Failed to Ping Host",failedCmd=cmd)
                        failed_segs.append(s)
                    pass
                
        pool.empty_completed_items()
        
        
        
        