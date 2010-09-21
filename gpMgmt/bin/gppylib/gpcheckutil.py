#!/usr/bin/env python

class HostType:
    GPCHECK_HOSTTYPE_UNDEFINED  = 0
    GPCHECK_HOSTTYPE_DELL_LINUX = 1

class omreport:
    def __init__(self):
        self.biossetup = dict() # key value pairs
        self.biossetup_errormsg = None
        self.bootorder = list() # list of Devices in order of boot
        self.bootorder_errormsg = None

class chkconfig:
    def __init__(self):
        self.services = dict() # hash of services, each entry is hash of run levels and boolean value
        self.xinetd = dict()   # hash of services, value is boolean
        self.errormsg = None

class DellOutputData:
    def __init__(self):
        self.chkconfig = None
        self.omreport = None

    def __str__(self):
        ser = "=============SERVICES=======================\n"
        xin = "=============XINETD  =======================\n"
        err = "=============CHKCONFIG ERRORMSG=============\n"
        output = "%s%s\n%s%s\n%s%s" % (ser, self.chkconfig.services.__str__(), xin, self.chkconfig.xinetd.__str__(), err, self.chkconfig.errormsg)

        omr = "=============OMREPORT BIOSSETUP=============\n"
        ome = "=============OMREPORT BIOSSETUP ERRORMSG====\n"
        output = "%s\n%s%s\n%s%s" % (output, omr, self.omreport.biossetup.__str__(), ome,self.omreport.biossetup_errormsg)

        boo = "=============OMREPORT BOOTORDER=============\n"
        boe = "=============OMREPORT BOOTORDER ERRORMSG====\n"
        output = "%s\n%s%s\n%s%s" % (output, boo, self.omreport.bootorder.__str__(), boe, self.omreport.bootorder_errormsg)

        return output
    
