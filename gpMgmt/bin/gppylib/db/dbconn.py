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

try: 
    from pygresql import pgdb
    from gppylib.commands.unix import UserId
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))

from gppylib import gplog

logger = gplog.get_default_logger()


class ConnectionError(StandardError): pass

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
                    line = line.strip()
                    if line.startswith('#'):
                        continue
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
               (entry['port'] == str(port) or entry['port'] == '*') and
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
            self.pghost = os.environ.get('PGHOST', 'localhost')
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
            self.pguser = os.environ.get('PGUSER', os.environ.get('USER', UserId.local('Get uid')))             
            if self.pguser is None or self.pguser == '':
                raise Exception('Both $PGUSER and $USER env variables are not set!')
        else:
            self.pguser = username

        if password is None:
            pgpass = Pgpass()
            if pgpass.pgpass_valid():
                password = pgpass.get_password(self.pguser, self.pghost, self.pgport, self.pgdb)
                if password:
                    self.pgpass = password
                else:
                    self.pgpass = os.environ.get('PGPASSWORD', None)
        else:
            self.pgpass = password

        
    
    def __str__(self):
        return "%s:%d:%s:%s:%s" % \
            (self.pghost,self.pgport,self.pgdb,self.pguser,self.pgpass)    




def connect(dburl, utility=False, verbose=False, encoding=None):
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

    # set client encoding if needed
    if encoding:
        cursor=conn.cursor()
        cursor.execute("SET CLIENT_ENCODING='%s'" % encoding)
        conn.commit()
        cursor.close()   
        
    return conn 


def execSQL(conn,sql):    
    cursor=conn.cursor()
    cursor.execute(sql)
    return cursor

def execSQLForSingletonRow(conn, sql):
    """
    Run SQL that returns exactly one row, and return that one row
    """
    cursor=conn.cursor()
    cursor.execute(sql)

    if cursor.rowcount != 1 :
        raise Exception("SQL retrieved %s rows but 1 was expected:\n%s" % \
                        (cursor.rowcount, sql))

    res = cursor.fetchall()[0]
    cursor.close()
    return res

def executeUpdateOrInsert(conn, sql, expectedRowUpdatesOrInserts):
    cursor=conn.cursor()
    cursor.execute(sql)
    
    if cursor.rowcount != expectedRowUpdatesOrInserts :
        raise Exception("SQL affected %s rows but %s were expected:\n%s" % \
                        (cursor.rowcount, expectedRowUpdatesOrInserts, sql))
    return cursor

         
