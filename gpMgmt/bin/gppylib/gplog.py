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
SOUT_HANDLER=None
FILE_HANDLER=None

#------------------------------- Public Interface --------------------------------
def get_default_logger():
    """
    Return a singleton logger.
    """
    global LOGGER,SOUT_HANDLER
    if LOGGER is None:
        LOGGER = logging.getLogger('default')
        f = get_default_formatter()
        SOUT_HANDLER = logging.StreamHandler(sys.stdout)
        SOUT_HANDLER.setFormatter(f)
        LOGGER.addHandler(SOUT_HANDLER)
        LOGGER.setLevel(logging.INFO)
    return LOGGER

def get_unittest_logger():
    global LOGGER,SOUT_HANDLER
    if LOGGER is None:
        LOGGER = logging.getLogger('default')
        f = get_default_formatter()
        filename="unittest.log"
        set_file_logging(filename)
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


def quiet_stdout_logging():
    """ reduce log level for stdout logging """
    
    #TODO:  this should ideally only quiet the stdout handler but still
    #       log normally to the file handler.
    global SOUT_HANDLER
    SOUT_HANDLER.setLevel(logging.WARN)
    
    
    
#------------------------------- Private --------------------------------    

def set_file_logging(filename): 
    global LOGGER,SOUT_HANDLER   
    FILE_HANDLER = logging.FileHandler( filename, 'a')
    FILE_HANDLER.setFormatter(get_default_formatter())
    LOGGER.addHandler(FILE_HANDLER)

def get_default_formatter():
    return logging.Formatter("%(asctime)s:%(filename)s:%(name)s-[%(levelname)-s]:-%(message)s","%Y%m%d:%H:%M:%S")

    
def enable_gpadmin_logging(name):
    global FILE_HANDLER
        
    logger = get_default_logger()
    now = datetime.date.today()
    homeDir=os.path.expanduser("~")
    
    filename = "%s/gpAdminLogs/%s_%s.log" % (homeDir,name, now.strftime("%Y%m%d"))
    set_file_logging(filename)    
    pass
