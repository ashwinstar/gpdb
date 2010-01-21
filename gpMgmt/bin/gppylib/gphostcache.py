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
    def __init__(self, hostname):
        self.hostname = hostname
        self.dbs=[]
    
    def addDB(self,db):
        self.dbs.append(db)

    def __str__(self):
        dirlist=[]
        for db in self.dbs:
            dirlist.append(db.datadir)
        return "Host %s has Datadirs: [%s]" % (self.hostname,','.join(dirlist))
                

class GpHostCache():
    
    def __init__(self, gparray, pool, skiplist=None):
        self.gparray=gparray
        self.hostcache={}         # interface -> hostname
        self.gphost_map={}        # hostname -> GpHost

        # these are any db's that should be skipped.
        skipmap={}
        for db in skiplist:
            skipmap[db.getSegmentDbId()]=db
        
        # Read the .gphostcache file if it exists
        #   Todo: handle failure more cleanly Should be able to deal with:
        #     - lack of permissions on file
        #     - file in unexpected format
        if os.path.isfile(CACHEFILE):
            try:
                fp = open(CACHEFILE, 'r')
                for line in fp:
                    (interface, hostname) = line.split(':')
                    self.hostcache[interface.strip()]=hostname.strip()
            except Exception, e:
                logger.warn(str(e))
                logger.warn("Error reading file '%s'" % CACHEFILE)
        
        # Go through the gparray and build the interface->hostname lookup table
        #
        # As Greeenplum 3.4 we have both interface and hostname information in
        # the catalog, so the gparray should be able to supply all of the 
        # information.  
        #
        # However if we have initialized from an old catalog, or from a flatfile
        # then the interface->hostname mapping may not be available.  In this 
        # case we still want to run this pass first so that we don't lookup 
        # a given interface more than once.
        inconsistent = []
        for db in self.gparray.getSegDbList():
            if db.getSegmentDbId() not in skipmap:
                interface = db.getSegmentAddress()
                hostname  = db.getSegmentHostName()

                # If we don't have this mapping yet set it, otherwise we
                # simply validate consistency.
                if interface not in self.hostcache:
                    self.hostcache[interface] = hostname
                elif self.hostcache[interface] is None:
                    self.hostcache[interface] = hostname
                elif self.hostcache[interface] != hostname:
                    logger.warn("inconsistent hostname '%s' for interface '%s' and expected hostname '%s'" % \
                                (self.hostcache[interface], interface, hostname))
                    inconsistent.append(interface)

        # Clear out any inconsistent hostnames to force a recheck.
        for i in inconsistent:
            self.hostcache[i] = None

        # Lookup any hostnames that we don't have answers for:
        pending_cmds={}
        for interface in self.hostcache:
            if self.hostcache[interface] is None:
                logger.debug("hostname lookup for %s" % interface)
                cmd=unix.Hostname('host lookup', ctxt=base.REMOTE, remoteHost=interface)
                pool.addCommand(cmd)
                pending_cmds[interface] = cmd

        # Fetch the results out of the WorkerPool
        if len(pending_cmds) > 0:
            pool.join()

            for interface in pending_cmds:
                cmd = pending_cmds[interface]

                # Make sure the command completed successfully
                if cmd.get_results().rc != 0:
                    logger.warn("Failed to resolve hostname for %s" % interface)
                    continue

                self.hostcache[interface] = cmd.get_hostname()

            pool.empty_completed_items()

        # Build up the GpHosts using our interface->hostname lookup
        for db in self.gparray.getSegDbList():
            # skip this dbid ?
            if db.getSegmentDbId() in skipmap:
                continue
            interface = db.getSegmentAddress()
            hostname  = self.hostcache[interface]   # should set the db?
            if hostname not in self.gphost_map:
                self.gphost_map[hostname] = GpHost(hostname)

            self.gphost_map[hostname].addDB(db)

        # Try to update the hostcache file if we executed any hostname commands
        if len(pending_cmds) > 0:
            try:
                fp = open(CACHEFILE, 'w')
                
                for interface in sorted(self.hostcache.keys()):
                    hostname = self.hostcache[interface]

                    # skip any dangling references we still have
                    if not hostname:
                        continue
                    fp.write("%s:%s\n" % (interface, hostname))

                fp.close()
            except Exception, e:
                logger.warn(str(e))
                logger.warn("Failed to write file '%s'" % CACHEFILE)
                
    ######
    def log_contents(self):
        logger.debug("Construct host-->datadirs mapping:")
        
        entries=[]
        for key in self.gphost_map.keys():
            gphost=self.gphost_map[key]
            entries.append(gphost.__str__())
        
        logger.debug('\n'.join(entries))
    
    ######
    def get_hostnames(self):
        hosts=[]
        for key in self.gphost_map.keys():
            gphost=self.gphost_map[key]
            hosts.append(gphost.hostname)
        return hosts
    
    ######
    def get_hosts(self):
        return self.gphost_map.values()
    
    
    ######
    def get_host(self,hostname):
        if hostname in self.gphost_map:
            return self.gphost_map[hostname]
        else:
            raise Exception("map does not contain host: " % hostname)
   
    #####
    def ping_hosts(self, pool):
        ''' 
        go through all of the gphosts and try and ping all of the hosts. 
        If any fail then go back to using the interface names for those 
        segments.  
        
        throws an Exception if still can't ping on the interface names. 
        '''
        failed_segs=[]
        for key in self.gphost_map.keys():
            p = unix.Ping('ping',key)
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
                    dbid = db.getSegmentDbId()
                    hostname = db.getSegmentHostName()

                    pingCmd = unix.Ping("dbid: %d" % dbid, hostname)
                    pingCmd.run()
                    if pingCmd.get_results().rc == 0:
                        alternateHost=hostname
                        break
                    else:
                        logger.warning("Ping to host: '%s' FAILED" % hostname)
                        logger.debug("  ping details: %s" % pingCmd)
                
                if alternateHost:
                    gphost.hostname=alternateHost                    
                else:
                    #no interface to reach any of the segments
                    for db in dblist:
                        s = (db,False,"Failed to Ping Host",cmd)                        
                        failed_segs.append(s)
                    pass
                
        pool.empty_completed_items()
        

######
def ping_segment_hosts(gparray,pool):
    failedSegs=[]
    hosts_pinged={}

    pool.empty_completed_items()
    for db in gparray.get_valid_segdbs():
        hostname = db.getSegmentHostName()

        if hostname not in hosts_pinged:
            pool.addCommand(unix.Ping('ping test',hostname,obj=db))
            hosts_pinged[hostname]=db                             
                                 
    pool.join()        
    cmds=pool.getCompletedItems()
        
    for cmd in cmds:
        if cmd.get_results().rc != 0:
            failedSegs.append(cmd.obj)                        
            logger.info("Ping to host: '%s' FAILED" % cmd.obj.getSegmentHostName())
    return failedSegs
        
