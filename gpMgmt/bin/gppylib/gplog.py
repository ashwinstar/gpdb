#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
'''
Greenplum logging facilities.

This Module contains some helper functions for setting up the 
python builtin logging module.  Tools and libraries are expected
to centralize configuration of logging through these functions.
'''
import datetime
import logging
import os
import sys


#evil global
LOGGER=None


#------------------------------- Public Interface --------------------------------
def get_default_logger():
    """
    Return a singleton logger.
    """
    global LOGGER
    if LOGGER is None:
        LOGGER = logging.getLogger('default')
        f = get_default_formatter()
        h = logging.StreamHandler(sys.stdout)
        h.setFormatter(f)
        LOGGER.addHandler(h)
        LOGGER.setLevel(logging.INFO)
    return LOGGER


def setup_tool_logging(appName,hostname,userName):
    """ sets up the logging as per standard GP Tools should  """
    logger = get_default_logger()
    logger.name="%s:%s" % (hostname,userName)
    enable_gpadmin_logging(appName)
    return logger


def enable_verbose_logging():
    """ increases the log level to be verbose """
    LOGGER.setLevel(logging.DEBUG)


def enable_quiet_logging():
    """ reduce log level """
    LOGGER.setLevel(logging.WARN)
    
    
    
#------------------------------- Private --------------------------------    

def get_default_formatter():
    return logging.Formatter("%(asctime)s:%(filename)s:%(name)s-[%(levelname)-s]:%(message)s","%Y%m%d:%H:%M:%S")

    
def enable_gpadmin_logging(name):    
    logger = get_default_logger()
    now = datetime.date.today()
    homeDir=os.path.expanduser("~")
    
    filename = "%s/gpAdminLogs/%s_%s.log" % (homeDir,name, now.strftime("%Y%m%d"))
    h = logging.FileHandler( filename, 'a')
    h.setFormatter(get_default_formatter())
    logger.addHandler(h)
    pass
