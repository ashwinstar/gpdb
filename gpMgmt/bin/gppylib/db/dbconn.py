#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
"""
TODO: module docs
"""
import os


try: 
    import pg8000
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))


class ConnectionError(StandardError): pass


#TODO: find a better home for this
gpconfig_Query = '''SELECT content, definedprimary, dbid, isprimary,
                           valid, hostname, port, datadir
                    FROM pg_catalog.gp_configuration
                    ORDER BY content, definedprimary DESC '''

def getConfig(conn):
    rows = PgConn.execute(conn,gpconfig_Query)

    rowList = []
    for row in rows:
        rowList.append(row)


    return rowList


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
    row={}
    row['content']=int(fields[0])
    row['definedprimary']=parseBool(fields[1])
    row['dbid']=int(fields[2])
    row['isprimary']=parseBool(fields[3])
    row['valid']=parseBool(fields[4])
    row['hostname']=fields[5]
    row['port']=int(fields[6])
    row['datadir']=fields[7].rstrip('\n')
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
        return "%s:%d/%s?user=%s?pass=%s" % \
            (self.pghost,self.pgport,self.pgdb,self.pguser,self.pgpass)    




def connect(dburl, utility=False):
    if utility:
        options = '-c gp_session_role=utility'
    else:
        options = ''
            
    conn = pg8000.DBAPI.connect(host=dburl.pghost, 
                                  user=dburl.pguser,
                                  port=dburl.pgport,
                                  database = dburl.pgdb,
                                  options=options)
    return conn 


def execSQL(conn,sql):    
    cursor=conn.cursor()
    cursor.execute(sql)
    return cursor



#-=-=-=-=-=-

class PgConn:
    """ PgConn is a basic connection wrapper around Pg8000 for connecting
        to a GP database (including segment dbs)
    
    """
    
    @staticmethod    
    def connect(dburl, utility=False):
        """Used to establish a connection to the database specified by
           the dburl.  
           
           Returns - a pg8000 Connection object.
        """
        
        if utility:
            options = '-c gp_session_role=utility'
        else:
            options = ''
        
        try:    
            conn = pg8000.Connection(user=dburl.pguser,
                                     host=dburl.pghost,
                                     port=dburl.pgport,
                                     database = dburl.pgdb,
                                     options=options)
            return conn
        except Exception, e:            
            raise ConnectionError(str(e))
            

    @staticmethod
    def execute(conn, qry):
        try:
            rows = []
            conn.execute(qry)
            iter=conn.iterate_dict()
            for row in iter:
                rows.append(row)
            return rows
        except Exception, e:
            conn.close()
            raise ConnectionError('%s\nsql> %s' % (str(e), qry))
            
