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
    def __init__(self,duration,renice_level,interval):
        self.seconds_to_age=duration.seconds
        self.renice_level=renice_level        
        self.interval_in_seconds=interval.seconds
        pass

    
    def run(self):
        
        dburl = dbconn.DbURL()
        self.conn = dbconn.connect(dburl)
        existing_entries = {}
        
        while True:            
            new_entries = {}
            
            sql = """
                SELECT 
                    round(extract( seconds from now() - query_start))  seconds_running,
                    datname,
                    procpid,
                    sess_id,
                    usename,
                    current_query,
                    query_start
                FROM 
                    pg_stat_activity
                WHERE
                    round(extract( seconds from now() - query_start)) > %s
                    AND current_query NOT LIKE '<IDLE>%%'
                """ % self.seconds_to_age
            
            logger.debug("Checking if there are any queries to age")
            
            cursor = dbconn.execSQL(self.conn,sql)
            
            
            for row in cursor:
                (seconds_running,datname,procpid,sess_id,usename,query,query_start)=row
                
                logger.debug("found an entry to consider...")
                if sess_id in existing_entries:
                    if existing_entries[sess_id] == query_start:
                        logger.debug("Skipping since already Aged Session: %d  to database: '%s' for user: '%s' running query: '%s'"
                                     %  (sess_id,datname,usename,query))
                        continue
                else:
                    new_entries[sess_id]= query_start
                
                logger.info("Session: %d  to database: '%s' for user: '%s' running query: '%s' will be aged with: %d"
                            % (sess_id,datname,usename,query,self.renice_level))
                
                sql = """SELECT pg_renice_session(%d,%d) """ % (sess_id,self.renice_level)
                dbconn.execSQL(self.conn,sql)
                
                
            cursor.close()
            self.conn.commit()
            
            existing_entries=new_entries    
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
    
    parser.add_option('-h', '-?', '--help', action='help',
                        help='show this help message and exit')
    parser.add_option('-v','--verbose', action='store_true',
                        help='debug output.')    
    parser.add_option('-d', '--duration', type='duration', metavar='[h][:m[:s]]',
                      help='queries running longer than this value will be re-niced.')
    parser.add_option('-i', '--interval', type='duration', default="00:00:05", metavar='[h][:m[:s]]',
                      help='interval between checks for which queries to age.')
    parser.add_option('-l', '--level', type="int", default=1, metavar="<nice level>",
                        help='nice level to apply (1-20)' )    
    
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
        
        logger.debug("duration: %s" % options.duration)        
        logger.debug("seconds: %s" % options.duration.seconds)
        
        age = GpAge(options.duration,options.level,options.interval)        
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

 