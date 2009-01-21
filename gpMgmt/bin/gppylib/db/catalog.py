#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
""" Provides Access Utilities for Examining and Modifying the GP Catalog.

"""

import dbconn

class CatalogError(Exception): pass


def getDatabaseList(conn):
    sql = "SELECT datname FROM pg_catalog.pg_database"
    cursor=None
    try:
        cursor=dbconn.execSQL(conn,sql)
        rows=cursor.fetchall()
        return rows
    finally:
        if cursor: 
            cursor.close()

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



#TODO:
class GpMasterMirroring(CatalogTable):  pass
class GpDistributionPolicy(CatalogTable): pass
    

        