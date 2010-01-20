#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
from gppylib.utils import checkNotNone
from gppylib.utils import checkNotNone
"""
  gparray.py:

    Contains three classes representing configuration information of a
    Greenplum array:

      GpArray - The primary interface - collection of all GpDB within an array
      GpDB    - represents configuration information for a single dbid
      Segment - collection of all GpDB with the same content id
"""

# ============================================================================
from datetime import date
import copy

from gppylib    import gplog
from gppylib.db import dbconn

logger = gplog.get_default_logger()

# ============================================================================
class GpDB:
    """
    GpDB class representing configuration information for
    a single dbid within a Greenplum Array.
    """

    # --------------------------------------------------------------------
    def __init__(self, content, preferred_role, dbid, role, mode, status,
                 hostname, address, port, datadir, replicationPort):

        # Todo: replace all these fields with private alternatives:
        # e.g. '_content' instead of 'content'.
        #
        # Other code should go through class interfaces for access, this
        # will allow easier modifications in the future.
        self.content=content
        self.preferred_role=preferred_role
        self.dbid=dbid
        self.role=role
        self.mode=mode
        self.status=status
        self.hostname=hostname
        self.address=address
        self.port=port
        self.datadir=datadir
        self.replicationPort=replicationPort

        # Pending filespace creation
        self._pending_filespace = None

        # Todo: Remove old dead code
        self.valid = (status == 'u')
        
    # --------------------------------------------------------------------
    def __str__(self):
        """
        Construct a printable string representation of a GpDB
        """
        return "%s:%s:content=%s:dbid=%s" % (
            self.hostname,
            self.datadir,
            self.content,
            self.dbid,
            )
   
    # --------------------------------------------------------------------
    def __repr__(self):
        """
        Construct a string representation of class, must be sufficient
        information to call initFromString on the result.
        """
        return '%d|%d|%s|%s|%s|%s|%s|%s|%d|%s|%s' % (
            self.dbid,
            self.content,
            self.role,
            self.preferred_role,
            self.mode,
            self.status,
            self.hostname,
            self.address,
            self.port,
            self.datadir,
            self.replicationPort)

    # --------------------------------------------------------------------
    @staticmethod
    def initFromString(s):
        """
        Factory method, initializes a GpDB object from string representation.
          - Used when importing from file format.
          - TODO: Should be compatable with repr() formatting.
        """
        tup = s.strip().split('|')
        
        # Old format: 8 fields
        #    Todo: remove the need for this, or rework it to be cleaner
        if len(tup) == 8:
            # This describes the gp_configuration catalog (pre 3.4)
            content         = tup[0]
            definedprimary  = tup[1]
            dbid            = tup[2]
            isprimary       = tup[3]
            valid           = tup[4]
            address         = tup[5]
            port            = tup[6]
            datadir         = tup[7]

            # Calculate new fields from old ones
            preferred_role  = 'p' if definedprimary else 'm'
            role            = 'p' if isprimary else 'm'
            hostname        = address                 # fixme
            mode            = 's'                     # ???
            status          = 'u' if valid else 'd'
            replicationPort = None

        # New format: 11 fields
        elif len(tup) == 11:
            # This describes the gp_segment_configuration catalog (3.4+)
            dbid            = tup[0]
            content         = tup[1]
            role            = tup[2]
            preferred_role  = tup[3]
            mode            = tup[4]
            status          = tup[5]
            hostname        = tup[6]
            address         = tup[7]
            port            = tup[8]
            datadir         = tup[9]   # from the pg_filespace_entry table
            replicationPort = tup[10]

        else:
            raise Exception("GpDB unknown input format: %s" % s)

        return GpDB(content         = content, 
                    preferred_role  = preferred_role, 
                    dbid            = dbid, 
                    role            = role, 
                    mode            = mode, 
                    status          = status,
                    hostname        = hostname, 
                    address         = address,
                    port            = port, 
                    datadir         = datadir, 
                    replicationPort = replicationPort)

    # --------------------------------------------------------------------
    # Six simple helper functions to identify what role a segment plays:
    #  + QD
    #     + master
    #     + standby master
    #  + QE
    #     + primary
    #     + mirror
    # --------------------------------------------------------------------    
    def isQD(self):
        return self.content < 0

    def isMaster(self):
        return self.content < 0 and self.preferred_role == 'p'

    def isStandby(self):
        return self.content < 0 and self.preferred_role == 'm'

    def isQE(self):
        return self.content >= 0

    def isPrimary(self):
        return self.content >= 0 and self.preferred_role == 'p'

    def isMirror(self):
        return self.content >= 0 and self.preferred_role == 'm'
       

    # --------------------------------------------------------------------
    # Simple access methods
    #
    #   Using access methods rather than letting other code pull private
    #   variables directly allows more easily adapt the internals of the
    #   class to changes to the underlying catalog.
    # --------------------------------------------------------------------
    def get_dbid(self):
        return self.dbid

    def get_content(self):
        return self.content

    def get_address(self):
        return self.address

    def get_hostname(self):
        return self.hostname

    def get_role(self):
        return self.role

    def get_status(self):
        """
        Called by gpstart to report segment status after startup
        """
        if self.status == 'u':
            return "Valid"
        else:
            return "Invalid"
    
    def get_datadir(self):
        '''
        Return the primary datadirectory location for the segment.

        Note: the datadirectory is just one of the filespace locations
        associated with the segment, calling code should be carefull not 
        to assume that this is the only directory location for this segment.

        Todo: open question - should this always return a list?
        '''
        return self.datadir


    #############################
    # getters
    #
    def getSegmentDbId(self):
        return checkNotNone("dbId", self.dbid)

    def getSegmentContentId(self):
        return checkNotNone("contentId", self.content)

    def getSegmentRole(self):
        return checkNotNone("role", self.role)

    def getSegmentPreferredRole(self):
        return checkNotNone("preferredRole", self.preferred_role)

    def getSegmentMode(self):
        return checkNotNone("mode", self.mode)

    def getSegmentStatus(self):
        return checkNotNone("status", self.status)

    #
    # Return the postmaster __port
    #
    def getSegmentPort(self):
        return checkNotNone("port", self.port)

    def getSegmentReplicationPort(self):
        # is Nullable (so can return None)
        return self.replicationPort

    def getSegmentHostName(self):
        # is Nullable (so can return None)
        return self.hostname

    def getSegmentAddress(self):
        # is Nullable (so can return None)
        return self.address

    def getSegmentDataDirectory(self):
        return checkNotNone("dataDirectory", self.datadir)

    #############################
    # setSegmentters
    #
    def setSegmentDbId(self, dbId):
        checkNotNone("dbId", dbId)
        self.__dbId = dbId

    def setSegmentContentId(self, contentId):
        checkNotNone("contentId", contentId)
        if contentId < -1:
            raise Exception("Invalid content id %s" % contentId)
        self.__contentId = contentId

    def setSegmentRole(self, role):
        checkNotNone("role", role)

        if role != 'm' and role != 'p':
            raise Exception("Invalid role '%s'" % role)

        self.__role = role

    def setSegmentPreferredRole(self, preferredRole):
        checkNotNone("preferredRole", preferredRole)

        if preferredRole != 'm' and preferredRole != 'p':
            raise Exception("Invalid preferredRole '%s'" % preferredRole)

        self.__preferredRole = preferredRole

    def setSegmentMode(self, mode):
        checkNotNone("mode", mode)

        if mode != 's' and mode != 'c' and mode != 'r':
            raise Exception("Invalid mode '%s'" % mode)

        self.__mode = mode

    def setSegmentStatus(self, status):
        checkNotNone("status", status)

        if status != 'd' and status != 'u':
            raise Exception("Invalid status '%s'" % status)

        self.__status = status

    #
    # Return the postmaster port
    #
    def setSegmentPort(self, port):
        checkNotNone("port", port)
        self.__port = port

    def setSegmentReplicationPort(self, replicationPort):
        # None is allowed -- don't check
        self.__replicationPort = replicationPort

    def setSegmentHostName(self, hostName):
        # None is allowed -- don't check
        self.__hostName = hostName

    def setSegmentAddress(self, address):
        # None is allowed -- don't check
        self.__address = address

    def setSegmentDataDirectory(self, dataDirectory):
        checkNotNone("dataDirectory", dataDirectory)
        self.__dataDirectory = dataDirectory

    def copy(self):
        return copy.copy(self) # a shallow copy is sufficent right now



    # --------------------------------------------------------------------
    # Extension to support gpfilespace:
    #    + add_filespace(path)
    #    + get_pending_filespace()
    #
    # A filespace location will be added to every GpDB in the array and then 
    # checks will be run to make sure that everything is valid.
    # --------------------------------------------------------------------
    def add_filespace(self, path):
        """
        Add a path for a pending filespace creation, 
        
        Throws: 
           Exception - if a path has already been specified for this segment.
        """
        if not self._pending_filespace:
            self._pending_filespace = path
        else:
            raise Exception("Duplicate filespace path for dbid %d" % self.dbid)

    def get_pending_filespace(self):
        """
        Returns the pending filespace location for this segment
        """
        return self._pending_filespace


# ============================================================================
class Segment:
    """
    Used to represent all of the SegmentDBs with the same contentID.  Today this
    can be at most a primary SegDB and a single mirror SegDB.  In the future we
    will most likely support multiple mirror segDBs.

    Note: This class seems to complicate the implementation of gparray, without
    adding much value.  Perhaps it should be removed.
    """
    primaryDB=None
    mirrorDBs=None
    
    # --------------------------------------------------------------------
    def __init__(self):
        self.mirrorDBs=[]
        pass

    # --------------------------------------------------------------------
    def addPrimary(self,segDB):
        self.primaryDB=segDB
    
    def addMirror(self,segDB):
        self.mirrorDBs.append(segDB)

    # --------------------------------------------------------------------    
    def get_dbs(self):
        dbs=[]
        dbs.append(self.primaryDB)
        if len(self.mirrorDBs) > 0:
            dbs.extend(self.mirrorDBs)
        return dbs

    # --------------------------------------------------------------------
    def get_hosts(self):
        hosts=[]
        hosts.append(self.primaryDB.hostname)
        for m in self.mirrorDBs:
            hosts.append(m.hostname)
        return hosts



def createSegmentRows(hostlist,primary_list,primary_portbase,mirror_type,mirror_list,mirror_portbase,dir_prefix):
    rows=[]
    
    dbid = 0
    content = 0
    #primaries
    for host in hostlist:
        isprimary='t'
        port=primary_portbase        
        for pdir in primary_list:
            fulldir = "%s/%s%d" % (pdir,dir_prefix,content)
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
            raise Exception("Not enough hosts for spread mirroring.  You must have more hosts than primary segments per host")
        
        mirror_port = {}

        mirror_host_offset=1
        last_mirror_offset=1
        for host in hostlist:
            mirror_host_offset = last_mirror_offset + 1
            last_mirror_offset += 1
            for mdir in mirror_list:
                fulldir = "%s/%s%d" % (mdir,dir_prefix,content)
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
            
            for mdir in mirror_list:
                fulldir = "%s/%s%d" % (mdir,dir_prefix,content)
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
    elif value.lower() == 't':
        return True
    raise Exception('Invalid true/false value')


# TODO: Destroy this  (MPP-7686)
#   Now that "hostname" is a distinct field in gp_segment_configuration
#   attempting to derive hostnames from interaface names should be eliminated.
#   
def get_host_interface(full_hostname):
    (host_part, inf_num) = ['-'.join(full_hostname.split('-')[:-1]), 
                            full_hostname.split('-')[-1]]
    if host_part == '' or not inf_num.isdigit():
        return (full_hostname, None)
    else:
        return (host_part, inf_num)

    
# ============================================================================
class GpArray:
    """ 
    GpArray is a python class that describes a Greenplum array.

    A Greenplum array consists of:
      master         - The primary QD for the array
      standby master - The mirror QD for the array [optional]
      segment array  - an array of segments within the cluster

    Each segment is either a single GpDB object, or a primary/mirror pair.

    It can be initialized either from a database connection, in which case
    it discovers the configuration information by examining the catalog, or
    via a configuration file.
    """

    # --------------------------------------------------------------------
    def __init__(self, segments):
        """TODO: """

        self.master = None
        self.standbyMaster = None
        self.segments = []
        self.expansionSegments=[]
        self.numPrimarySegments = 0

        self.strategy = 'n'

        for segdb in segments:

            # Handle QD nodes
            if segdb.isMaster():
                if self.master != None:
                    logger.error("multiple master dbs defined")
                    raise Exception("GpArray - multiple master dbs defined")
                self.master = segdb

            elif segdb.isStandby():
                if self.standbyMaster != None:
                    logger.error("multiple standy master dbs defined")
                    raise Exception("GpArray - multiple standby master dbs defined")
                self.standbyMaster = segdb

            # Handle regular segments
            elif segdb.isQE():
                
                # Todo: Relax the requirement that the segdbs must be sorted 
                # in content order.
                if len(self.segments) <= segdb.content:
                    self.segments.insert(segdb.content, Segment())
                
                seg = self.segments[segdb.content]
                if segdb.isPrimary():
                    seg.addPrimary(segdb)
                    self.numPrimarySegments += 1
                else:
                    seg.addMirror(segdb)
                    
            else:
                # Not a master, standbymaster, primary, or mirror?
                # shouldn't even be possible.
                logger.error("FATAL - invalid dbs defined")
                raise Exception("Error: GpArray() - invalid dbs defined")

        # Make sure we have a master db
        if self.master is None:
            logger.error("FATAL - no master dbs defined!")
            raise Exception("Error: GpArray() - no master dbs defined")

    # --------------------------------------------------------------------
    @staticmethod
    def initFromCatalog(dbURL, utility=False):
        """
        Factory method, initializes a GpArray from provided database URL
        """
        conn = dbconn.connect(dbURL, utility)

        strategy_rows = dbconn.execSQL(conn, '''
           SELECT fault_strategy FROM gp_fault_strategy
        ''')

        config_rows = dbconn.execSQL(conn, '''
           SELECT dbid, content, role, preferred_role, mode, status, hostname, 
               address, port, fselocation as datadir, replication_port
           FROM pg_catalog.gp_segment_configuration
           JOIN pg_catalog.pg_filespace_entry on (dbid = fsedbid)
           JOIN pg_catalog.pg_filespace fs 
             on (fsefsoid = fs.oid and fsname='pg_system')
           ORDER BY content, preferred_role DESC
        ''')

        conn.close()

        segments = []
        for row in config_rows:
            # Extract fields from the row
            (dbid, content, role, preferred_role, mode, status, hostname, 
             address, port, datadir, replicationPort) = row
            
            # Build a segment with the information
            seg = GpDB(content, preferred_role, dbid, role, mode, status, 
                       hostname, address, port, datadir, replicationPort)

            # append it to the list of segments
            segments.append(seg)

        for strategy_row in strategy_rows:
            (strategy) = strategy_row
            break

        array = GpArray(segments)
        array.strategy = strategy

        return array

    # --------------------------------------------------------------------
    @staticmethod
    def initFromFile(filename):
        """
        Factory method: creates a GpArray from an input file 
        (called by gpexpand.)

        Note: Currently this is only used by the gpexpand rollback facility,
        there is currently NO expectation that this file format is saved
        on disk in any long term fashion.  

        Format changes of the file are acceptable until this assumption is 
        changed, but initFromFile and dumpToFile must be kept in parity.
        """
        segdbs=[]    
        fp = open(filename, 'r')
        for line in fp:
            segdbs.append(GpDB.initFromString(line))
        fp.close()

        return GpArray(segdbs)

    # --------------------------------------------------------------------
    def dumpToFile(self, filename):
        """
        Dumps a GpArray to a file (called by gpexpand)

        Note: See notes above for initFromFile()
        """
        fp = open(filename, 'w')
        for gpdb in self.getDbList():
            fp.write(repr(gpdb) + '\n')
        fp.close()

    # --------------------------------------------------------------------
    def getDbList(self, includeExpansionSegs=False):
        """Return a list of all GpDb objects that make up the array"""
        dbs=[]
        dbs.append(self.master)
        if self.standbyMaster:
            dbs.append(self.standbyMaster)
        if includeExpansionSegs:
            dbs.extend(self.getSegDbList(True))
        else:
            dbs.extend(self.getSegDbList())
        return dbs

    # --------------------------------------------------------------------    
    def getSegDbList(self, includeExpansionSegs=False):
        """Return a list of all GpDb objects for all segments in the array"""
        dbs=[]
        for seg in self.segments:
            dbs.extend(seg.get_dbs())
        if includeExpansionSegs:
           for seg in self.expansionSegments:
               dbs.extend(seg.get_dbs()) 
        return dbs

    # --------------------------------------------------------------------    
    def getExpansionSegDbList(self):
        """Returns a list of all GpDb objects that make up the new segments
        of an expansion"""
        dbs=[]
        for seg in self.expansionSegments:
            dbs.extend(seg.get_dbs())
        return dbs

    # --------------------------------------------------------------------    
    def getSegmentContainingDb(self, db):
        for seg in self.segments:
            for segDb in seg.get_dbs():
                if db.dbid == segDb.dbid:
                    return seg
        return None

    # --------------------------------------------------------------------
    def get_invalid_segdbs(self):
        dbs=[]
        for seg in self.segments:
            segdb = seg.primaryDB
            if not segdb.valid:
                dbs.append(segdb)
            for db in seg.mirrorDBs:
                if not db.valid:
                    dbs.append(db)
        return dbs

    # --------------------------------------------------------------------
    def get_inactive_mirrors_segdbs(self):
        dbs=[]

        if self.strategy[0] != 's':
            return dbs

        for seg in self.segments:
            segdb = seg.primaryDB
            for db in seg.mirrorDBs:
                dbs.append(db)
        return dbs

    # --------------------------------------------------------------------    
    def get_valid_segdbs(self):
        dbs=[]
        for seg in self.segments:
            db = seg.primaryDB
            if db.valid:
                dbs.append(db)
            for db in seg.mirrorDBs:
                if db.valid:
                    dbs.append(db)
        return dbs        

    # --------------------------------------------------------------------
    def get_hostlist(self, includeMaster=True):
        hosts=[]
        if includeMaster:
            hosts.append(self.master.hostname)
            if self.standbyMaster is not None:
                hosts.append(self.standbyMaster.hostname)        
        for seg in self.segments:
            hosts.extend(seg.get_hosts())
        return hosts

    # --------------------------------------------------------------------   
    def get_max_dbid(self,includeExpansionSegs=False):
        """Returns the maximum dbid in the array.  If includeExpansionSegs
        is True, this includes the expansion segment array in the search"""
        dbid = 0

        for db in self.getDbList(includeExpansionSegs):
            if db.dbid > dbid:
                dbid = db.dbid
        
        return dbid

    # --------------------------------------------------------------------    
    def get_max_contentid(self, includeExpansionSegs=False):
        """Returns the maximum contentid in the array.  If includeExpansionSegs
        is True, this includes the expansion segment array in the search"""
        content = 0
        
        for db in self.getDbList(includeExpansionSegs):
            if db.content > content:
                content = db.content
                
        return content

    # --------------------------------------------------------------------   
    def get_segment_count(self):
        return len(self.segments)

    # --------------------------------------------------------------------    
    def get_min_primary_port(self):
        """Returns the minimum primary segment db port"""
        min_primary_port = self.segments[0].primaryDB.port
        for seg in self.segments:
            if seg.primaryDB.port < min_primary_port:
                min_primary_port = seg.primaryDB.port
        
        return min_primary_port
    
    # --------------------------------------------------------------------    
    def get_max_primary_port(self):
        """Returns the maximum primary segment db port"""
        max_primary_port = self.segments[0].primaryDB.port
        for seg in self.segments:
            if seg.primaryDB.port > max_primary_port:
                max_primary_port = seg.primaryDB.port
        
        return max_primary_port
    
    # --------------------------------------------------------------------    
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
    
    # --------------------------------------------------------------------    
    def get_max_mirror_port(self):
        """Returns the maximum mirror segment db port"""
        if self.get_mirroring_enabled() is False:
            raise Exception('Mirroring is not enabled')
        
        max_mirror_port = self.segments[0].mirrorDBs[0].port
        
        for seg in self.segments:
            for db in seg.mirrorDBs:
                if db.port > max_mirror_port:
                    max_mirror_port = db.port

    # --------------------------------------------------------------------    
    def get_interface_numbers(self):
        """Returns interface numbers in the array.  Assumes that nodes are named
        <hostname>-<int_num>.  If the nodes just have <hostname> then an empty
        array is returned."""
        # Get the first segment hostname minus the interface number
        interface_nums = []
        primary_hostname = self.segments[0].primaryDB.hostname
        (host_part, inf_num) = get_host_interface(primary_hostname)
        if host_part == '' or inf_num is None or not inf_num.isdigit():
            return []
        
        search_host_part = host_part
        
        # get the interface numbers for host_part
        for db in self.getDbList():
            (host_part, inf_num) = get_host_interface(db.hostname)
            if host_part == search_host_part:
                if inf_num is None or not inf_num.isdigit():
                    # We have an unexpected formatted hostname
                    return []
                else:
                    if inf_num not in interface_nums:
                        interface_nums.append(inf_num)
        
        return interface_nums

    # --------------------------------------------------------------------    
    def get_primary_count(self):
        return self.numPrimarySegments
        
    # --------------------------------------------------------------------
    def get_mirroring_enabled(self):
        """Returns True is mirrors are defined"""
        return len(self.segments[0].mirrorDBs) != 0

    # --------------------------------------------------------------------        
    def get_primary_root_datadirs(self):
        """
        Returns a list of primary data directories minus the <prefix><contentid>

        NOTE 1: 
           This currently assumes that all segments are configured the same
           and gets the results only from the host of segment 0

        NOTE 2: 
           The determination of hostname is based on faulty logic
        """
        primary_datadirs = []

        seg0_hostname = self.segments[0].primaryDB.get_hostname()
        (seg0_hostname, inf_num) = get_host_interface(seg0_hostname)
        
        for db in self.getDbList():
            if db.isPrimary() and db.get_hostname().startswith(seg0_hostname):
                primary_datadirs.append(db.datadir[:db.datadir.rfind('/')])
        
        return primary_datadirs

    # --------------------------------------------------------------------    
    def get_mirror_root_datadirs(self):
        """Returns a list of mirror data directories minus the <prefix><contentid>"""
        mirror_datadirs = []

        seg0_hostname = self.segments[0].primaryDB.get_hostname()
        (seg0_hostname, inf_num) = get_host_interface(seg0_hostname)

        for db in self.getDbList():
            if db.isMirror() and db.get_hostname().startswith(seg0_hostname):
                mirror_datadirs.append(db.datadir[:db.datadir.rfind('/')])
        
        return mirror_datadirs

    # --------------------------------------------------------------------    
    def get_datadir_prefix(self):
        """Returns the prefix portion of <prefix><contentid>"""
        start_last_dir = self.master.datadir.rfind('/') + 1
        start_dir_content = self.master.datadir.rfind('-')
        prefix = self.master.datadir[start_last_dir:start_dir_content]
        return prefix

    # --------------------------------------------------------------------    
    def addExpansionSeg(self, content, preferred_role, dbid, role, 
                        hostname, port, datadir):
        """ 
        Adds a segment to the gparray as an expansion segment.

        Note: may work better to construct the new GpDB in gpexpand and
        simply pass it in.
        """

        if (content <= self.segments[-1].get_dbs()[0].content):
            raise Exception('Invalid content ID for expansion segment')
        
        segdb = GpDB(content = content, 
                     preferred_role = preferred_role, 
                     dbid = dbid, 
                     role = role,
                     mode = 's',
                     status = 'u',
                     hostname = hostname, 
                     address = hostname,      # todo: add to parameters
                     port = port, 
                     datadir = datadir, 
                     replicationPort = None)  # todo: add to parameters
        
        seglen = len(self.segments)
        expseglen = len(self.expansionSegments)
        
        expseg_index = content - seglen
        logger.debug('New segment index is %d' % expseg_index)
        if expseglen < expseg_index + 1:
            logger.debug('Extending expansion array by %d' % (expseglen - expseg_index + 1))
            self.expansionSegments.extend([None] * (expseglen - expseg_index + 1))
            self.expansionSegments[expseg_index] = Segment()

        seg = self.expansionSegments[expseg_index]
        if preferred_role == 'p':
            if seg.primaryDB:
                raise Exception('Duplicate content id for primary segment')                   
            seg.addPrimary(segdb)
        else:             
            seg.addMirror(segdb)

    # --------------------------------------------------------------------
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
                if segdb.role == 'p':
                   isprimary = True
                else:
                   isprimary = False
                content.append((segdb.content, isprimary))
            
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
        
        # Check for redefinition data dirs and ports
        datadirs = {}
        used_ports = {}
        for db in self.getDbList(True):
            (hostname,inf) = get_host_interface(db.get_hostname())
            datadir = db.get_datadir()
            if datadirs.has_key(hostname):
                if datadir in datadirs[hostname]:
                    raise Exception('Data directory %s used multiple time on host %s' % (datadir, db.hostname))
                else:
                    datadirs[hostname].append(datadir)
            else:
                datadirs[hostname] = []
                datadirs[hostname].append(datadir)
            if used_ports.has_key(hostname):
                if db.port in used_ports[hostname]:
                    raise Exception('Port %d is used multiple times on host %s' % (db.port, db.get_hostname()))
                else:
                    used_ports[hostname].append(db.port)
            else:
                used_ports[hostname] = []
                used_ports[hostname].append(db.port)
    

    # --------------------------------------------------------------------    
    def addExpansionHosts(self, hosts, mirror_type):
        """ Adds a list of hosts to the array, using the same data
        directories as the original hosts.  Also adds the mirrors
        based on mirror_type.
        """
        # remove interface numbers if they exist
        existing_hosts = []
        for host in self.get_hostlist(True):
            (host_part, inf_num) = get_host_interface(host)
            if host_part not in existing_hosts:        
                existing_hosts.append(host_part)
        
        new_hosts = []
        
        for host in hosts:
            # see if we already have the host
            (host_part, inf_num) = get_host_interface(host)
            if host_part in existing_hosts or host_part in new_hosts:
                continue
            else:
                new_hosts.append(host_part)

        if len(new_hosts) == 0:        
            raise Exception('No new hosts to add')
        
        primary_dirs = self.get_primary_root_datadirs()
        mirror_dirs = self.get_mirror_root_datadirs()
        interface_list = self.get_interface_numbers()
        base_primary_port = self.get_min_primary_port()
        base_mirror_port = 0
        if mirror_type != 'none':
            base_mirror_port = self.get_min_mirror_port()
        
        prefix = self.get_datadir_prefix()
        
        rows = createSegmentRows(new_hosts, primary_dirs, base_primary_port, 
                                 mirror_type, mirror_dirs, base_mirror_port, 
                                 prefix)
        
        self._fixup_and_add_expansion_segments(rows, interface_list)
        
    # --------------------------------------------------------------------    
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
        

    # --------------------------------------------------------------------    
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
                self.addExpansionSeg(curr_content, 'p', curr_dbid, 
                                        True, hostname, int(row[6]), new_datadir)
                # The content id was adjusted, so we need to save it for the mirror
                mirror_dict[row[0]] = curr_content
                curr_content += 1
            else:
                new_content = mirror_dict[row[0]]
                new_datadir += ('%d' % new_content)
                self.addExpansionSeg(new_content, 'm', curr_dbid, 
                                     False, hostname, int(row[6]), new_datadir)
            curr_dbid += 1           


# === EOF ====
