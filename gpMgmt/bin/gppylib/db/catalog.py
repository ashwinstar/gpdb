#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
""" Provides Access Utilities for Examining and Modifying the GP Catalog.

"""
import copy


import dbconn
from  gppylib import gplog

logger=gplog.get_default_logger()


class CatalogError(Exception): pass

def basicSQLExec(conn,sql):
    cursor=None
    try:
        cursor=dbconn.execSQL(conn,sql)
        rows=cursor.fetchall()
        return rows
    finally:
        if cursor:
            cursor.close()


def getSessionGUC(conn,gucname):
    sql = "SHOW %s" % gucname
    return basicSQLExec(conn,sql)[0][0]            

def getUserDatabaseList(conn):
    sql = "SELECT datname FROM pg_catalog.pg_database WHERE datname NOT IN ('postgres','template1','template0') ORDER BY 1"
    return basicSQLExec(conn,sql)


def getDatabaseList(conn):
    sql = "SELECT datname FROM pg_catalog.pg_database"
    return basicSQLExec(conn,sql)


def getActiveUserPIDs(conn):
    sql = """
        SELECT 
            procpid 
        FROM 
            pg_stat_activity 
        WHERE 
            procpid != pg_backend_pid() AND current_query<>'idle'        
        """
    rows = basicSQLExec(conn,sql)
    return rows

def getUserPIDs(conn):
    """dont count ourselves"""
    sql = """SELECT procpid FROM pg_stat_activity WHERE procpid != pg_backend_pid()"""
    return basicSQLExec(conn,sql)
    
    
    
def cancelBackendPIDs(conn):
    sql = """
        SELECT 
            pg_cancel_backend(procpid) 
        FROM 
            (SELECT procpid FROM pg_stat_activity WHERE procpid != pg_backend_pid()) as s;
        """
    rows = basicSQLExec(conn,sql)
    
        
    
def getCollationSettings(conn):
    sql = """ 
    SELECT current_setting('lc_collate') as lc_collate,
           current_setting('lc_monetary') as lc_monetary,
           current_setting('lc_numeric') as lc_numeric;        
    """
    rows = basicSQLExec(conn,sql)
    
    return (rows[0][0],rows[0][1],rows[0][2])

        
def doesSchemaExist(conn,schemaname):
    sql = "SELECT nspname FROM pg_catalog.pg_namespace WHERE nspname = '%s'" % schemaname        
    cursor=None
    try:
        cursor=dbconn.execSQL(conn,sql)
        numrows = cursor.rowcount
        if numrows == 0:
            return False
        elif numrows == 1:
            return True
        else:
            raise CatalogError("more than one entry in pg_namespace for '%s'" % schemaname)
    finally:
        if cursor: 
            cursor.close()


def get_catalogtable_list(conn):
    sql = """SELECT schemaname || '.' ||  tablename 
             FROM pg_tables 
             WHERE schemaname = 'pg_catalog'
          """
    cursor=None
    try:
        cursor=dbconn.execSQL(conn,sql)
        
        return cursor.fetchall()
    finally:
        if cursor:
            cursor.close()

def get_usertable_list(conn):    
    sql = """SELECT schemaname || '.' ||  tablename 
             FROM pg_tables 
             WHERE schemaname <> 'pg_catalog'
          """
    return basicSQLExec(conn,sql)


def vacuum_catalog(dburl,conn,full=False,utility=False):
    """ Will use the provided connection to enumerate the list of databases
        and then connect to each one in turn and vacuum full all of the 
        catalog files
        
        TODO:  There are a few tables that are cluster-wide that strictly speaking
               don't need to be vacuumed for each database.  These are most likely
               small and so perhaps isn't worth the added complexity to optimize them.               
    
        WARNING:  doing a vacuum full on the catalog requires that 
        there aren't any users idle in a transaction as they typically
        hold catalog share locks.  The result is this vacuum will wait forever on
        getting the lock.  This method is best called when no one else
        is connected to the system.  our own connections are typically idle
        in transactiona and so are especially bad.
    """
    dblist = getDatabaseList(conn)
    catlist = get_catalogtable_list(conn)
    conn.commit()
    
    for db in dblist:
        test_url = copy.deepcopy(dburl)
        test_url.pgdb = db[0]
        
        if db[0] == 'template0' or db[0] == 'postgres':
            continue
        
        vac_conn = dbconn.connect(test_url,utility)
        vac_curs = vac_conn.cursor()        
        vac_curs.execute("COMMIT")
        vac_curs.execute("SET CLIENT_MIN_MESSAGES='ERROR'")
        for table in catlist:
            logger.debug('Vacuuming %s %s' % (db[0],table[0]) )
            
            if full:
                sql = "VACUUM FULL %s" % table[0]
            else:
                sql = "VACUUM %s" % table[0]
            
            vac_curs.execute(sql)
            
        
        vac_curs.execute(sql)        
        vac_conn.commit()
        vac_conn.close()

        

class CatalogTable():
    
    def __init__(self):
        raise CatalogError("Programmer Error!  CatalogTable is an abstract class")
    
    def update_db(self,conn):
        try:
            cursor=conn.cursor()
            cursor.execute(self.get_update_sql())
        finally:
            if cursor: cursor.close()
    
    def get_update_sql(self):
        raise CatalogError("Programmer Error!  subclasses should override")
    
    

class GpIdTable(CatalogTable):
    """ pg_catalog.gp_id Table
    
    template1=# \d pg_catalog.gp_id
          Table "pg_catalog.gp_id"
       Column    |   Type   | Modifiers 
    -------------+----------+-----------
     gpname      | name     | not null
     numsegments | smallint | not null
     dbid        | smallint | not null
     content     | smallint | not null
    Tablespace: "pg_global"

    """
    gpname=None
    numsegments=-1
    dbid=-1
    content=-100
    
    def __init__(self,conn):
        try:
            cursor=conn.cursor()
            cursor.execute(self.get_select_sql())
            if cursor.rowcount != 1:
                raise CatalogError("pgcatalog.gp_id had unexpected row count "\
                                   "of: %d.  Was expecting 1." % cursor.rowcount)
            row = cursor.fetchone()
            self.gpname = row[0]
            self.numsegments = int(row[1])
            self.dbid = int(row[2])
            self.content = int(row[3])
            
        finally:
            if cursor: cursor.close()
    
    
    def set(self,dbid=None,content=None,numsegments=None):
        if dbid is not None:
            self.dbid = dbid
        if content is not None:
            self.content = content
        if numsegments is not None:
            self.numsegments = numsegments
            
            
    def get_select_sql(self):
        return "SELECT * FROM pg_catalog.gp_id"
    
    
    def get_update_sql(self):
        return "UPDATE pg_catalog.gp_id SET gpname = '%s', numsegments = %d, "\
               "dbid = %d, content=%d" % (self.gpname,self.numsegments,self.dbid,self.content) 
    


class GpVersionAtInitdbTable(CatalogTable):  
    """ pg_catalog.gp_version_at_initdb
    
    template1=# \d pg_catalog.gp_version_at_initdb;
    Table "pg_catalog.gp_version_at_initdb"
         Column     |   Type   | Modifiers 
    ----------------+----------+-----------
     schemaversion  | smallint | not null
     productversion | text     | 
    Tablespace: "pg_global"
    
    """
    schemaversion=-1
    productversion=None
    
    def __init__(self,conn):
        try:
            cursor = conn.cursor()
            cursor.execute(self.get_select_sql())
            if cursor.rowcount != 1:
                raise CatalogError("pgcatalog.gp_version_at_initdb had unexpected row count "\
                                   "of: %d.  Was expecting 1." % cursor.rowcount)
            row = cursor.fetchone()
            self.schemaversion = int(row[0])
            self.productversion = row[1]
        finally:
            if cursor: cursor.close()
    
    def get_select_sql(self):
        return "SELECT * FROM pg_catalog.gp_version_at_initdb"
    
    def get_update_sql(self):
        return "UPDATE pg_catalog.gp_version_at_initdb SET schemaversion = %d, "\
               " productversion = '%s' " % (self.schemaversion,self.productversion)



class GpConfigurationTable(CatalogTable):
    """pg_catalog.gp_configuration
    
template1=# \d gp_configuration
  Table "pg_catalog.gp_configuration"
     Column     |   Type   | Modifiers 
----------------+----------+-----------
 content        | smallint | not null
 definedprimary | boolean  | not null
 dbid           | smallint | not null
 isprimary      | boolean  | not null
 valid          | boolean  | not null
 hostname       | name     | not null
 port           | integer  | not null
 datadir        | text     | 
Indexes:
    "gp_configuration_content_definedprimary_index" UNIQUE, btree (content, definedprimary), tablespace "pg_global"
    "gp_configuration_dbid_index" UNIQUE, btree (dbid), tablespace "pg_global"
Tablespace: "pg_global"

    """
    
    
    @staticmethod
    def addRows(conn,segments):
        try:
            cursor=conn.cursor()
            
            for seg in segments:
                sql = """INSERT INTO pg_catalog.gp_configuration(content, definedprimary, dbid, isprimary,                 
                                                                valid, hostname, port, datadir)
                         VALUES (%d, \'%s\', %d, \'%s\', \'t\', \'%s\', %d, \'%s\')
                """ % (seg.content, seg.definedprimary,seg.dbid, seg.isprimary,seg.hostname,
                       seg.port, seg.datadir)
                cursor.execute(sql)
            conn.commit()
        finally:
            if cursor: cursor.close()        
#TODO:
class GpMasterMirroring(CatalogTable):  pass
class GpDistributionPolicy(CatalogTable): pass
    

        