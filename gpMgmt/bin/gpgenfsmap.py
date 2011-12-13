#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2011. All Rights Reserved. 
'''
gpgenfsmap.py     Generates a mapping of Servers, Filesystems, GPDB Filespaces and Storage Pools
Options: 
    -h, -?, --help   Print this usage.
    -p --port        Port to use to connect to DB.  Defaults to $PGPORT
    -u --username    Username to connect to DB.  Defaaults to $PGUSER
    -n --host        Hostname to connect to DB. Defaults to $PGHOST
    -w --password    Password to connect to DB.
    -v, --verbose    Enable verbose Logging.  Adds messages to stdout that will may break programs that parse the fsmap output.
    -q, --quiet      Disable most log messages.  This mode is on by default.
'''

import os, sys
try:
    from optparse import Option, OptionParser 
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.commands.base import WorkerPool, Command, REMOTE
    from gppylib import gphostcache
    from gppylib.commands import unix
    from gppylib.db import dbconn
    from gppylib.db import catalog
    from pygresql   import pg            # Database interaction
    from gppylib import gplog          # Greenplum logging facility
    from gppylib.gpcoverage import GpCoverage
    from getpass import getpass

except ImportError, e:    
    sys.exit('Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e))

#Data Structures to be populated 
class FSMap:
    def __init__(self):
        self.servers = []
        self.filespaces = []

    def addServer(self, name):
        newSvr = Server(name)
        self.servers.append(newSvr)
        return newSvr
    
    def getServer(self, name):
        for server in self.servers:
            if server.name == name:
                return server
        return None
    
    def getAllServers(self):
        return self.servers
    
    def addFilespace(self, filespace):
        self.filespaces.append(filespace)
    
    def toXML(self):
        print "<fsmap>"
        for svr in self.servers:
            svr.toXML()
        print "</fsmap>"
        
    def toString(self):
        mapStr = ""
        for svr in self.servers:
            mapStr += svr.toString()
        return mapStr
            
        

class Server:
    def __init__(self, name):
        self.name = name
        self.filesystems = []

    def addFilesystem(self, name):
        newFs = Filesystem(name)
        self.filesystems.append(newFs)
        return newFs
         
    def getFilesystem(self, name):
        for filesystem in self.filesystems:
            if filesystem.name == name:
                return filesystem
        return None
    
    def toXML(self):
        print "<server name=%s>" % self.name
        for fs in self.filesystems:
            fs.toXML()
        print "</server>"
        
    def toString(self):
        svrStr = ""
        for fs in self.filesystems:
            fsStrList = fs.toString()
            for fsStr in fsStrList:
                svrStr += "%s:%s:%s\n" %(self.name, fs.name, fsStr)
        return svrStr

class Filesystem:
    def __init__(self, name):
        self.name = name
        self.dirs = []

    def addDir(self, path, filespace):
        self.dirs.append((path, filespace))
        
    def toXML(self):
        print "<filesystem name=%s>" % self.name
        for dir in self.dirs:
            print "<dir filespace=%s datadir=%s />" %(dir[1], dir[0])
        print "</filesystem>"
        
    def toString(self):
        dirStrList = []
        for dir in self.dirs:
            dirStr = "%s:%s" %(dir[0], dir[1])
            dirStrList.append(dirStr)
        return dirStrList
            

#Global Variables
systemFSMap  = None
#Keyed on servername, each value is a list of devices & directories
serverFSDict = None
dbConn       = None
logger       = None
    
def runPoolCommand(host, commandStr, pool):
    output = ""

    cmd = Command(host, commandStr, REMOTE, host)
    pool.addCommand(cmd)
    pool.join()
    items = pool.getCompletedItems()
    for i in items:
        if i.results.rc or i.results.halt or not i.results.completed:
            logger.info("Error running command on host: " + host + " Command: " + commandStr)
            logger.info(i.results.stderr.strip())
            logger.info(i.results.stdout.strip())
        output = i.results.stdout
    return output

def dbConnect(options):
    global dbConn
    
    if dbConn != None:
        return
    try:
        user     = options.username
        host     = options.host 
        port     = options.port
        db       = 'template1'
        password = None

        dburl = dbconn.DbURL(username=user, hostname=host, 
                             port=port, dbname=db, password=password)
        logger.info("Connecting to DB at: %s " %(str(dburl)) )
        
        conn = dbconn.connect(dburl)
        logger.info( "Connected to DB: %s" %(str(conn)) )
        dbConn = conn
    except Exception, e:
        logger.error ("Error Connecting to Database: %s" % (str(e)) )
        sys.exit(1)

    
def runQuery(query):
    global dbConn
    if dbConn == None:
        logger.error ( "Error: Not connected to database.")
        sys.exit(1)
    
    try:
        rows = catalog.basicSQLExec(dbConn, query)
    except Exception, e:
        logger.error ("Error: Failed to run query: %s" % (str(e)) )
        sys.exit(1)
    return rows

def parseargs(args):
    global logger
    
    pguser = os.environ.get("PGUSER") or unix.getUserName()
    pghost = os.environ.get("PGHOST") or unix.getLocalHostname()
    pgport = os.environ.get("PGPORT") or 5432
    
    parser = OptParser(option_class=OptChecker)
    parser.remove_option('-h')
    parser.add_option('-?', '--help', '-h', action='store_true', default=False)
    parser.add_option('-n', '--host', default=pghost)
    parser.add_option('-p', '--port', default=pgport)
    parser.add_option('-u', '--username', default=pguser)
    parser.add_option('-w', '--password', default=False, action='store_true')
    parser.add_option('-v', '--verbose', default=False, action='store_true')
    parser.add_option('-q', '--quiet', default=True, action='store_true')
    
    (options, args) = parser.parse_args()
    
    if options.help:
        print __doc__
        sys.exit(1)
    try:
        options.port = int(options.port)
    except:
        logger.error("Invalid PORT: '%s'" % options.port)
        sys.exit(1)
        
    if options.verbose:
        gplog.enable_verbose_logging()
    elif options.quiet:
        gplog.quiet_stdout_logging()

    return options
            
#Grabs the mtab or mnttab from every server and loads it into a dictionary of lists keyed by hostname
def genServerFSDict():
    global systemFSMap
    global serverFSDict
    serverFSDict = dict()
    result = os.uname()
    pool   = WorkerPool()
    
    if result[0] == 'Linux':
        filename = "/etc/mtab"
    elif result[0] == 'SunOS':
        filename = "/etc/mnttab"
    else:
        logger.error("OS not supported.  Exiting")
        sys.exit(1)
    #Get list of servers
    serverList = systemFSMap.getAllServers()
    cmd = "cat " + filename
    for server in serverList:
        servername = server.name
        try:
            output = runPoolCommand(servername, cmd, pool)
        except Exception, e:
            logger.info("Failed to get filesystem list from server: %s Error: %s" %(servername, e.__str__() ))
        else:
            outputLines = output.splitlines()
            mounted_fs_list = []
            for line in outputLines:
                logger.info ("From: " + servername + " Got: " + line)
                mounted_fs_list.append(line.split())
                serverFSDict[servername] = mounted_fs_list
            
    pool.join()
    pool.haltWork()
    pool.joinWorkers()
    logger.debug("serverFSDict: " + str(serverFSDict) )

def genFSMap():
    global systemFSMap
    global serverFSDict
    results = runQuery("SELECT fsname as filespc, fsedbid as seg_dbid, fselocation as datadir, hostname \
                        FROM pg_filespace pgfs, pg_filespace_entry pgfse,  gp_segment_configuration gpsec \
                        WHERE pgfse.fsefsoid=pgfs.oid AND pgfse.fsedbid=gpsec.dbid  ORDER BY seg_dbid;")
    if len(results) == 0:
        logger.error( "No data directories found.  Exiting.")
        sys.exit(1)
    logger.debug( "Results from query:" + str(results) )
    #result columns will be: [0] filespace name, [1] dbid, [2] datadir, [3] hostname
    for result in results:
        filespaceName= result[0]
        #dbid=result[1]
        datadir=result[2]
        hostname=result[3]
        fsname = resolveFilesystemName(hostname, datadir)
        if fsname == None:
            logger.info( "Filesystem not found for  " + hostname + ":" + datadir )
            continue
        svr = systemFSMap.getServer(hostname)
        if svr == None:
            logger.info( "Error Server not found: " + hostname )
            continue
        fs = svr.getFilesystem(fsname)
        if fs == None:
            fs = svr.addFilesystem(fsname)
        fs.addDir(datadir, filespaceName)
        
    
def genServerList():
    global systemFSMap
    serverList = []
    results = runQuery("select distinct hostname from  gp_segment_configuration")
    
    if len(results) == 0:
        logger.error( "Error: gp_segment_configuration empty" )
        sys.exit(1)
        
    for item in results:
        if len(item) > 0:
            systemFSMap.addServer(item[0])
    logger.debug( "serverList set to: %s" % str(systemFSMap.getAllServers())  )

def resolveFilesystemName(server, directory):
    global serverFSDict
    if serverFSDict == None:
        logger.info( "serverFSDict not loaded... Exiting")
        sys.exit(1)
    if server == None or directory == None:
        logger.info( "Failed to resolve Filsystem Name: Bad parameters")
        return None
    
    try:
        fsData = serverFSDict[server]
    except:
        logger.info( "No FS data for server: " + server )
        return None
    
    bestMatch = None
    for entry in fsData:
        mountPoint = entry[1]
        if directory.startswith(mountPoint):
            if bestMatch == None:
                bestMatch = entry
            elif len(mountPoint) > len(bestMatch[1]):
                bestMatch = entry
    if bestMatch != None:
        return bestMatch[0]
    
def main (argv):
    global systemFSMap
    global logger
    
    logger = gplog.get_default_logger()
    
    options = parseargs(argv)
    dbConnect(options)
    systemFSMap = FSMap()
    genServerList()
    genServerFSDict()
    genFSMap()
    
    print systemFSMap.toString()
    
if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

