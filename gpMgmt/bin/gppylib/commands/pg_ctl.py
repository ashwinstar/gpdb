#!/usr/bin/env python
# Line too long - pylint: disable=C0301
# Invalid name  - pylint: disable=C0103

"""
  pg_ctl.py
  Copyright (c) EMC/Greenplum Inc 2011. All Rights Reserved. 
"""

class PgCtlBackendOptions(list):
    """
    Conceptually this is a list of options built in a structured
    manner with a canonical string representation suitable for use
    with the -o option of pg_ctl.

    Used by MasterStart, SegmentStart to format the backend options 
    string passed via pg_ctl -o

    Examples
    --------

    >>> str(PgCtlBackendOptions(5432, 1, 2))
    '-p 5432 -b 1 -z 2 --silent-mode=true'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_master(2, False, False))
    '-p 5432 -b 1 -z 2 --silent-mode=true -i -M master -C -1 -x 2'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_master(2, False, True))
    '-p 5432 -b 1 -z 2 --silent-mode=true -i -M master -C -1 -x 2 -E'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_segment('mirror', 1))
    '-p 5432 -b 1 -z 2 --silent-mode=true -i -M mirror -C 1'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_special('upgrade'))
    '-p 5432 -b 1 -z 2 --silent-mode=true -U'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_special('maintenance'))
    '-p 5432 -b 1 -z 2 --silent-mode=true -m'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_utility(True))
    '-p 5432 -b 1 -z 2 --silent-mode=true -c gp_role=utility'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_utility(False))
    '-p 5432 -b 1 -z 2 --silent-mode=true'
    >>> str(PgCtlBackendOptions(5432, 1, 2).set_restricted(True,1))
    '-p 5432 -b 1 -z 2 --silent-mode=true -c superuser_reserved_connections=1'
    >>> 

    """

    def __init__(self, port, dbid, numcids):
        """
        @param port: backend port
        @param dbid: backed dbid 
        @param numcids: total number of content ids in cluster
        """
        list.__init__(self, [
            "-p", str(port),
            "-b", str(dbid),
            "-z", str(numcids),
            "--silent-mode=true"
        ])

    def __str__(self):
        return " ".join(self)

    #
    # master/segment-specific options
    #

    def set_master(self, standby_dbid, disable, seqserver):
        """
        @param standby_dbid: standby dbid
        @param disable: start without master mirroring?
        @param seqserver: start with seqserver?
        """
        self.extend(["-i", "-M", "master", "-C", "-1", "-x", str(standby_dbid)])
        if disable: self.append("-y")
        if seqserver: self.append("-E")
        return self

    def set_segment(self, mode, content):
        """
        @param mode: mirroring mode
        @param content: content id
        """
        self.extend(["-i", "-M", str(mode), "-C", str(content)])
        return self

    #
    # startup mode options
    #

    def set_special(self, special):
        """
        @param special: special mode (none, 'upgrade' or 'maintenance')
        """
        opt = {None:None, 'upgrade':'-U', 'maintenance':'-m'}[special]
        if opt: self.append(opt)
        return self

    def set_utility(self, utility):
        """
        @param utility: true if starting in utility mode
        """
        if utility: self.append("-c gp_role=utility")
        return self

    def set_restricted(self, restricted, max_connections):
        """
        @param restricted: true if restricting connections
        @param max_connections: connection limit
        """
        if restricted:  self.append("-c superuser_reserved_connections=%s" % max_connections)
        return self


class PgCtlStart(list):
    """
    Used by MasterStart, SegmentStart to format the pg_ctl command
    to start a backend postmaster.

    Examples
    --------

    >>> str(PgCtlStart("/data1/master/gpseg-1", str(PgCtlBackendOptions(5432, 1, 2)), None, None, True, 600))
    'env GPKILL=NEVER $GPHOME/bin/pg_ctl -D /data1/master/gpseg-1 -l /data1/master/gpseg-1/pg_log/startup.log -w -t 600 -o " -p 5432 -b 1 -z 2 --silent-mode=true " start'

    """

    def __init__(self, datadir, backend, wrapper, args, wait, timeout=None):
        """
        @param datadir: database data directory
        @param backend: backend options string from PgCtlBackendOptions
        @param wrapper: wrapper executable for pg_ctl
        @param args: wrapper arguments for pg_ctl
        @param wait: true if pg_ctl should wait until backend starts completely
        @param timeout: number of seconds to wait before giving up
        """

        list.__init__(self, [
            "env",
            "GPKILL=NEVER",
            "$GPHOME/bin/pg_ctl",
            "-D", str(datadir),
            "-l", "%s/pg_log/startup.log" % datadir,
        ])
        if wrapper:
            self.append("--wrapper=\"%s\"" % wrapper)
            if args:
                self.append(" --wrapper-args=\"%s\"" % args)
        if wait: 
            self.append("-w")
        if timeout: 
            self.append("-t")
            self.append(str(timeout))
        self.extend([
            "-o", "\"", str(backend), "\"",
            "start"
        ])

    def __str__(self):
        return " ".join(self)


class PgCtlStop(list):
    """
    Used by MasterStop, SegmentStop to format the pg_ctl command
    to stop a backend postmaster

    >>> str(PgCtlStop("/data1/master/gpseg-1", "smart", True, 600))
    '$GPHOME/bin/pg_ctl -D /data1/master/gpseg-1 -m smart -w -t 600 stop'

    """

    def __init__(self, datadir, mode, wait, timeout):
        """
        @param datadir: database data directory
        @param mode: shutdown mode (smart, fast, immediate)
        @param wait: true if pg_ctlshould wait for backend to stop
        @param timeout: number of seconds to wait before giving up
        """
        list.__init__(self, [
            "$GPHOME/bin/pg_ctl",
            "-D", str(datadir),
            "-m", str(mode),
        ])
        if wait: 
            self.append("-w")
        if timeout: 
            self.append("-t")
            self.append(str(timeout))
        self.append("stop")

    def __str__(self):
        return " ".join(self)

        
if __name__ == '__main__':

    import doctest
    doctest.testmod()
