#!/usr/bin/env python 
"""
    USAGE: gpcheckos  [-s SRC_HOST]
    where 
    SRC_HOST = is used to check ntp, most cases it's the master host and defaults to mdw 
"""

import os, sys, getopt, re, string, shutil,platform
from xml.dom import minidom
from gppylib.gpcheckosXml import *
from gppylib.gpcheckosVo  import *
from gppylib.utils import *
from gppylib.stringUtil import *
from gppylib.gplog import *
from gppylib.commands.gpcheckosUnix import *
from socket import gethostname

def printWarning(str):
    print "--------"
    print " WARNING %s"%(str)
    print "--------"

progname = os.path.split(sys.argv[0])[-1]

from optparse import Option, OptionGroup, OptionParser
try:
    import os, sys
    from optparse import Option, OptionParser
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib.gparray import *
    from gppylib.gphostcache import *
    from gppylib.gplog import *
    from gppylib.commands.unix import *
    from gppylib.commands.gp import *
    from gppylib.db import dbconn
    from gppylib.userinput import *
    from pygresql.pgdb import DatabaseError

except ImportError, e: 
    sys.exit('Cannot import modules, Detail: %s'%(str(e)))
    
    

class GpCheckOs001(Command):

    def __init__(self,  srcHost=None):
        gpXml = GpXml()
        self.osvo = gpXml.findNode("osParm")
        self.srcHost = srcHost
        self.baselineMemory=None
        self.cpuCoreCount=None
        self.cpuMHz=None
        self.ra = []
        self.baseline = Baseline()
        logger=get_default_logger()
        global baselineFile 
        self.baseline.serializeFileToObject(baselineFile)
        
        logger.info("\n")
        logger.info('****************')
        logger.info('checking %s'%(gethostname()))
        logger.info('****************')
    def checkSysctl(self,vos):
        fd=None
        fp=None
        if (getOs() == "CentOS" or getOs() == "SuSe"): 
            fp="/etc/sysctl.conf" 
            print "\nChecking %s.........."%(fp)
            for vo in vos:
                if os.path.isfile(fp):  
                    try:
                        fd=open(fp)
                        for line in fd:
                            if re.search(r"^%s"%(vo.param), line): 
                                lineVal= line.split("=")[1]
                                vl=vo.value.split()
                                for index,item in enumerate(lineVal.split()):
                                    if long(item) < long(vl[index]):
                                         printWarning("parameter %s %s is smaller than suggested value %s"%(vo.param, item,vl[index] ))
                    finally: 
                        fd.close()

    def checkNtpdate(self):                   
        print "\nChecking Ntp ...... "
        #"stop ntpd for testing
        # do not run ntp check on the source host, just 
        # make sure the service is up
        ntpD = None
        if (getOs() == "CentOS"): 
     	    ntpD="ntpd"
        elif (getOs() == "SuSE"):
     	    ntpD="ntp"
        if getLocalHostname() == self.srcHost:
            cmd = ServiceDaemon("service daemon",ntpD,"start")
            cmd.run()
            if (cmd.get_results().rc != 0):
                print cmd.get_results().stderr
            else: 
                print cmd.get_results().stdout
        # run ntp check on the specified host
        else:
            cmd = ServiceDaemon("service daemon",ntpD,"stop")
            cmd.run()
            print "\nChecking ntpdate using server %s"%(self.srcHost)
            cmd = Ntpdate("ntpdate",self.srcHost)
            cmd.run() 
            if (cmd.get_results().rc != 0):
                print cmd.get_results().stderr
            else: 
                print cmd.get_results().stdout

            #start ntpd after testing
            print "finished ntpdate testing restarting ntpd "
            cmd = ServiceDaemon("service daemon",ntpD,"start")
            cmd.run()
            if (cmd.get_results().rc != 0):
                printWarning ( "%s NOTE: this test fails because ntp server requires time to synchronize." \
                        %(cmd.get_results().stderr))
            else: 
                print cmd.get_results().stdout

    def print_result(self,cmd):
        if (cmd.get_results().rc != 0):
            return cmd.get_results().stderr
        else: 
            return cmd.get_results().stdout.rstrip("\n")
   
    def checkMemory(self):
        print "Checking Memory Configuration ...... "
        memTotal= self.runCmd(nixCheckMemoryTotal)
        print "Physical Memory  is %sk" %(memTotal)
        if long(memTotal) != long(self.baseline.memoryTotal) :
            printWarning("memory %s is not equal to the baseline memory %s"%(memTotal,self.baseline.memoryTotal))
    
    def checkIoSched(self):
        print "\nChecking IO scheduler in grub file......." 
        cmd = anyCommand("any Command", nixCheckIoSchedRH)
        cmd.run()
        result=self.print_result(cmd)
        if result:
            printWarning("IO scheduler not set in grub file, please put elevator=deadline in /boot/grub/menu.list")
        else:
            print "OK"
        print "\nChecking IO scheduler in sysfs......."
        cmd = anyCommand("any Command", nixGetIoScheduler)
        cmd.run()
        sds=self.print_result(cmd).split("\n")
        for sd in sds:
            s,i =sd.split("|")
            p = re.compile('(\[)(\w+)(\])')
            m=p.search(i)
            if m.group(2) != "deadline":
                printWarning("current io scheduler for %s is %s, please set it to deadline"%(s,m.group(2)))
        
    def runCmd(self,cmdStr):
        cmd=anyCommand("any Command",cmdStr)    
        cmd.run()
        return self.print_result(cmd)

    def runCmdStatus(self,cmdStr):
        cmd=anyCommand("any Command",cmdStr)    
        cmd.run()
        return cmd.get_results().rc

    def getNetworkConfig(self):
        print "\nChecking Active Network configuration...... "
        cmd = anyCommand("any Command", nixGetEthCmd)
        cmd.run()
        eth=self.print_result(cmd).rstrip('\n')
        eths =eth.rstrip(':').split(':')
        for eth in eths:
            nixGetMacCmd="ifconfig %s |awk '/HWaddr/ {print $5}'"%(eth)
            nixGetIpCmd="ifconfig %s |awk '/inet addr/ {print $2}'|awk -F: '{print $2}'"%(eth)
            nixGetNetmaskCmd="ifconfig %s |awk '/inet addr/ {print $4}'|awk -F: '{print $2}'"%(eth)
            nixGetSpeedCmd="ethtool %s |awk '/Speed/ {print $2}'"%(eth)

            mac = self.runCmd(nixGetMacCmd)
            ip = self.runCmd(nixGetIpCmd)
            netmask = self.runCmd(nixGetNetmaskCmd)
            speed = self.runCmd(nixGetSpeedCmd)

            print "interface=%s MAC=%s IP_ADDRESS=%s NETMASK=%s SPEED=%s"%(eth,mac,ip,netmask,speed)
            if long(speed.split("Mb/s")[0]) < 1000 :
                printWarning( "speed is less then 1000 Mb/s")         
   
    def checkFirewall(self):        
        print "\nChecking firewall settings.........."
        if (getOs() == "CentOS"): 
            if self.runCmdStatus(nixCheckFirewall4RH) == 0 :
                printWarning ("ipv4 firewall is on, please turn off with /etc/init.d/iptables off then run chkconfig iptables off")
            else: 
                print("ipv4 firewall [OK]")
            if self.runCmdStatus(nixCheckFirewall6RH) == 0 :
                printWarning( "ipv6 firewall is on, please turn off with /etc/init.d/iptables off then run chkconfig iptables off")
            else: 
                print("ipv6 firewall [OK]")
        elif(getOs() == "SuSE"):
            if self.runCmdStatus(nixCheckFirewallSuSEInit) == 0:
                printWarning ("SuSE  firewall Init is on, please turn off with /etc/init.d/SuSEFirewall2_init off then run chkconfig SuSEFirewall2_init off")
            else:
                print("SusE firewall init disabled [OK]")
            if self.runCmdStatus(nixCheckFirewallSuSESetup) == 0:
                printWarning( "SuSE firewall setup is on, please turn off with /etc/init.d/SuSEFirewall2_setup off then run chkconfig SuSEFirewall2_setup off")
            else:
                print("SuSE firewall setup disabled [OK]")

    def checkCpuSpeed(self): 
        print "\nChecking cpu speed daemons .........."
        if (getOs() == "CentOS"): 
            cpuSpeed= self.runCmd(nixCheckCpuSpeedRH) 
            if re.search('on', cpuSpeed):
                printWarning ("cpu speed daemon is on, please turn off with /etc/init.d/cpuspeed off then run chkconfig cpuspeed off")
            else:
                print("cpuspeed disabled [OK]")
        elif (getOs() == "SuSE"):
            cpuSpeed = self.runCmd(nixCheckCpuSpeedSuSE)
            if re.search('on', cpuSpeed):
                printWarning("cpu speed daemon is on, please turn off with /etc/init.d/powersaved off then run chkconfig powersaved off")
            else:
                print("SuSE powersaved disabled [OK]")
         
                             
        MHzs=self.runCmd(nixGetCpuMHz).rstrip('\n').split('\n')
        for index, MHz in enumerate(MHzs):
            
            if MHz != self.baseline.cpuMHz[index]:
                printWarning ("cpuspeed %s is different than the baseline %s on core %s"% \
                              (MHz, self.baseline.cpuMHz[index], index))
            else:
                print "cpu%s speed is %s [OK]"%(index, MHz)
            
        print "\nChecking number of cpu cores ....."
        self.cpuCoreCount=self.runCmd(nixGetCpuCoreCount)     
        if int(self.cpuCoreCount) != int(self.baseline.coreCount):
            printWarning("number of cpu cores %s is different than the baseline cpu cores %s"% \
                            (self.cpuCoreCount, self.baseline.coreCount))
        else:
            print "Number of cpu cores %s [OK]" %(self.cpuCoreCount)
              
    def checkSwap(self):   
        print "\nChecking Swap settings.........."
        if (getOs() == "CentOS"): 
            swap = self.runCmd(nixCheckSwapTotal)
            if long(swap) < long(self.baseline.memoryTotal):
                print "swap %sK is less then %s"%(swap,self.baseline.memoryTotal )
            else:
                print "swap : %sK [OK]"%(swap)

    def checkBlockdev(self):   
        print "\nChecking blockdev settings.........."
        
        if (getOs() == "CentOS"): 
            setra = self.runCmd(nixCheckBlockdevRH)
            if not setra:
                printWarning ("device read ahead not found in /etc/rc.local.  please add /sbin/blockdev --setra 16384 /dev/<device_name> to /etc/rc.local")
            else:
                print "readahead settings rc.local is OK"
	    

        elif (getOs() == "SuSE"): 
            setra = self.runCmd(nixCheckBlockdevSuSE)
            if not setra:
                printWarning ("device read ahead not found in /etc/after.local.  please add /sbin/blockdev --setra 16384 /dev/<devices_name> to /etc/after.local")
            else:
                print "readahead setting in after.local is OK"
	
        print "\nChecking read ahead in sysfs......."
	if platform.system() == 'Linux':
            cmd = anyCommand("any Command", nixGetBlockdev)
            cmd.run()
            sds=self.print_result(cmd).split("\n")
            for sd in sds:
                s,i =sd.split("|")
                #p = re.compile('(\[)(\d+)(\])')
                #m=p.search(i)
                if long(i) != 8192:
                    printWarning("current readahead for %s is %sK, please set it to 8192K or 16384 blocks with blockdev command" \
		                 %(s,i))
		else:
		    print("current readahead for %s is %sK...[OK]"%(s,i))
 
    def checkLimits(self,vos):
        fd=None
        fp=None
        if (getOs() == "CentOS" or getOs()== "SuSE"): 
            fp="/etc/security/limits.conf" 
            print "\nChecking %s.........."%(fp)
            for vo in vos:
                softValueExist = False
                hardValueExist = False
                if os.path.isfile(fp):  
                    try:
                        vs=string.split(vo.softValue)
                        vh=string.split(vo.hardValue)
                        fd=open(fp)
                        for line in fd:
                            if not re.match('#.*$', line) and line.rstrip('\n') != "":
                                lines=line.split()
                                if lines[1] == vs[1] and lines[2] == vs[2]:
                                #if re.search('%s\s.*%s'%(vs[1],vs[2]), line.rstrip('\n')):
                                #    lines=line.split()
                                    if long(lines[3]) < long(vs[3]):
                                        printWarning( "system param %s %s value %s is less then recommended param %s value %s"% \
                                        (lines[1],lines[2],lines[3],vs[2],vs[3]))
                                    softValueExist=True
                                if lines[1] == vh[1] and lines[2] == vh[2]:
                                #if re.search('%s\s.*%s'%(vh[1],vh[2]), line.rstrip('\n')):
                                    if long(lines[3]) < long(vh[3]):
                                        printWarning("system param %s %s value %s is less then recommended param %s value %s"% \
                                        (lines[1],lines[2],lines[3],vh[2],vh[3]))    
                                    hardValueExist=True

                    finally: 
                        fd.close()
        
                if not softValueExist :             
                    printWarning("!WARNING! type %s for item %s is not in %s"%(vs[1],vs[2], fp))                    
                if not hardValueExist :             
                    printWarning("!WARNING! Type \"%s\" for item \"%s\" is not in %s"%(vh[1],vh[2], fp))                    
def parseargs():
    parser = OptParser(option_class=OptChecker, usage=__doc__)
    parser.remove_option('-h')
   
    parser.add_option('-h', '-?', '--help', action='store_true')
    
    parser.add_option('-s', '--src_host', type="string", dest="srcHost" ,
                        help='specify a source host for ntp checks')
    parser.add_option('--debug', action='store_true')

    (options,args) = parser.parse_args()

#sanity check
    if (not options.srcHost ):
        options.srcHost="mdw" 
  
    if options.help:
        print __doc__
        sys.exit(0) 

    return options, args 

def main():

    options,args = parseargs()
    if options.debug:
        enable_verbose_logging()
    
    
    s=GpCheckOs001(options.srcHost)
    s.checkSysctl(s.osvo.sysctlConfList)
    s.checkLimits(s.osvo.limitsConfList)
    s.checkIoSched()
    s.checkNtpdate()
    s.checkMemory()
    s.getNetworkConfig()
    s.checkFirewall()
    s.checkSwap()
    s.checkCpuSpeed() 
    s.checkBlockdev()

if __name__ == "__main__":
        main()
