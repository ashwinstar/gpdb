#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
#
# Internal Use Function.
#
import os,sys
import time

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

import figleaf

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib import gplog
from gppylib.commands import base
from gppylib.commands import unix
from gppylib.commands import gp
from gppylib.db import catalog
from gppylib.db import dbconn
from gppylib import logfilter

description = (""" 
This utility is NOT SUPPORTED and is for internal-use only.

It is used to priority age running queries.""")
_help = ["""
"""]

EXECNAME = os.path.split(__file__)[-1]

    
#-------------------------------------------------------------------------    
class GpAge():
    ######
    def __init__(self,duration,renice_level,interval,dburl):
        self.seconds_to_age=duration.seconds
        self.renice_level=renice_level        
        self.interval_in_seconds=interval.seconds
	self.dburl=dburl
	self.conn=""
        pass

    
    def run(self):
        
#        dburl = dbconn.DbURL()
	try:
            self.conn = dbconn.connect(self.dburl)
    	except Exception, e:
	    raise SystemExit, "Database connection failed. Reason=%s" % e

        existing_entries = {}
        
        while True:            
            sql = """
                SELECT 
                    round(extract(epoch from now()) - extract(epoch from query_start))  seconds_running,
                    datname,
                    procpid,
                    sess_id,
                    usename,
                    current_query,
                    query_start
                FROM 
                    pg_stat_activity
                WHERE
                    current_query NOT LIKE '<IDLE>%%'
                    AND round(extract(epoch from now()) - extract(epoch from query_start)) > %s
                """ % self.seconds_to_age
            
            logger.debug("Checking if there are any queries to age")
            
            cursor = dbconn.execSQL(self.conn,sql)
            
            for row in cursor:
                (seconds_running,datname,procpid,sess_id,usename,query,query_start)=row
                
		logger.debug("found an entry to consider, session: %d start '%s'" % (sess_id,query_start))
                if sess_id in existing_entries:
                    if existing_entries[sess_id] == query_start:
                        logger.debug("Skipping since already Aged Session: %d  to database: '%s' for user: '%s' running query: '%s'"
                                     %  (sess_id,datname,usename,query))
                        continue
                else:
                    existing_entries[sess_id]= query_start
                
                logger.info("Session: %d  to database: '%s' for user: '%s' running query: '%s' will be aged with: %d"
                            % (sess_id,datname,usename,query,self.renice_level))
                
                sql = """SELECT pg_renice_session(%d,%d) """ % (sess_id,self.renice_level)
                dbconn.execSQL(self.conn,sql)
                
                
            cursor.close()
            self.conn.commit()
            
            time.sleep(self.interval_in_seconds)
            
        conn.close()
                
                
    def cleanup(self):
        if self.conn:
            self.conn.close()
        
        
#-------------------------------------------------------------------------
def parseargs():
    parser = OptParser(option_class=OptChecker,
                description=' '.join(description.split()),
                version='%prog version $Revision: #12 $')
    parser.setHelp(_help)
    parser.set_usage('%prog [--help] [options] ')
    parser.remove_option('-h')
    
    parser.add_option('-?', '--help', action='help',
                        help='show this help message and exit')
    parser.add_option('-v','--verbose', action='store_true',
                        help='debug output.')    
    parser.add_option('-D', '--duration', type='duration', default="00:05:00", metavar='[h][:m[:s]]',
                      help='queries running longer than this value will be re-niced.')
    parser.add_option('-i', '--interval', type='duration', default="00:00:05", metavar='[h][:m[:s]]',
                      help='interval between checks for which queries to age.')
    parser.add_option('-l', '--level', type="int", default=1, metavar="<nice level>",
                        help='nice level to apply (1-20)' )    
    parser.add_option('-h', '--host', type="string", default="", metavar="<host name>",
                        help='host for database connection' )    
    parser.add_option('-p', '--port', type="int", default="5432", metavar="<port number>",
                        help='port for database connection' )    
    parser.add_option('-d', '--pgdb', type="string", default="", metavar="<database name>",
                        help='database name for database connection' )    
    parser.add_option('-U', '--pguser', type="string", default="", metavar="<database user name>",
                        help='user name for database connection' )    
    parser.add_option('-P', '--pgpassword', type="string", default="", metavar="<database password>",
                        help='password for database connection' )    
    
    parser.set_defaults(verbose=False, filters=[], slice=(None, None))
    
    # Parse the command line arguments
    (options, args) = parser.parse_args()

    return options, args



#-------------------------------------------------------------------------
    
def main():
    
    age = None
        
    try:
        gplog.setup_tool_logging(EXECNAME,unix.getLocalHostname(),unix.getUserName())
        logger.info("Starting gpage with args: %s" % ' '.join(sys.argv[1:]))
        
        options, args = parseargs()        
        if options.verbose:
            gplog.enable_verbose_logging()
        
        logger.info("duration of queries that will have priorities reduced: %s" % options.duration)        
        logger.info("polling frequency: %s seconds" % options.duration.seconds)
        logger.info("Beginning polling, going quiet, will print out queries that are re-prioritized...")

        dburl = dbconn.DbURL(hostname=options.host,port=options.port,dbname=options.pgdb,username=options.pguser,password=options.pgpassword)

        age = GpAge(options.duration,options.level,options.interval,dburl)        
        logger.debug("Initialized GpAge...")
        age.run()
             
        sys.exit(0)
    except Exception, e:
        logger.exception("gpage.py failed. (Reason=%s) exiting..." % e)
        sys.exit(3)
    except KeyboardInterrupt:
        sys.exit('\nUser Interrupted')
    finally:
        if age: age.cleanup()
 
logger = gplog.get_default_logger()    
if __name__ == '__main__':
    main()

 
