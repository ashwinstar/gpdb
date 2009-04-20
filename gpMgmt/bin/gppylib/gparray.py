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
from gppylib.db import dbconn
    

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
    
    def __str__(self):
        return "%s:%s:content=%s:dbid=%s:valid=%s" % (self.hostname,self.datadir,self.content,self.dbid,self.valid)
   
    def __repr__(self):
        return self.__str__()
    
    def get_hostname(self):
        return self.hostname
    
    def get_datadir(self):
        return self.datadir
    
    def get_status(self):
        if self.valid:
            return "Valid"
        else:
            return "Invalid"

    def get_gp_configuration_string(self):
        return '%d|%s|%d|%s|%s|%s|%d|%s' % ( self.content,
                                            't' if self.definedprimary else 'f',
                                            self.dbid,
                                            't' if self.isprimary else 'f',
                                            't' if self.valid else 'f',
                                            self.hostname,
                                            self.port,
                                            self.datadir)


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
def createGPDBFromDB(dbURL, utility=False):
    """Constructor for building gparray from provided database URL"""    
    conn = dbconn.connect(dbURL,utility)
    rows = dbconn.getConfig(conn)
    array =  GpArray(rows)
    conn.close()
    return array

    
def createFromFile(inputFile):
    """TODO: """
        
    f = open(inputFile, 'r')
    rows=[]
    
    for line in f:
        rows.append(dbconn.parseLine(line))
    
    return GpArray(rows)

def dumpToFile(gparray, filename):
    try:
        fp = open(filename, 'w')
        if gparray.master:
            fp.write(gparray.master.get_gp_configuration_string() + '\n')
        if gparray.standbyMaster:
            fp.write(gparray.standbyMaster.get_gp_configuration_string() + '\n')
        for gpdb in gparray.getSegDbList():
            fp.write(gpdb.get_gp_configuration_string() + '\n')
    except IOError:
        raise
    finally:
        if fp: fp.close()



def createFromGpInitFile(intputFile):
    """TODO: """
    raise Exception,"TODO: implement createFromGpInitFile"

def createSegmentRows(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase,dir_prefix):
    rows=[]
    
    dbid = 0
    content = 0
    #primaries
    for host in hostlist:
        isprimary='t'
        port=primary_portbase        
        for dir in primary_list:   
            fulldir = "%s/%s%d" % (dir,dir_prefix,content)                             
            rows.append( (content,isprimary,dbid,isprimary,'t',host,port,fulldir) )
            port += 1
            content += 1
            dbid += 1
    
    #mirrors
    if mirror_type is None or mirror_type == 'none':
        return rows
    elif mirror_type.lower().strip() == 'spread':
        #TODO: must be sure to put mirrors on a different subnet than primary.
        #      this is a general problem for GPDB these days. perhaps we should
        #      add something to gpdetective to be able to detect this and fix it.
        #      best to have the interface mapping stuff 1st.
        content=0
        isprimary='f'
        num_hosts = len(hostlist)
        num_dirs=len(primary_list)
        if num_hosts <= num_dirs:
            raise Exception("Not enough hosts for spread mirroring.  You must have more hosts then primary segments per host")
        
        mirror_port = {}

        for host in hostlist:
            mirror_host_offset=1
            for dir in mirror_list:
                fulldir = "%s/%s%d" % (dir,dir_prefix,content)
                mirror_host = hostlist[mirror_host_offset % num_hosts]
                if mirror_host == host:
                    mirror_host_offset += 1
                    mirror_host = hostlist[mirror_host_offset % num_hosts]
                    
                if not mirror_port.has_key(mirror_host):
                    mirror_port[mirror_host] = mirror_portbase
                    
                rows.append( (content,isprimary,dbid,isprimary,'t',mirror_host,mirror_port[mirror_host],fulldir) )
                mirror_port[mirror_host] += 1
                content += 1
                dbid += 1
                mirror_host_offset += 1
                
        
    elif mirror_type.lower().strip() == 'grouped':
        content = 0
        num_hosts = len(hostlist)
        
        if num_hosts < 2:
            raise Exception("Not enough hosts for grouped mirroring.  You must have at least 2")
        
        #we'll pick our mirror host to be 1 host "ahead" of the primary.
        mirror_host_offset = 1
        
        isprimary='f'
        for host in hostlist:            
            mirror_host = hostlist[mirror_host_offset % num_hosts]
            mirror_host_offset += 1
            port = mirror_portbase
            
            for dir in mirror_list:
                fulldir = "%s/%s%d" % (dir,dir_prefix,content)
                rows.append( (content,isprimary,dbid,isprimary,'t',mirror_host,port,fulldir) )
                port += 1
                content += 1
                dbid += 1
        
    else:
        raise Exception("Invalid mirror type specified: %s" % mirror_type)
    
    return rows

def parseTrueFalse(value):
    if value.lower() == 'f':
        return False
    else:
        return True

    
class GpArray:
    """ TODO: 
    
    """
    arrayName=None
    numSegments=0
    master=None
    standbyMaster=None
    segments=None
    expansionSegments=None
    
    
    def __init__(self,rows):
        """TODO: """
        
        self.arrayName=''
        self.segments=[]
        self.expansionSegments=[]
        self.numPrimarySegments = 0
        
        for row in rows:         
            content=row[0]
            definedprimary=row[1]
            dbid=int(row[2])
            isprimary=row[3]
            valid=row[4]
            hostname=row[5]
            port=int(row[6])
            datadir=row[7]
            
            
            if self.master is None and content is not -1:
                logger.error("FATAL - no master dbs defined!")
                sys.exit(1)
                
            #TODO: what about dbids???
            
            if content is -1:
                if definedprimary is True:
                    self.master = GpDB(-1,definedprimary,dbid,isprimary,valid,
                                           hostname,port,datadir)
                else:
                    self.standbyMaster = GpDB(-1,definedprimary,dbid,isprimary,valid,
                                                  hostname,port,datadir)
            else:
                segdb = GpDB(content,definedprimary,dbid,
                             isprimary,valid,hostname,port,datadir)

                
                if len(self.segments) <= content:
                    self.segments.insert(content, Segment())
                
                seg = self.segments[content]                
                if definedprimary is True:                    
                    seg.addPrimary(segdb)
                    self.numPrimarySegments += 1
                else:             
                    seg.addMirror(segdb)
        self.numSegments=len(self.segments)
        
    
    def getDbList(self, includeExpansionSegs=False):
        """Return a list of all GpDb objects that make up the array"""
        list=[]
        list.append(self.master)
        if includeExpansionSegs:
            list.extend(self.getSegDbList(True))
        else:
            list.extend(self.getSegDbList())

        return list
    
    def getSegDbList(self, includeExpansionSegs=False):
        """Return a list of all GpDb objects for all segments in the array"""
        list=[]
        for seg in self.segments:
            list.extend(seg.get_dbs())
        if includeExpansionSegs:
           for seg in self.expansionSegments:
               list.extend(seg.get_dbs()) 
        return list
    
    def getExpansionSegDbList(self):
        """Returns a list of all GpDb objects that make up the new segments
        of an expansion"""
        list=[]
        for seg in self.expansionSegments:
            list.extend(seg.get_dbs())
        return list
    
    def get_invalid_segdbs(self):
        list=[]
        for seg in self.segments:
            segdb = seg.primaryDB
            if not segdb.valid:
                list.append(segdb)
            for db in seg.mirrorDBs:
                if not db.valid:
                    list.append(db)
        return list
    
    def get_valid_segdbs(self):
        list=[]
        for seg in self.segments:
            db = seg.primaryDB
            if db.valid:
                list.append(db)
            for db in seg.mirrorDBs:
                if db.valid:
                    list.append(db)
        return list
        
        
    def get_hostlist(self, includeMaster=True):
        list=[]
        if includeMaster:
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
   
   
    def get_max_dbid(self,includeExpansionSegs=False):
        """Returns the maximum dbid in the array.  If includeExpansionSegs
        is True, this includes the expansion segment array in the search"""
        dbid = 0
        
        for db in self.getDbList(includeExpansionSegs):
            if db.dbid > dbid:
                dbid = db.dbid
        
        return dbid
    
    def get_max_contentid(self, includeExpansionSegs=False):
        """Returns the maximum contentid in the array.  If includeExpansionSegs
        is True, this includes the expansion segment array in the search"""
        content = 0
        
        for db in self.getDbList(includeExpansionSegs):
            if db.content > content:
                content = db.content
                
        return content
   
    def get_segment_count(self):
        return len(self.segments)
    
    def get_primary_count(self):
        return self.numPrimarySegments
        
    def get_mirroring_enabled(self):
        """Returns True is mirrors are defined"""
        return len(self.segments[0].mirrorDBs) != 0
    
    def get_min_primary_port(self):
        """Returns the minimum primary segment db port"""
        min_primary_port = self.segments[0].primaryDB.port
        for seg in self.segments:
            if seg.primaryDB.port < min_primary_port:
                min_primary_port = seg.primaryDB.port
        
        return min_primary_port
    
    def get_max_primary_port(self):
        """Returns the maximum primary segment db port"""
        max_primary_port = self.segments[0].primaryDB.port
        for seg in self.segments:
            if seg.primaryDB.port > max_primary_port:
                max_primary_port = seg.primaryDB.port
        
        return max_primary_port
    
    def get_min_mirror_port(self):
        """Returns the minimum mirror segment db port"""
        if self.get_mirroring_enabled() is False:
            raise Exception('Mirroring is not enabled')
        
        min_mirror_port = self.segments[0].mirrorDBs[0].port
        
        for seg in self.segments:
            for db in seg.mirrorDBs:
                if db.port < min_mirror_port:
                    min_mirror_port = db.port
        
        return min_mirror_port
    
    def get_max_mirror_port(self):
        """Returns the maximum mirror segment db port"""
        if self.get_mirroring_enabled() is False:
            raise Exception('Mirroring is not enabled')
        
        max_mirror_port = self.segments[0].mirrorDBs[0].port
        
        for seg in self.segments:
            for db in seg.mirrorDBs:
                if db.port > max_mirror_port:
                    max_mirror_port = db.port
        
        return max_mirror_port

    
    def get_interface_numbers(self):
        """Returns interface numbers in the array.  Assumes that nodes are named
        <hostname>-<int_num>.  If the nodes just have <hostname> then an empty
        array is returned."""
        # Get the first segment hostname minus the interface number
        interface_nums = []
        primary_hostname = self.segments[0].primaryDB.hostname
        (host_part, inf_num) = ['-'.join(primary_hostname.split('-')[:-1]), primary_hostname.split('-')[-1]]
        if host_part == '' or not inf_num.isdigit():
            return []
        
        search_host_part = host_part
        
        # get the interface numbers for host_part
        for db in self.getDbList():
            (host_part, inf_num) = ['-'.join(db.hostname.split('-')[:-1]), db.hostname.split('-')[-1]]
            if host_part == search_host_part:
                if not inf_num.isdigit():
                    # We have an unexpected formatted hostname
                    return []
                else:
                    if inf_num not in interface_nums:
                        interface_nums.append(inf_num)
        
        return interface_nums
    
    def get_primary_root_datadirs(self):
        """Returns a list of primary data directories minus the <prefix><contentid>"""
        primary_datadirs = []

        seg0_hostname = self.segments[0].primaryDB.get_hostname()
        if len(self.get_interface_numbers()) > 0:
            seg0_hostname = seg0_hostname[:seg0_hostname.rfind('-')]
        
        for db in self.getDbList():
            if db.definedprimary and db.content != -1 and \
                db.get_hostname().startswith(seg0_hostname):
                
                primary_datadirs.append(db.datadir[:db.datadir.rfind('/')])
        
        return primary_datadirs
    
    def get_mirror_root_datadirs(self):
        """Returns a list of mirror data directories minus the <prefix><contentid>"""
        mirror_datadirs = []

        seg0_hostname = self.segments[0].primaryDB.get_hostname()
        if len(self.get_interface_numbers()) > 0:
            seg0_hostname = seg0_hostname[:seg0_hostname.rfind('-')]

        for db in self.getDbList():
            if not db.definedprimary and db.content != -1 and \
                db.get_hostname().startswith(seg0_hostname):
                mirror_datadirs.append(db.datadir[:db.datadir.rfind('/')])
        
        return mirror_datadirs
    
    def get_datadir_prefix(self):
        """Returns the prefix portion of <prefix><contentid>"""
        start_last_dir = self.master.datadir.rfind('/') + 1
        start_dir_content = self.master.datadir.rfind('-')
        prefix = self.master.datadir[start_last_dir:start_dir_content]
        return prefix
    
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
    
    
    def addExpansionSeg(self, content, definedprimary, dbid, isprimary, hostname, port, datadir):
        """ Adds a segment to the gparray as an expansion segment.
        """
        if (content <= self.segments[-1].get_dbs()[0].content):
            raise Exception('Invalid content ID for expansion segment')
        
        segdb = GpDB(content,definedprimary,dbid,isprimary,
                     True,hostname,port,datadir)
        
        seglen = len(self.segments)
        expseglen = len(self.expansionSegments)
        
        expseg_index = content - seglen
        logger.debug('New segment index is %d' % expseg_index)
        if expseglen < expseg_index + 1:
            logger.debug('Extending expansion array by %d' % (expseglen - expseg_index + 1))
            self.expansionSegments.extend([None] * (expseglen - expseg_index + 1))
            self.expansionSegments[expseg_index] = Segment()

        seg = self.expansionSegments[expseg_index]
        if definedprimary is True: 
            if seg.primaryDB:
                raise Exception('Duplicate content id for primary segment')                   
            seg.addPrimary(segdb)
        else:             
            seg.addMirror(segdb)
    
            
    def validateExpansionSegs(self):
        """ Checks the segments added for various inconsistencies and errors.
        """
        dbids = []
        content = []
        expansion_seg_count = 0
       
        # make sure we have added at least one segment
        if len(self.expansionSegments) == 0:
            raise Exception('No expansion segments defined')
        
        # how many mirrors?
        mirrors_per_segment = len(self.segments[0].mirrorDBs)
            
        for seg in self.expansionSegments:
            # If a segment is 'None' that means we have a gap in the content ids
            if seg is None:
                raise Exception('Expansion segments do not have contiguous content ids.')
            
            expansion_seg_count += 1
            
            for segdb in seg.get_dbs():
                dbids.append(segdb.dbid)
                content.append((segdb.content, segdb.isprimary))
            
            # mirror count correct for this content id?
            if mirrors_per_segment > 0:
                if len(seg.mirrorDBs) != mirrors_per_segment:
                    raise Exception('Expansion segment has incorrect number of mirrors defined.')
            else:
                #shouldn't have any mirrors
                if len(seg.mirrorDBs) != 0:
                    raise Exception('Expansion segment has a mirror segment defined but mirroring is not enabled.')
        
        # check that the dbids are what they should be
        dbids.sort()

        if dbids[0] != self.get_max_dbid() + 1:
            raise Exception('Expansion segments have incorrect dbids')
        for i in range(0, len(dbids) - 1):
            if dbids[i] != dbids[i + 1] - 1:
                raise Exception('Expansion segments have incorrect dbids')


        # check that content ids are ok
        valid_content = []
        for i in range(self.segments[-1].primaryDB.content + 1, 
                       self.segments[-1].primaryDB.content + 1 + len(self.expansionSegments)):
            valid_content.append((i, True))
            for j in range(0, mirrors_per_segment):
                valid_content.append((i, False))
        
        valid_content.sort(lambda x,y: cmp(x[0], y[0]) or cmp(x[1], y[1]))
        content.sort(lambda x,y: cmp(x[0], y[0]) or cmp(x[1], y[1]))
        
        if valid_content != content:
            raise Exception('Invalid content ids')
        
        # Check for redefinition of original segments
        original_datadirs = {}
        for segdb in self.getDbList():
            original_datadirs[segdb.get_hostname() + segdb.get_datadir()] = True
            
        for segdb in self.getExpansionSegDbList():
            if original_datadirs.has_key(segdb.get_hostname() + segdb.get_datadir()):
                raise Exception('Segment %s:%s existed in the original array' % (sebdb.get_hostname(), segdb.get_datadir()))
        
        pass
    
    
    def addExpansionHosts(self, hosts, mirror_type):
        """ Adds a list of hosts to the array, using the same data
        directories as the original hosts.  Also adds the mirrors
        based on mirror_type.
        """
        primary_dirs = self.get_primary_root_datadirs()
        mirror_dirs = self.get_mirror_root_datadirs()

        interface_list = self.get_interface_numbers()
        
        base_primary_port = self.get_min_primary_port()
        base_mirror_port = 0
        if mirror_type != 'none':
            base_mirror_port = self.get_min_mirror_port()
        
        prefix = self.get_datadir_prefix()
        
        rows = createSegmentRows(hosts, primary_dirs, base_primary_port, mirror_type, mirror_dirs, base_mirror_port, prefix)
        
        self._fixup_and_add_expansion_segments(rows, interface_list)
        
    
    def addExpansionDatadirs(self, datadirs, mirrordirs, mirror_type):
        """ Adds new segments based on new data directories to both original
        hosts and hosts that were added by addExpansionHosts.
        """
        max_primary_port = self.get_max_primary_port()
        if mirror_type != 'none':
            max_mirror_port = self.get_max_mirror_port()
        else:
            max_mirror_port = 0

        interface_list = self.get_interface_numbers()
        interface_count = len(interface_list)
            
        prefix = self.get_datadir_prefix()
          
        hosts = []
        # Get all the hosts to add the data dirs to
        for seg in self.getSegDbList(True):
            host = seg.get_hostname()
            
            # If we have interface numbers, strip them so we only add data
            # dirs once per node
            if interface_count > 0:
                host = host[:host.rfind('-')]
        
            if host not in hosts:
                hosts.append(host)
                
        # Create the rows
        rows = createSegmentRows(hosts, datadirs, max_primary_port + 1, 
                                 mirror_type, mirrordirs, max_mirror_port + 1,
                                 prefix)
        
        self._fixup_and_add_expansion_segments(rows, interface_list)
        
    
    def _fixup_and_add_expansion_segments(self, rows, interface_list):
        """Fixes up expansion segments added to be after the original segdbs 
        This includes fixing up the dbids, content ids, data directories, 
        interface part of the hostnames and mirrors.  After this is done, it
        adds them to the expansion array."""
        interface_count = len(interface_list)

        mirror_dict = {}
        # must be sorted by isprimary, then hostname
        rows.sort(lambda a,b: (cmp(b[1], a[1]) or cmp(a[5],b[5])))
        current_host = rows[0][5]
        current_interface = 0
        curr_dbid = self.get_max_dbid(True) + 1
        curr_content = self.get_max_contentid(True) + 1
        
        # Fix up the rows with correct dbids, contentids, datadirs and interfaces
        for row in rows:
            hostname = row[5]
            
            # If we have interface hostnames in the original segments, 
            # adjust the new segment hostname
            if interface_count > 0:
                if current_host != hostname:
                    current_host = hostname
                    current_interface = 0

                if current_interface == interface_count:
                    current_interface = 0
                
                hostname = '%s-%s' % (current_host, interface_list[current_interface])
                current_interface += 1
     
            # Add the new segment to the expansion segments array
            # Remove the content id off of the datadir
            new_datadir = row[7][:row[7].rfind(str(row[0]))]
            if row[1] == 't':
                new_datadir += ('%d' % curr_content)
                self.addExpansionSeg(curr_content, True, curr_dbid, 
                                        True, hostname, int(row[6]), new_datadir)
                # The content id was adjusted, so we need to save it for the mirror
                mirror_dict[row[0]] = curr_content
                curr_content += 1
            else:
                new_content = mirror_dict[row[0]]
                new_datadir += ('%d' % new_content)
                self.addExpansionSeg(new_content, False, curr_dbid, 
                                     False, hostname, int(row[6]), new_datadir)
            curr_dbid += 1           
#------------------------------- Mainline --------------------------------
