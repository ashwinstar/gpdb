#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
"""
TODO: module docs
"""
import sys
import os
import stat
from collections import namedtuple

try: 
    from pygresql import pgdb
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))

from gppylib import gplog

logger = gplog.get_default_logger()

# Here's where we create our named-tuple.
# we'll access our fields based on the field name.
SegmentTuple = namedtuple("SegmentTuple", "dbid content role preferred_role mode status hostname address port datadir replicationPort")

class ConnectionError(StandardError): pass


#TODO: find a better home for this
#gpconfig_Query = '''SELECT content, definedprimary, dbid, isprimary,
#                           valid, hostname, port, datadir
#                    FROM pg_catalog.gp_configuration
#                    ORDER BY content, definedprimary DESC '''
gpconfig_Query = '''
  SELECT dbid, content, role, preferred_role, mode, status, hostname, address, port, 
        fselocation as datadir, replication_port
  FROM pg_catalog.gp_segment_configuration
  JOIN pg_catalog.pg_filespace_entry on (dbid = fsedbid)
  JOIN pg_catalog.pg_filespace fs on (fsefsoid = fs.oid and fsname='pg_system')
  ORDER BY content, preferred_role DESC
'''

def getConfig(conn):
    return execSQL(conn,gpconfig_Query)

gpfailover_Query = '''SELECT fault_strategy FROM gp_fault_strategy'''

def getFaultStrategy(conn):
    return execSQL(conn,gpfailover_Query)

def parseBool(val):
    if val == 't':
        return True
    else:
        return False

def parseRow(row):
    pass
    #row['definedprimary'] = GpConfig.parseBool(row['definedprimary'])
    #row['isprimary'] = GpConfig.parseBool(row['isprimary'])
    #print "row valid? " + str(row['valid'])
    #row['valid'] = GpConfig.parseBool(row['valid'])
    #print "row valid2? " + str(row['valid'])

def parseListIntoTuple(fields):
    return(SegmentTuple(dbid=int(fields[0]), content=int(fields[1]), role=fields[2],
                       preferred_role=fields[3], mode=fields[4], status=fields[5],
                       hostname=fields[6], address=fields[7], port=int(fields[8]), datadir=fields[9],
                       replicationPort=fields[10]))

# We return a named-tuple
def parseLine(line):
    fields=line.split('|')

    return parseListIntoTuple(fields)

class Pgpass():
    """ Class for handling .pgpass file.
    """
    entries = []
    valid_pgpass = True
    
    def __init__(self):
        HOME = os.getenv('HOME')
        PGPASSFILE = os.getenv('PGPASSFILE', '%s/.pgpass' % HOME)
        
        if not os.path.exists(PGPASSFILE):
            return
        
        st_info = os.stat(PGPASSFILE)
        mode = str(oct(st_info[stat.ST_MODE] & 0777))
        
        if mode != "0600":
            print 'WARNING: password file "%s" has group or world access; permissions should be u=rw (0600) or less' % PGPASSFILE
            self.valid_pgpass = False
            return
        
        try:
            fp = open(PGPASSFILE, 'r')
            try:
                lineno = 1
                for line in fp:
                    try:
                        (hostname, port, database, username, password) = line.strip().split(':')
                        entry = {'hostname': hostname,
                                 'port': port,
                                 'database': database,
                                 'username': username,
                                 'password': password }
                        self.entries.append(entry)
                    except:
                        print 'Invalid line in .pgpass file.  Line number %d' % lineno 
                    lineno += 1
            except IOError:
                pass
            finally:
                if fp: fp.close()
        except OSError:
            pass

    
    def get_password(self, username, hostname, port, database):
        for entry in self.entries:
            if ((entry['hostname'] == hostname or entry['hostname'] == '*') and
               (entry['port'] == port or entry['port'] == '*') and
               (entry['database'] == database or entry['database'] == '*') and
               (entry['username'] == username or entry['username'] == '*')):
                return entry['password']
        return None
    
    def pgpass_valid(self):
        return self.valid_pgpass
        
class DbURL:
    """ DbURL is used to store all of the data required to get at a PG 
        or GP database.
    
    """
    pghost='foo'
    pgport=5432
    pgdb='template1'
    pguser='username'
    pgpass='pass'    

    def __init__(self,hostname=None,port=0,dbname=None,username=None,password=None):
        
        if hostname is None:
            self.pghost = os.environ.get('PGHOST', '127.0.0.1')
        else:
            self.pghost = hostname
            
        if port is 0:
            self.pgport = int(os.environ.get('PGPORT', '5432'))
        else:
            self.pgport = int(port)
                    
        if dbname is None:
            self.pgdb = os.environ.get('PGDATABASE', 'template1')
        else:
            self.pgdb = dbname

        if username is None:
            self.pguser = os.environ.get('PGUSER', os.environ.get('USER'))             
            if self.pguser is None:
                raise Exception('$USER env variable is not set!')
        else:
            self.pguser = username

        if password is None:
            self.pgpass = os.environ.get('PGPASSWORD', None)
        else:
            self.pgpass = password

        
    
    def __str__(self):
        return "%s:%d:%s:%s:%s" % \
            (self.pghost,self.pgport,self.pgdb,self.pguser,self.pgpass)    




def connect(dburl, utility=False, verbose=False):
    if utility:
        options = '-c gp_session_role=utility'
    else:
        options = ''
    
    dsn_str = "" + str(dburl) + ": " + options + ":1"
    conn = pgdb.connect(dsn=dsn_str)
    
    #by default, libpq will print WARNINGS to stdout
    if not verbose:
        cursor=conn.cursor()
        cursor.execute("SET CLIENT_MIN_MESSAGES='ERROR'")
        conn.commit()
        cursor.close()
    return conn 


def execSQL(conn,sql):    
    cursor=conn.cursor()
    cursor.execute(sql)
    return cursor


         
