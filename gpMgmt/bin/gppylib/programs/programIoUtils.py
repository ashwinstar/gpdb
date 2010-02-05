#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
from gppylib import gparray, gplog, pgconf, userinput

logger = gplog.get_default_logger()

def displaySegmentInfo(instanceType, seg):
    paddingLen = 11 - len(instanceType)
    padding = ""
    for i in range(paddingLen):
        padding = padding + " "

    logger.info('%s instance host             %s= %s' % (instanceType, padding, seg.getSegmentHostName()))
    logger.info('%s instance directory        %s= %s' % (instanceType, padding, seg.getSegmentDataDirectory()))
    logger.info('%s instance port             %s= %d' % (instanceType, padding, seg.getSegmentPort()))
    logger.info('%s instance replication port %s= %d' % (instanceType, padding, seg.getSegmentReplicationPort()))

