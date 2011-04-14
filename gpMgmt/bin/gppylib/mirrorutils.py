#!/usr/bin/env python
# Line too long - pylint: disable=C0301
# Invalid name  - pylint: disable=C0103

"""
  mirrorutils.py
  Routines to help generate mirror distributions
  Copyright (c) EMC/Greenplum Inc 2011. All Rights Reserved. 
"""

from math            import ceil
from heapq           import heapify, heappop, heappush
from gppylib.gparray import ROLE_PRIMARY, ROLE_MIRROR
from gppylib         import gplog
from gppylib.gplog   import logging_is_verbose

logger = gplog.get_default_logger()

                        #####################
                        #  basic utilities  #
                        #####################

import re
standard_host_re = re.compile(r'([a-zA-Z]+)((\d+)(-(\d+))?)')

def standard_host_cmp(h1, h2):
    """
    Standard comparison function for GPDB hosts derived from 
    gpinitsystem.  Relies on the standard host regular expression.

    @param h1: first host
    @param h2: second host
    @returns:  -1, 0, or 1 depending on their relationship
    """
    # fallback to ordinary compare when regex not matched
    m1 = standard_host_re.match(h1)
    if not m1:
        return cmp(h1,h2)
    m2 = standard_host_re.match(h2)
    if not m2:
        return cmp(h1,h2)

    # regexes match: compare by prefix, number and interface
    if m1.group(1) <= m2.group(1):
        if not int(m1.group(3)) == int(m2.group(3)):
            return int(m1.group(3)) - int(m2.group(3))
        elif m1.group(5):
            return int(m1.group(5)) - int(m2.group(5))
        else:
            return 0

    return 1


#
# hid (host, interface, dbid) list and supporting routines:
# 
#  get_hid(db)           -> returns (host, interface, dbid) for the db
#  hid_cmp(i1,i2)        -> compares two (host, interface, dbid) tuples
#  HIDList(gparray)      -> list of (host, interface, dbid) for primaries and mirrors in gparray
#  HostList(hidlist)     -> list of hosts built from the (host, interface, dbid) list
#  InterfaceSet(hidlist) -> set of interfaces built from a (host, interface, dbid) list
#
# the name 'hid' was selected because when working with a 'hid'
# it's easy to remember the following identities:
#
#  hid[0] = host       
#  hid[1] = interface  
#  hid[2] = dbid       
#

def get_hid(db):
    """
    Identify the host and interface for the specified 
    db segment by parsing the 'address' (which is actually a name) 
    using the standard host regular expression.  

    Returns a triple (host, interface, dbid).  host and interface
    are determined by looking for the '-' in the address field
    indicating the start of the interface. For example:

    match = standard_host_re.match('ab1-2')

    >>> match.group(0)  # the entire string
    'ab1-2'
    >>> match.group(1)  # initial alpha portion
    'ab'
    >>> match.group(2)  # everything after alpha portion
    '1-2'
    >>> match.group(3)  # numeric suffix after alpha part
    '1'
    >>> match.group(4)  # interface part including the -
    '-2'
    >>> match.group(5)  # interface after the -
    '2'

    So we identify the host using groups 1 and 3: 'ab1' and the interface 
    using group 5: '2' and return the triple ('ab1', '2', <dbid>)

    If a nonstandard naming scheme was used then we simply
    use the entire address as the 'host' and the empty string
    as the interface.

    @param db: the database segment to identify
    @returns:  a triple identifying the segment's host
    """
    address = db.getSegmentAddress()
    dbid    = db.getSegmentDbId()
    match   = standard_host_re.match(address)
    if match and match.group(1) and match.group(3) and match.group(5):
        host      = match.group(1) + match.group(3)
        interface = match.group(5)
    else:
        host      = address
        interface = ''
    return (host, interface, dbid)


def hid_cmp(i1, i2):
    """
    Compare a triple of (host, interface, dbid) values using the
    standard_host_cmp for the hosts.

    @param h1: first (host, interface)
    @param h2: second (host, interface)
    @returns:  -1, 0, or 1 depending on their relationship
    """
    r = standard_host_cmp(i1[0], i2[0])
    if r != 0:
        return r
    r = cmp(i1[1], i2[1])
    if r != 0:
        return r
    r = cmp(i1[2], i2[2])
    return r


class HIDList(list):
    """
    Python list of (host, interface, dbid) values built from a gparray.  
    The list values depend on the definition of standard_host_identification 
    and standard_host_cmp
    """
    def __init__(self, array):
        """
        Initialize the list of addresses from the ordinary (non-expansion)
        segments of the specified GpArray.

        @param array: The GpArray
        """
        hids = {}
        for db in array.getDbList(False):
            if db.isSegmentMaster() or db.isSegmentStandby():
                continue
            hid = get_hid(db)
            if hid in hids:
                continue
            hids[hid] = True
        list.__init__(self, sorted(hids.keys(), cmp=hid_cmp))
        if logging_is_verbose():
            logger.debug("HIDList")
            for h in self:
                logger.debug("  %s" % str(h))


class HostList(list):
    """
    Python list of "host" values built from hid list.
    """
    def __init__(self, hidlist):
        """
        Initialize the list of hosts from the hidlist.
        @param hidlist: The HIDList
        """
        list.__init__(self, sorted(dict([(hid[0],True) for hid in hidlist]).keys()))

        if logging_is_verbose():
            logger.debug("HostList")
            for h in self:
                logger.debug("  %s" % str(h))



class InterfaceSet(set):
    """
    Python set of "interface" values and a queue for tracking counts
    of databases assigned to each interface.  

    Empty '' interfaces which appear in the hid list when non-standard 
    names are used are omitted from the set.
    """
    def __init__(self, hidlist):
        """
        Initialize the set of interfaces from the hidlist.
        @param hidlist: The HIDList
        """
        set.__init__(self, dict([(hid[1],True) for hid in hidlist if len(hid[1]) > 0]).keys())
        self.name   = ''
        self.counts = []
        for interface in self:
            self.counts.append( (0, interface) )
        heapify(self.counts)

    def choose_not(self, interface, primary=''):
        """
        Return an interface other than the specified one which also 
        has the fewest databases assigned to it.  As a side effect
        the count associated with the returned interface is incremented.
        This may return None if there is no suitable interface

        @param interface: the interface we don't want
        @returns:         an interface other than the specified one or None
        """
        putback = []
        result  = None
        while result is None:
            try:
                (counts, candidate) = heappop(self.counts)
            except IndexError:
                break
            if candidate != interface:
                counts += 1
                result = candidate
            putback.append((counts, candidate))
        while len(putback) > 0:
            heappush(self.counts, putback.pop())

        logger.debug("%s InterfaceSet %s choose_not %s %s returns %s" % (primary, self.name, interface, self.counts, result))
        return result


                        ####################
                        # HostDatabasesMap #
                        ####################


# HostDatabasesMap is a map from host to one of several kinds of things
# depending on the 'make' function passed to its constructor:
#
# make_dblist:     map of host -> [ dbs ]
# make_ifset:      map of host -> set( interfaces )
# make_cidset:     map of host -> set( contentids )


def make_dblist(d, dbs, hosts):
    """
    Build sorted db lists from a temporary dictionary
    filling in empty lists for hosts with no elements

    @param d:     the result dictionary
    @param dbs:   the temporary dictionary
    @param hosts: map of all hosts
    """
    for key, dbl in dbs.items():
        d[key] = sorted(dbl, key=lambda db:db.getSegmentDbId())
    for key in hosts:
        if not key in d:
            d[key] = []


def make_ifset(d, dbs, hosts):
    """
    Build an interface set from a temporary dictionary
    filling in empty sets for hosts with no elements,

    @param d:     the result dictionary
    @param dbs:   the temporary dictionary
    @param hosts: map of all hosts
    """
    for key, dbl in dbs.items():
        d[key] = InterfaceSet([get_hid(db) for db in dbl])
        d[key].name = key
    for key in hosts:
        if not key in d:
            d[key] = InterfaceSet()
            d[key].name = key


def make_cidset(d, dbs, hosts):
    """
    Build a content-id set from a temporary dictionary
    filling in empty sets for hosts with no elements

    @param d:     the result dictionary
    @param dbs:   the temporary dictionary
    @param hosts: map of all hosts
    """
    for key, dbl in dbs.items():
        d[key] = set([db.getSegmentContentId() for db in dbl])
    for key in hosts:
        if not key in d:
            d[key] = set()



class HostDatabasesMap(dict):
    """
    Python map from host to one of several different kinds of things
    depending on the argument to the 'make' parameter.
    """

    def __init__(self, array, hosts, role=None, make=make_dblist, info=''):
        """
        @param array: the GpArray
        @param hosts: list of hosts (to ensure present in result)
        @param role:  role of interest
        @param make:  function to construct final map from temporary
        @param info:  debugging info
        """

        # build temporary table of desired databases for each host
        #
        dict.__init__(self)
        dbs = {}
        for db in array.getDbList(False):

            # filter dbs by the requested role
            if role is not None:
                if role == ROLE_PRIMARY and not db.isSegmentPrimary():
                    continue
                elif role == ROLE_MIRROR and not db.isSegmentMirror():
                    continue

            hid = get_hid(db)
            dbl = dbs.get(hid[0])
            if dbl is None:
                dbs[hid[0]] = dbl = []
            dbl.append(db)

        # build final map from temporary table using the caller's make function
        #
        make(self, dbs, hosts)

        # diagnostic output
        #
        if logging_is_verbose():
            logger.debug("HostDatabaseMap (%s) %s %s" % (info, role, make.__name__))
            for k, v in self.items():
                if make == make_dblist:
                    logger.debug("  %s:" % k)
                    for v2 in v: 
                        logger.debug("    %s" % v2)
                else: 
                    logger.debug("  %s: %s" % (k,v))



class CidMirrorMap(dict):
    """
    Python map from content id to mirror database.
    """

    def __init__(self, array, info=''):
        """
        @param array: the GpArray
        @param info:  debugging info
        """

        dict.__init__(self)
        for db in array.getDbList(False):
            if not db.isSegmentMirror():
                continue

            cid = db.getSegmentContentId()
            assert self.get(cid) is None
            self[ cid ] = db

        # diagnostic output
        #
        if logging_is_verbose():
            logger.debug("CidMirrorMap (%s)" % info)
            for k, v in self.items():
                logger.debug("  %s: %s" % (k,v))



                    ############################
                    # generator helper classes #
                    ############################

class MirroringMaps:
    """
    This class constructs a number of maps and list from the gparray
    for use by the mirroring generators.
    """

    def __init__(self, array):
        self.array = array

        # (host, interface, dbid) list for all hosts in array
        self.hidlist          = HIDList(array)

        # (host) list for all hosts
        self.hosts            = HostList(self.hidlist)

        # host->[ db ] map for each primary host
        self.dbmap            = HostDatabasesMap(array, self.hosts, ROLE_PRIMARY, info='dbmap')

        # unique set of interfaces observed in cluster
        self.interfaces       = InterfaceSet(self.hidlist)

        # host-> set of interfaces on that host
        self.ifmap            = None
        if len(self.interfaces) > 1:
            self.ifmap        = HostDatabasesMap(array, self.hosts, None, make=make_ifset, info='ifmap')

        # host->{ content ids } map for primary hosts
        self.primary_cids     = HostDatabasesMap(array, self.hosts, ROLE_PRIMARY, make=make_cidset, info='primary_cids')

        # host->{ content ids } map for mirror hosts
        self.mirror_cids      = HostDatabasesMap(array, self.hosts, ROLE_MIRROR, make=make_cidset, info='mirror_cids')

        # content id->{ mirror db } map for hosts wth mirrors
        self.cidmirrors       = CidMirrorMap(array, info='cidmirrors')

        # chassis maps (not used yet)
        self.chmap            = None

        # failure group maps (not used yet)
        self.fgmap            = None
        


class ForbiddenHosts(dict):
    """
    Python map of hosts which should not be used for a given primary's mirror.
    """
    def __init__(self, primaryhost, maps):
        """
        @param primaryhost: the name of the host containing the primary
        @param maps:        the mirroring maps
        """
        dict.__init__(self)

        # 1. a mirror may not be placed on the same host as the primary
        dict.__setitem__(self, primaryhost, True)

        # 2. a mirror may not be placed on any host in the same chassis as the primary
        if maps.chmap is not None:
            primarychassis = maps.chmap[primaryhost]
            for h in maps.hosts:
                if maps.chmap[h] == primarychassis:
                    dict.__setitem__(self, h, True)

        # 3. a mirror must be placed on a host in the same failure group as the primary
        if maps.fgmap is not None:
            primarygroup = maps.fgmap[primaryhost]
            for h in maps.hosts:
                if maps.fgmap[h] != primarygroup:
                    dict.__setitem__(self, h, True)


def assign_ports(maps, issues, final):
    """
    Assign ports to mirrors.  Updates elements of 'final' list
    as a side effect.

    @param maps:   mirroring maps
    @param issues: issues
    @param final:  the list of RedistributeMirrors to assign
    """

    # build a table of free ports for each address
    #
    freeports = {}
    maxports  = None
    extra     = 0
    for rm in final:
        cid = rm.db.getSegmentContentId()
        db  = maps.cidmirrors.get( cid )
        if db:

            # This db is a mirror no-longer assigned to its primary.
            # Put its ports in the free ports map.
            #
            maddr = db.getSegmentAddress()
            ports = freeports.get( maddr )
            if ports is None:
                freeports[ maddr ] = ports = []

            port  = db.getSegmentPort()
            rport = db.getSegmentReplicationPort()
            ports.append( (port, rport) )

            # We also keep a record of the highest ports in use
            # so that we can safely assign ports to new mirrors.
            #
            if maxports is None:
                maxports = (port, rport)
            else:
                maxports = (max(port,maxports[0]), max(rport,maxports[1]))

    # allocate free ports to mirrors
    #
    for rm in final:
        maddr = rm.mirroraddress
        ports = freeports.get( maddr )
        if ports:
            (rm.mirrorport, rm.mirrorreplicationport) = ports.pop(0)
        elif maxports is not None:

            # We've run out of previously assigned ports on this address,
            # start allocating new ports after the highest port in use.
            #
            extra += 1
            (rm.mirrorport, rm.mirrorreplicationport) = maxports
            rm.mirrorport            += extra
            rm.mirrorreplicationport += extra
        else:

            # maxports can only be none if there never were any mirrors.
            #
            msg = 'No existing mirrors to move'
            issues.append(msg)
            break

        logger.debug("+ %s" % rm)


                        ###################
                        # group mirroring #
                        ###################


# For group mirroring, we have the structure:
#
#   GroupMirroringGenerator
#      |
#      +---> CheapestMirrorQUeue
#              | a priority queue tracking the primaries
#              | and their best mirror candidates
#              |
#              +-->> MirrorCostQueue
#                      | a priority queue maintaining the best 
#                      | mirror candidates for each primary
#                      |
#                      +-->> CostHost
#                             a triple (#interfaces, #mirrors, host)
#                             reflecting the value to a particular 
#                             primary of selecting this host to hold
#                             all its mirrors.
#
# The basic algorithm of the generator is to construct
# the structure shown above and then drain the queues.


class CostHost:
    # Too few public methods - pylint: disable=R0903
    """
    Simple (#interfaces,#mirrors,host) triple.
    """

    def __init__(self, icnt, mcnt, host):
        (self.icnt, self.mcnt, self.host) = icnt, mcnt, host

    def __cmp__(self, other):
        result = cmp(self.icnt, other.icnt)
        if result != 0:
            return result
        result = cmp(self.mcnt, other.mcnt)
        if result != 0:
            return result
        return standard_host_cmp(self.host, other.host)

    def __repr__(self):
        return '(%s,%s,%s)' % (self.icnt,self.mcnt,self.host)



class MirrorCostQueue:
    """
    A priority queue of (icnt, mcnt, host) elements where 
    icnt represents the number of interfaces on the mirror host and
    mcnt represents the number of mirrors which would need to 
    be moved to the host if that host were selected to hold all
    mirrors of primaries on another host.

    When all hosts have the same number of interfaces this
    heuristic will attempt to choose group mirrors in a way
    that will move less data.  

    When there is an uneven distribution of interfaces, it's
    likely that some mirror candidates will be unable to satisify
    the majority of hosts, so we purposely weight those mirrors
    with lower costs to help ensure that the most qualified primaries
    attempt to match those before considering less needy hosts.
    """

    def __init__(self, h1, maps, disallow):
        """
        @param h1:       the primary host whose mirrors we need to place
        @param maps:     mirroring maps
        @param disallow: map of forbidden hosts for mirrors
        """

        self.maps         = maps
        self.primary      = h1
        self.costs        = []
        self.disqualified = {}

        # add all the potential mirror candidates for h1 into our queue
        #
        logger.debug("mirror cost queue for %s" % h1)
        for h2 in maps.primary_cids.keys():

            if h2 == h1:
                logger.debug("  rejected self: %s" % h2)
                continue

            if disallow is not None and h2 in disallow:
                logger.debug("  not allowed by filter: %s" % h2)
                continue

            if maps.ifmap is not None:
                primary_interfaces = maps.ifmap[h1]
                mirror_interfaces  = maps.ifmap[h2]
                if len(mirror_interfaces) < 2 and len(primary_interfaces) > 1:
                    if len(mirror_interfaces & primary_interfaces) > 0:
                        self.disqualified[h2] = True
                        continue

            # h2 is an eligible mirror, now compute the cost relative to the primary
            # icnt is number of interfaces on mirror candidate
            # mcnt is #primaries - #mirrors already on candidate

            icnt               = len(maps.ifmap[h2]) if maps.ifmap is not None else 1
            primary_contentids = maps.primary_cids[h1]
            mirror_contentids  = maps.mirror_cids[h2]
            needed             = len(primary_contentids)
            present            = len(primary_contentids & mirror_contentids)
            mcnt               = needed - present

            entry              = CostHost(icnt, mcnt, h2)
            logger.debug("  adding: %s" % entry)

            self.costs.append(entry)

        heapify(self.costs)
        logger.debug("%s" % self)


    def __repr__(self):
        """
        @returns: string representation for diagnostic purposes.
        """
        return 'MirrorCostQueue %s: %s, %s' % (self.primary, self.costs, self.disqualified)


    def __cmp__(self, other):
        """
        Queue comparison function.  First compares by least element in
        the queue and then by hostname using standard comparison.
        """
        result = self.least().__cmp__(other.least())
        if result != 0:
            return result
        return standard_host_cmp(self.primary, other.primary)


    def least(self):
        """
        @returns: cheapest CostHost or None if the queue is exhausted.
        """
        try:
            return self.costs[0]
        except IndexError:
            return None


    def pop(self):
        """
        @returns: Pops and returns cheapest host or None if the queue is exhausted.
        """
        try:
            return heappop(self.costs).host
        except IndexError:
            return None


    def nexthost(self, inuse=None):
        """
        Return the least expensive mirror candidate for this primary, 
        discarding any candidates already in the 'inuse' map which have
        already been assigned to other primaries.

        @param inuse: hosts already selected for other primaries' mirrors
        @returns:     the least expensive mirror candidate not presently inuse
        """
        host = self.pop()

        while inuse is not None and host is not None and host in inuse:
            logger.debug("  %s nexthost, %s in use by %s" % (self.primary, host, inuse[host]))
            host = self.pop()
        
        logger.debug("  %s nexthost returns %s" % (self.primary, host))
        return host



class CheapestMirrorQueue:
    # Too few public methods - pylint: disable=R0903
    """
    A priority queue of (cost, queue) elements where the cost
    represents the least known cost to place any primary's mirrors.

    Suppose we have the following primary/mirror configuration:
    
                  primaries              mirrors
                                                         
            host1:  p1  p2  p3  p4   m7 m16 m11 m15
            host2:  p5  p6  p7  p8   m3 m10 m12 m14
            host3:  p9 p10 p11 p12   m1  m2  m4 m13
            host4: p13 p14 p15 p16   m5  m8  m9  m6

    The cost matrix in this situation is:
        
                               mirror host
                          
                        host1  host2  host3  host4
                        -----  -----  -----  -----
                host1 | None     3      1      4
     primary    host2 |   3    None     4      1
      host      host3 |   3      2    None     3
                host4 |   2      3      3     None


    For example, ['host1']['host3'] = 1 because only 1 mirror (m3)
    needs to be put on host3 in order for it to hold mirrors for all
    of host1's primaries.  Similarly ['host2']['host3'] = 4 because
    host3 currently holds none of host2's mirrors.  The entries along
    the diagonal are all None because it's not acceptable for any
    mirrors to reside on the same host as their primaries.

    For such a configuration, our priority queue of queues could be
    
        (1, queue for host1)
        (1, queue for host2)
        (2, queue for host4)
        (2, queue for host3)

    reflecting that in the best choice for setting up group mirroring
    we need only move one mirror for host1 and host2 and two mirrors
    for host3 and host 4.  Note that the queue is dynamic since the 
    cheapest costs may change as mirrors are assigned.
    """

    def __init__(self, maps, issues):
        """
        @param maps:   mirroring maps
        @param issues: issues
        """

        self.maps       = maps
        self.issues     = issues
        self.inuse      = {}
        self.heap       = []
        self.ineligible = []
        for h1 in maps.hosts:
            disallow = ForbiddenHosts(h1, maps)
            queue    = MirrorCostQueue(h1, maps, disallow)

            for dis in queue.disqualified.keys():
                msg = '%s disqualified as group mirror for %s' % (dis, h1)
                self.issues.append(msg)

            if queue.least() is None:
                msg = 'Not eligible for group mirroring: %s' % h1
                self.issues.append(msg)
            else:
                self.heap.append(queue)

        heapify(self.heap)


    def pop(self):
        """
        Return the next pair of (primary host, mirror host) for group 
        mirroring or None if we've exhausted our queues.
        """
        assert len(self.issues) < 1

        primary  = None
        mirror   = None

        while mirror is None:
            try:
                queue = heappop(self.heap)
            except IndexError:
                logger.debug("CheapestMirrorQueue pop: None, None")
                return (None, None)

            primary = queue.primary
            mirror  = queue.nexthost(self.inuse)

            if mirror is not None:
                self.inuse[mirror] = primary
            else:
                msg = 'No mirror available for %s' % primary
                self.issues.append(msg)
            
        logger.debug("CheapestMirrorQueue pop: %s, %s" % (primary, mirror))
        return (primary, mirror)




class GroupMirroringGenerator:
    # Too few public methods - pylint: disable=R0903
    """
    Sample class for generating group mirroring distributions
    taking into account current mirror placements and/or chassis
    and failure group host mappings.
    """

    def __init__(self, maps):
        """
        @param maps:  mirroring maps
        """
        self.maps   = maps
        self.final  = None
        self.issues = []
        self.best   = None

        if len(self.maps.cidmirrors) < 1:
            msg = 'No existing mirrors to move'
            self.issues.append(msg)
        else:
            self.best = CheapestMirrorQueue(self.maps, self.issues)


    def build(self):
        "Attempt to build a group mirroring distribution."

        if len(self.issues) > 0:
            return None

        final = []
        while True:

            # get the next primary/mirror from our cheapest mirror queue
            #
            primaryhost, mirrorhost = self.best.pop()
            if primaryhost is None:
                break

            # for each primary database segment
            #
            for db in self.maps.dbmap[primaryhost]:

                # choose specific mirror for this primary segment
                #
                rm                       = RedistributeMirror()
                rm.db                    = db
                rm.primaryaddress        = db.getSegmentAddress()
                rm.mirroraddress         = None
                rm.mirrorhost            = mirrorhost
                rm.mirrorinterface       = None
                rm.mirrorport            = None
                rm.mirrorreplicationport = None

                if self.maps.ifmap:
                    rm.primaryinterface = get_hid(db)[1]
                    rm.mirrorinterface  = self.maps.ifmap[rm.mirrorhost].choose_not(rm.primaryinterface, primaryhost)
                    rm.mirroraddress = '%s-%s' % (rm.mirrorhost, rm.mirrorinterface)
                else:
                    rm.mirroraddress = rm.mirrorhost

                final.append(rm)

        # assign new mirror ports
        #
        if len(self.issues) < 1:
            assign_ports(self.maps, self.issues, final)

        if len(self.issues) > 0:
            return None

        self.final = final
        return self.final


                        ####################
                        # spread mirroring #
                        ####################


class CountHost:
    def __init__(self, count, host):
        (self.count, self.host) = count, host

    def __cmp__(self, other):
        result = cmp(self.count, other.count)
        if result != 0:
            return result
        return 0-standard_host_cmp(self.host, other.host)

    def __repr__(self):
        return '(%s,%s)' % (self.count,self.host)



class SpreadMirroringGenerator:
    # Too many instance attributes - pylint: disable=R0902
    """
    Generate a suggested spread mirroring configuration using 
    a simple balancing heuristic.
    """

    def __init__(self, maps):
        "@param maps:  mirroring maps"

        self.maps          = maps
        self.filter        = None    
        self.host_dbcounts = [CountHost(0,host) for host in maps.hosts]
        heapify(self.host_dbcounts)
        self.final         = None
        self.issues        = []

        if len(self.maps.cidmirrors) < 1:
            msg = 'No existing mirrors to move'
            self.issues.append(msg)


    def build(self):
        "Attempt to build a spread mirroring distribution."

        if len(self.issues) > 0:
            return None
        
        # for each primary host
        #
        final = []
        for primaryhost in self.maps.hosts:
            
            # update filter used by nextmirror() to disqualify mirror candidates
            #
            self.filter = ForbiddenHosts(primaryhost, self.maps)
            logger.debug("%s forbids %s" % (primaryhost, self.filter))

            # for each primary database segment
            #
            for db in self.maps.dbmap[primaryhost]:

                # choose specific mirror for this primary segment
                #
                rm                       = RedistributeMirror()
                rm.db                    = db
                rm.primaryaddress        = db.getSegmentAddress()
                rm.mirroraddress         = None
                rm.mirrorhost            = None
                rm.mirrorinterface       = None
                rm.mirrorport            = None
                rm.mirrorreplicationport = None
        
                while rm.mirroraddress is None:
        
                    rm.mirrorhost = self.nextmirror()

                    if rm.mirrorhost is None:
                        msg = 'Not eligible for spread mirroring: %s' % rm.primaryaddress
                        self.issues.append(msg)
                        return None
        
                    if self.maps.ifmap:
                        rm.primaryinterface = get_hid(db)[1]
                        rm.mirrorinterface  = self.maps.ifmap[rm.mirrorhost].choose_not(rm.primaryinterface, primaryhost)
                        if rm.mirrorinterface is None:
                            msg = 'Mirror candidate %s interfaces insufficient for primary %s' % (mirrorhost, rm.primaryaddress)
                            self.issues.append(msg)
                            continue

                        rm.mirroraddress = '%s-%s' % (rm.mirrorhost, rm.mirrorinterface)
                    else:
                        rm.mirroraddress = rm.mirrorhost
        
                final.append(rm)

        # assign new mirror ports
        #
        if len(self.issues) < 1:
            assign_ports(self.maps, self.issues, final)

        if len(self.issues) > 0:
            return None

        self.final = final
        return self.final


    def nextmirror(self):
        """
        Return the host selected to place the next mirror using
        a priority queue tracking the number of databases already
        assigned to each host in an effort to keep the distribution
        balanced across the hosts.
        """
        logger.debug("  nextmirror %s" % self.host_dbcounts)
        putback = []
        result  = None
        while result is None:
            try:
                candidate = heappop(self.host_dbcounts)
            except IndexError:
                break
            if candidate.host not in self.filter:
                logger.debug("  nextmirror selects %s" % candidate)
                candidate.count += 1
                result = candidate.host
            else:
                logger.debug("  nextmirror rejects %s" % candidate)
            putback.append(candidate)
        while len(putback) > 0:
            heappush(self.host_dbcounts, putback.pop())

        return result


        
                        ###########################
                        # mirror layout interface #
                        ###########################


class RedistributeMirror:
    """
    Instances of this class hold the information needed to 
    precisely specify the new location of a mirror for a given primary.  
    These objects are presented to the users of the MirroringDistributions
    class via the group_result and spread_result lists.
    """

    def __init__(self):
        self.db                    = None
        self.primaryaddress        = None
        self.mirroraddress         = None
        self.mirrorhost            = None
        self.mirrorinterface       = None
        self.mirrorport            = None
        self.mirrorreplicationport = None

    def __cmp__(self, other):
        return cmp(self.primaryaddress, other.primaryaddress)

    def __repr__(self): 
        return 'RedistributeMirror(' + ','.join([str(a) for a in self.attrs()]) + ')'

    def primary(self):
        return '%s:%s' % (self.primaryaddress, self.db.getSegmentPort())

    def mirror(self):
        return '%s:%s:%s' % (self.mirroraddress, self.mirrorport, self.mirrorreplicationport)

    def short(self):
        return '%s:%s' % (self.mirroraddress, self.mirrorport)

    def attrs(self):
        yield self.db.dbid
        yield self.primaryaddress
        yield self.mirroraddress
        yield self.mirrorhost
        yield self.mirrorinterface
        yield self.mirrorport
        yield self.mirrorreplicationport


class MirroringDistributions:
    """
    Interface to the mirroring layout algorithms.  

    Callers are expected to instanciate this class and then directly 
    inspect the members to access the results.  In particular,

    * group_result  and group_issues
    * spread_result and spread_issues
    """

    def __init__(self, array):
        """
        Populate results and issues based on the contents of the array.

        @param array: the GpArray
        """

        # maps
        maps                   = MirroringMaps(array)

        # group mirroring
        logger.debug("== computing group mirrors ==")
        group_generator        = GroupMirroringGenerator(maps)
        self.group_result      = group_generator.build()
        self.group_issues      = group_generator.issues

        # spread mirroring
        logger.debug("== computing spread mirrors ==")
        spread_generator       = SpreadMirroringGenerator(maps)
        self.spread_result     = spread_generator.build()
        self.spread_issues     = spread_generator.issues


