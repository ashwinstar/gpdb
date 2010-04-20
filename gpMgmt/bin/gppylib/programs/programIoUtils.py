#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
from gppylib import gparray, gplog, pgconf, userinput

logger = gplog.get_default_logger()

def appendSegmentInfoForOutput(instanceType, gpArray, seg, linesOut):

    linesOut.append(['%s instance host' % instanceType, "= " + seg.getSegmentHostName()])
    linesOut.append(['%s instance directory' % instanceType, "= " + seg.getSegmentDataDirectory()])
    linesOut.append(['%s instance port' % instanceType, "= " + str(seg.getSegmentPort())])
    linesOut.append(['%s instance replication port' % instanceType, "= " + str(seg.getSegmentReplicationPort())])

    for fs in gpArray.getFilespaces(False):
        path = seg.getSegmentFilespaces()[fs.getOid()]
        linesOut.append(['%s instance %s directory' % (instanceType, fs.getName()), "= " + path])



