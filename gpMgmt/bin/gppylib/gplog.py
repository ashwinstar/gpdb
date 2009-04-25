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


def setup_helper_tool_logging(appName,hostname,userName):
    """ for cmd line utilities that get called by tools.  we want the same
        log format but we dont want to log to ~/gpAdminLogs
    """
    logger = get_default_logger()
    logger.name="%s:%s" % (hostname,userName)
    return logger

def setup_tool_logging(appName,hostname,userName,logdir=None,nonuser=False):
    """ sets up the logging as per standard GP Tools should  """
    logger = get_default_logger()
    logger.name="%s:%s" % (hostname,userName)
    
    if nonuser:
        appName=appName + "_" + logger.name
    enable_gpadmin_logging(appName,logdir)
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


def logging_is_verbose():
    return LOGGER.getEffectiveLevel() == logging.DEBUG    
    
def logging_is_quiet():
    return LOGGER.getEffectiveLevel() == logging.WARN

def get_logfile():
    global FILENAME
    return FILENAME

    
#------------------------------- Private --------------------------------    

def set_file_logging(filename): 
    global LOGGER,SOUT_HANDLER,FILENAME
    FILENAME=filename   
    FILE_HANDLER = logging.FileHandler( filename, 'a')
    FILE_HANDLER.setFormatter(get_default_formatter())
    LOGGER.addHandler(FILE_HANDLER)


def get_default_formatter():
    return logging.Formatter("%(asctime)s:%(filename)s:%(name)s-[%(levelname)-s]:-%(message)s","%Y%m%d:%H:%M:%S")

    
def enable_gpadmin_logging(name,logdir=None):
    global FILE_HANDLER
        
    logger = get_default_logger()
    now = datetime.date.today()
    
    if logdir is None:
        homeDir=os.path.expanduser("~")
        gpadmin_logs_dir=homeDir + "/gpAdminLogs"
    else:
        gpadmin_logs_dir=logdir
    
    if not os.path.exists(gpadmin_logs_dir):
        os.mkdir(gpadmin_logs_dir)
    
    filename = "%s/%s_%s.log" % (gpadmin_logs_dir,name, now.strftime("%Y%m%d"))
    set_file_logging(filename)    
    pass
