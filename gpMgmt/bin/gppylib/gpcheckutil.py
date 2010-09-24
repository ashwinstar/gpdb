#!/usr/bin/env python

class HostType:
    GPCHECK_HOSTTYPE_UNDEFINED  = 0
    GPCHECK_HOSTTYPE_DELL_LINUX = 1

class sysctl:
    def __init__(self):
        self.variables = dict() # dictionary of values
        self.errormsg = None

class omreport:
    def __init__(self):
        self.biossetup = dict()     # key value pairs
        self.biossetup_errormsg = None

        self.bootorder = list()     # list of Devices in order of boot
        self.bootorder_errormsg = None

        self.remoteaccess = dict()  # key value pairs
        self.remoteaccess_errormsg = None

        self.vdisks = list()        # list of dicts, 1 for each virtual disk
        self.vdisks_errormsg = None 

class chkconfig:
    def __init__(self):
        self.services = dict() # hash of services, each entry is hash of run levels and boolean value
        self.xinetd = dict()   # hash of services, value is boolean
        self.errormsg = None

class grubconf:
    def __init__(self):
        self.serial_declaration = False
        self.terminal_declaration = False
        self.ttyS1_declaration = False
        self.errormsg = None

    def __str__(self):
        return "serial_declaration(%s) terminal_declaration(%s) ttyS1_declaration(%s)" % (self.serial_declaration, self.terminal_declaration, self.ttyS1_declaration)

class inittab:
    def __init__(self):
        self.s1 = False
        self.defaultRunLevel = None
        self.errormsg = None

    def __str__(self):
        return "s1_declaration(%s) default_run_level(%s)" % (self.s1, self.defaultRunLevel)

class securetty:
    def __init__(self):
        self.errormsg = None
        self.data = set() 

class bcu:
    def __init__(self):
        self.firmware = None
        self.errormsg = None

    def __str__(self):
        return "firmware_version=%s" % self.firmware

class ioschedulers:
    def __init__(self):
        self.devices = dict() # key is device name value is scheduler name
        self.errormsg = ''

class blockdev:
    def __init__(self):
        self.ra = dict() # key is device name value is getra value
        self.errormsg = ''

class rclocal:
    def __init__(self):
        self.isexecutable = False # check that /etc/rc.d/rc.local is executable permissions

    def __str__(self):
        return "executable(%s)" % self.isexecutable

class GpMount:
    def __init__(self):
        self.partition = None
        self.dir= None
        self.type = None
        self.options = dict() # dictionary of options, some options have no value, some have values

    def __str__(self):
        optionstring = ''
        first = True
        for k in self.options.keys():

            if not first:
                optionstring = "%s," % optionstring

            thisoption = k
            if self.options[k]:
                thisoption = "%s=%s" % (thisoption, self.options[k])

            optionstring = "%s%s" % (optionstring, thisoption)
            first = False

        return "%s on %s type %s (%s)" % (self.partition, self.dir, self.type, optionstring)
                
 
class mounts:

    def __init__(self):
        self.entries = dict() # dictionary key=partition value=mount object
        self.errormsg = None

    def __str__(self):
        output = ''
        for k in self.entries.keys():
            output = "%s\n%s" % (output, self.entries[k].__str__())
        return output


       
class DellOutputData:
    def __init__(self):
        self.chkconfig = None
        self.omreport = None
        self.grubconf = None
        self.mounts = None
        self.inittab = None
        self.securetty = None
        self.bcu = None
        self.blockdev = None
        self.rclocal = None
        self.ioschedulers = None
        self.sysctl = None

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

        omr = "=============OMREPORT REMOTEACCESS==========\n"
        ome = "=============OMREPORT REMOTEACCESS ERRORMSG=\n"
        output = "%s\n%s%s\n%s%s" % (output, omr, self.omreport.remoteaccess.__str__(), ome,self.omreport.remoteaccess_errormsg)

        omr = "=============OMREPORT VIRTUAL DISKS=========\n"
        ome = "=============OMREPORT VIRTUAL DISKS ERRORMSG\n"
        output = "%s\n%s%s\n%s%s" % (output, omr, self.omreport.vdisks.__str__(), ome,self.omreport.vdisks_errormsg)

        grc = "============= GRUB.CONF======================\n"
        gre = "============= GRUB.CONF ERRORMSG=============\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.grubconf.__str__(), gre, self.grubconf.errormsg)

        grc = "============= SYSCTL=========================\n"
        gre = "============= SYSCTL ERRORMSG================\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.sysctl.variables.__str__(), gre, self.sysctl.errormsg)

        mnt = "============= MOUNT==========================\n"
        mte = "============= MOUNT ERRORMSG=================\n"
        output = "%s\n%s%s\n%s%s" % (output, mnt, self.mounts.__str__(), mte, self.mounts.errormsg)

        grc = "============= INITTAB========================\n"
        gre = "============= INITTAB ERRORMSG===============\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.inittab.__str__(), gre, self.inittab.errormsg)

        grc = "============= SECURETTY======================\n"
        gre = "============= SECURETTY ERRORMSG=============\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.securetty.data.__str__(), gre, self.securetty.errormsg)

        grc = "============= IO SCHEDULERS==================\n"
        gre = "============= IO SCHEDULERS  ERRORMSG========\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.ioschedulers.devices.__str__(), gre, self.ioschedulers.errormsg)

        grc = "============= BLOCKDEV RA ====================\n"
        gre = "============= BLOCKDEV RA ERRORMSG============\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.blockdev.ra.__str__(), gre, self.blockdev.errormsg)

        grc = "============= BCU CNA ========================\n"
        gre = "============= BCU CNA ERRORMSG================\n"
        output = "%s\n%s%s\n%s%s" % (output, grc, self.bcu.__str__(), gre, self.bcu.errormsg)

        grc = "============= /etc/rc.d/rc.local =============\n"
        output = "%s\n%s%s" % (output, grc, self.rclocal.__str__())

        return output
    
