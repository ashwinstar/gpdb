#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved.
#
"""
Set of Classes for executing unix commands.


"""
import os
import platform
import socket
import sys
import signal
from gppylib.stringUtil import *
from gppylib.gplog import *
from gppylib.commands.base import *
from gppylib.commands.unix import *

logger = gplog.get_default_logger()
nixGetEthCmd="ifconfig |grep eth |awk '{printf( \"%s:\",$1 ) }'"
nixCheckFirewall4RH="chkconfig --list |awk '/iptables/ {print $5\";\"$7}'"
nixCheckFirewall6RH="chkconfig --list |awk '/ip6tables/ {print $5\";\"$7}'"
nixCheckCpuSpeedRH="chkconfig --list |awk '/cpuspeed/ {print $5\";\"$7}'"
nixCheckCpuSpeedSuSE="chkconfig --list |awk '/powersaved/ {print $5\";\"$7}'"
nixCheckIoSchedRH="grep deadline /boot/grub/grub.conf"
nixCheckMemoryTotal="awk '/MemTotal/ {print $2}'  /proc/meminfo"
nixCheckSwapTotal="free |awk '/Swap/ {print $2}'"
nixCheckBlockdevRH="grep blockdev /etc/rc.local | cut -d' ' -f3"
nixCheckBlockdevSuSE="grep blockdev /etc/after.local |awk '{print $3}'"
nixGetCpuCoreCount="grep processor /proc/cpuinfo |wc -l"
nixGetCpuMHz="grep MHz /proc/cpuinfo | cut -d: -f2"
nixGetXfsMounts="grep xfs /proc/mounts |cut -d' ' -f2"
nixGetIoScheduler="/opt/gpcheckos/bin/checkIoScheduler"
baselineFile="/tmp/.BASELINE"


#---------------ntpdate--------------------
class Ntpdate(Command):
    def __init__(self,name,hostToNtpdate,ctxt=LOCAL,remoteHost=None,obj=None):
        self.hostToNtpdate=hostToNtpdate
        self.obj=obj
        cmdStr = "%s  %s" % (findCmdInPath('ntpdate'),hostToNtpdate)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

#---------------linux service--------------------
class ServiceDaemon(Command):
    def __init__(self,name,serviceName,serviceMode,ctxt=LOCAL,remoteHost=None,obj=None):
        self.serviceName=serviceName
        self.serviceMode=serviceMode
        self.obj=obj
        cmdStr = "/etc/init.d/%s %s"%(self.serviceName,self.serviceMode)
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)
#---------------generic command--------------------
class anyCommand(Command):
    def __init__(self,name,cmdStr,ctxt=LOCAL,remoteHost=None,obj=None):
        self.obj=obj
        #cmdStr = "%s  %s" % (findCmdInPath(cmdStr))
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)


#--------------- non-ref platform global variables --------------------
class Baseline(object):
    def __init__(self):
        self.memoryTotal=None
        self.dataMounts=[]
        self.cpuMHz=[]
        self.networkInterface={}
        self.coreCount=None
    def serializeToFile(self,ds,baselineFile):
        mountStr =None
        cpuMHzStr = None
        if ds == "memoryTotal":
            appendString("memoryTotal=%s"%(self.memoryTotal), baselineFile)       
        elif ds == "dataMounts":
            for mount, dataMount in enumerate(self.dataMounts):
                if None == mountStr :
                    mountStr="%s"%(dataMount)
                else:
                    mountStr="%s|%s"%(mountStr,dataMount)
            appendString("mounts=%s"%(mountStr), baselineFile)
            
        elif ds == "cpuMHz":
            for cpu, mhz in enumerate(self.cpuMHz):
                if None == cpuMHzStr:
                    cpuMHzStr = "%s"%(mhz)
                else:
                    cpuMHzStr="%s|%s"%(cpuMHzStr,mhz)
                    appendString("cpuMHz=%s"%(cpuMHzStr), baselineFile)
            
        elif ds == "coreCount":
            appendString("coreCount=%s"%(self.coreCount), baselineFile)
        
            
    def printBaseline(self):
        print "Baseline memory : "
        print self.memoryTotal
        print "Baseline data mounts :"
        for mount, dataMount in enumerate(self.dataMounts):
            print "%s"%(dataMount)
        print "Baseline cpu speed"
        for cpu, mhz in enumerate(self.cpuMHz):
            print "cpu%s %s"%(cpu,mhz)
        print "Baseline number of cores:"
        print "%s"%(self.coreCount)
    
    def serializeFileToObject(self,baselineFile):
        try:
            fd = open(baselineFile)
            for line in fd:
                if re.match("memoryTotal",line):
                    self.memoryTotal = line.split("=")[1]
                elif re.match("mounts",line): 
                    self.dataMounts = line.split("=")[1].rstrip('\n').split("|")
                elif re.match("cpuMHz",line):
                    self.cpuMHz = line.split("=")[1].rstrip('\n').split("|")
                elif re.match("coreCount",line):
                    self.coreCount=line.split("=")[1]
                   
        except IOError:
            pass
        finally:
            fd.close()
        