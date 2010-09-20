#!/usr/bin/env python

class HostType:
    GPCHECK_HOSTTYPE_UNDEFINED  = 0
    GPCHECK_HOSTTYPE_DELL_LINUX = 1

class chkconfig:
    def __init__(self):
        self.services = dict()
        self.xinetd = dict()
        self.errormsg = None

class DellOutputData:
    def __init__(self):
        self.chkconfig = None

    def __str__(self):
        ser = "=============SERVICES=============\n"
        xin = "=============XINETD  =============\n"
        err = "=============ERRORMSG=============\n"
        return "%s%s%s%s%s%s" % (ser, self.chkconfig.services.__str__(), xin, self.chkconfig.xinetd.__str__(), err, self.chkconfig.errormsg)
