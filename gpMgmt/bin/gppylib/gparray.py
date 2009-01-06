#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
import os, sys
from datetime import date

from gppylib.commands.base import *
from gppylib.commands.unix import *
from gppylib.commands.pg import *
from gppylib.commands.gp import *
from gppylib.gplog import *
from gppylib import gpconfig

    

class GpDB:
    content=-2
    definedprimary=False
    dbid=-2
    isprimary=False
    valid=False
    hostname=None
    port=0
    datadir=None
    
    def __init__(self,content,definedprimary,dbid,isprimary,valid,hostname,port,datadir):
        self.content=content
        self.definedprimary=definedprimary
        self.dbid=dbid
        self.isprimary=isprimary
        self.valid=valid
        self.hostname=hostname
        self.port=port
        self.datadir=datadir
    
   

    def get_hostname(self):
        return self.hostname
    
    def get_datadir(self):
        return self.datadir



class Segment:
    """ Used to represent all of the SegmentDBs with the same contentID.  Today this
    can be at most a primary SegDB and a single mirror SegDB.  In the future we
    will most likely support multiple mirror segDBs.
    """
    primaryDB=None
    mirrorDBs=None
    
    def __init__(self):
        self.mirrorDBs=[]
        pass

    def addPrimary(self,segDB):
        self.primaryDB=segDB
    
    def addMirror(self,segDB):
        self.mirrorDBs.append(segDB)

    def hasFailedComponents(self):
        if self.primaryDB.valid is False:
            return True
            
        for mirror in self.mirrorDBs:
            if mirror is not None and mirror.valid is False:
                return True
        
        return False
    
    def getFailedComponentReport(self):
        out=''
        if self.primaryDB.valid is False:
            out += "Primary Segment with content %d is down. Located at: %s " % \
                    (self.primaryDB.content,self.primaryDB.getLocation())
            
        for mirror in self.mirrorDBs:
            if mirror is not None and mirror.valid is False:
                out += "Mirror Segment with content %d is down. Located at: %s " % \
                    (self.primaryDB.content,mirror.getLocation())

        return out
    
    def get_dbs(self):
        list=[]
        list.append(self.primaryDB)
        if len(self.mirrorDBs) > 0:
            list.extend(self.mirrorDBs)
        return list

    def get_hosts(self):
        list=[]
        list.append(self.primaryDB.hostname)
        for m in self.mirrorDBs:
            list.append(m.hostname)
        return list

    
#--- factory methods for creating GpArray objects.
def createGPDBFromDB(dbURL):
    """Constructor for building gparray from provided database URL"""    
    conn = pgconn.PgConn.connect(dbUrl)
    rows = gpconfig.getConfig(conn)
    rows = gpconfig.getConfig(conn)
    array =  GpArray(rows)
    conn.close()
    return array

    
def createFromFile(inputFile):
    """TODO: """
        
    f = open(inputFile, 'r')
    rows=[]
    
    for line in f:
        rows.append(gpconfig.parseLine(line))
    
    return GpArray(rows)



def createFromGpInitFile(intputFile):
    """TODO: """
    raise Exception,"TODO: implement createFromGpInitFile"


    
class GpArray:
    """ TODO: 
    
    """
    arrayName=None
    numSegments=0
    master=None
    standbyMaster=None
    segments=None
    
    
    def __init__(self,rows):
        """TODO: """
        
        self.arrayName=''
        self.segments=[]
        
        for row in rows:
            if self.master is None and row['content'] is not -1:
                logger.error("FATAL - no master dbs defined!")
                sys.exit(1)
                
            #TODO: what about dbids???
            
            if row['content'] is -1:
                if row['definedprimary'] is True:
                    self.master = GpDB(-1,row['definedprimary'],row['dbid'],row['isprimary'],row['valid'],
                                           row['hostname'],row['port'],row['datadir'])
                else:
                    self.standbyMaster = GpDB(-1,row['definedprimary'],row['dbid'],row['isprimary'],row['valid'],
                                                  row['hostname'],row['port'],row['datadir'])
            else:
                segdb = GpDB(row['content'],row['definedprimary'],row['dbid'],
                                row['isprimary'],row['valid'],
                                row['hostname'],row['port'],row['datadir'])

                
                if len(self.segments) <= row['content']:
                    self.segments.insert(row['content'], Segment())
                
                seg = self.segments[row['content']]                
                if row['definedprimary'] is True:                    
                    seg.addPrimary(segdb)
                else:             
                    seg.addMirror(segdb)
        self.numSegments=len(self.segments)
        
    
    def getDbList(self):
        """Return a list of all GpDb objects that make up the array"""
        list=[]
        list.append(self.master)
        if self.standbyMaster is not None:
            list.append(self.standbyMaster)
        for seg in self.segments:
            list.extend(seg.get_dbs())
        return list
    
    def get_hostlist(self):
        list=[]
        list.append(self.master.hostname)
        if self.standbyMaster is not None:
            list.append(self.standbyMaster.hostname)        
        for seg in self.segments:
            list.extend(seg.get_hosts())
        return list
   
    def cleanup(self):
        logger.debug("start cleanup")
        self.queue.haltWork()
        self.queue.join()
        logger.debug("finished cleanup")
   
   
    
    def untarSegments(self,queue,filename):        
        logger.debug("untarSegments start")
        
        list=self.getDbList()
        for db in list:
            mkdir_cmd=MakeDirectory("mkdir",db.datadir,ctxt=REMOTE,remoteHost=db.get_hostname())
            queue.addCommand(mkdir_cmd)
        queue.join()
        queue.check_results()
        
        for db in list:
            (head,tail)=os.path.split(db.datadir)
            srcTarFile = "%s/%s" % (head,filename)
            untar_cmd=ExtractTar("untar seg",srcTarFile,db.datadir,ctxt=REMOTE,remoteHost=db.get_hostname())                        
            queue.addCommand(untar_cmd)
        queue.join()
        queue.check_results()        
            
    
    def createArray(self):
        hosts=self.get_hostlist()
        self.queue=WorkerPool(numWorkers=4)
        #TODO: validate software versions        
        run_list(self.queue,hosts,Ping.ping_list)                            
        #TODO: parent directory exists
        #TODO: no directory already there
        InitDB.local("master",self.master)
        filename='%s-%s.tar' % ("master_initdb", date.today().strftime('%Y%m%d'))     
        run_command(CreateTar("master",self.master.datadir,filename))
        
        distribute_tarball(self.queue,list,tarball)                
        self.untarSegments(self.queue,tarball)                  
        #TODO: fix up gp catalog tables.
        #TODO: populate gp_configuration
        self.cleanup()
        pass


#------------------------------- Mainline --------------------------------
