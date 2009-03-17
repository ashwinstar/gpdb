#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
"""
TODO: module docs
"""
import sys
import os


try: 
    from pygresql import pgdb
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))

from gppylib import gplog

logger = gplog.get_default_logger()


class ConnectionError(StandardError): pass


#TODO: find a better home for this
gpconfig_Query = '''SELECT content, definedprimary, dbid, isprimary,
                           valid, hostname, port, datadir
                    FROM pg_catalog.gp_configuration
                    ORDER BY content, definedprimary DESC '''

def getConfig(conn):
    return execSQL(conn,gpconfig_Query)


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

def parseLine(line):
    fields=line.split('|')
    row=[]
    row.append(int(fields[0]))
    row.append(parseBool(fields[1]))
    row.append(int(fields[2]))
    row.append(parseBool(fields[3]))
    row.append(parseBool(fields[4]))
    row.append(fields[5])
    row.append(int(fields[6]))
    row.append(fields[7].rstrip('\n'))
    return row




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
            self.pguser = os.environ.get('PGUSER', os.environ['USER'])             
        else:
            self.pguser = username

        if password is None:
            self.pgpass = os.environ.get('PGPASS', None)
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


         
