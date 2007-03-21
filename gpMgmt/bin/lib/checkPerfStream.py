#!/usr/bin/env python
"""
    stream -Perform Stream tests on all the hosts.


"""
import os
from stat import *


class StreamTest:
    """
        StreamTest Class
        Perform Stream Test on the all the hosts
    """
    def __init__(self, gSession, localPath, remotePath, logger):
        """ Initialize Stream Object """
        self.logger=logger
        self.gSession=gSession
        self.strmLPath=localPath
        self.strmRPath=remotePath
        #
        # Initialize Results objects to None
        #
        self.Results=None
        self.resDict=None

    def syncStream(self):
        """ Sync the stream software to remote hosts 
            copy self.lStrmPath to self.strmPath
        """
        logger=self.logger
        # TBD Check if the file has read/execute permissions.
        if not self.strmLPath:
            logger.error("Path of stream is not defined.")
            return False
        try:
            strmStat=os.stat(self.strmLPath)
        except IOError, e :
            logger.error("IO Error %s." % self.strmLPath)
            return False
        except OSError, e:
            logger.error("IO Error %s." % self.strmLPath)
            return False
        if not S_ISREG(strmStat.st_mode):
            logger.error("Invalid File %s." % self.strmLPath)
            return False
        if  not strmStat.st_mode & os.X_OK:
            logger.error("File %s is not executable."% self.strmLPath)
            return False
        print "Syncing Stream binary to all hosts.\n\n"
        cmd='scp'
        resDict={}
        logger.info("Copyting %s to %s on all hosts " % (self.strmLPath, \
            self.strmRPath))
        interrupted=self.gSession.runCommand('scp', [self.strmLPath, \
            self.strmRPath],  quiet=True, retDict=True, resDict=resDict, \
                useHost=True)
        if interrupted:
            logger.error("Copying stream to remote hosts is interrupted.")
            return False
        retVal=True
        for eachHost in resDict.keys():
            if resDict[eachHost]:
                logger.error(resDict[eachHost])
                retVal=False
        if not retVal: return retVal
        chmodCmd="chmod +x %s" % self.strmRPath
        logger.info("Executing %s on all hosts " % chmodCmd)
        resDict={}
        interrupted=self.gSession.runCommand('ssh', [chmodCmd], quiet=True, \
            retDict=True, resDict=resDict, useHost=True)
        if interrupted:
            logger.error("Copying stream to remote hosts is interrupted.")
            return False
        retVal=True
        for eachHost in resDict.keys():
            if resDict[eachHost]:
                logger.error(resDict[eachHost])
                retVal=False
        return retVal

    def execStream(self):
        """ Execute stream command on all hosts ... 
            return a dictionary of results for each host.
        """
        logger=self.logger
        self.resDict={}
        print "\nPerforming stream test on all hosts.\n"
        if not self.syncStream():
            return False
        strmCmd="%s | grep '^Triad' | awk -F' ' '{print $2} '" % \
            self.strmRPath
        logger.info("Executing %s on all hosts " % strmCmd)
        resDict={}
        interrupted=self.gSession.runCommand('ssh', [strmCmd], \
                quiet=True, retDict=True, resDict=resDict, useHost=True)
        if interrupted:
            logger.error("Executing stream on remote hosts is interrupted.")
            return False
        self.resDict=resDict
        return True

    def computeStrmResults(self):
        """
            Compute Stream Results from stream Dictionary
        """
        logger=self.logger

        if not self.resDict:
            logger.error("resDict is empty, Have you performed streamTest")
            return False

        totStrm= avgStrm= cntr=0
        minStrm=maxStrm=minStrmHost=maxStrmHost=None
        for eachHost in  self.resDict.keys():
            eachRes=self.resDict[eachHost]
            trmRes=eachRes.strip()
            try:
                res=float(trmRes)
                self.resDict[eachHost]=res
            except ValueError:
                logger.error("Received %s for %s" % (trmRes, eachHost))
                continue
            if not maxStrm or maxStrm<res:
                maxStrm=res
                maxStrmHost=eachHost
            if not minStrm or minStrm>res:
                minStrm=res
                minStrmHost=eachHost
            totStrm+=res
            cntr+=1
        if not cntr:
            logger.error("None of the hosts values is correct.")
            return False
        if not maxStrm or not minStrm:
            logger.error("Unable to obtain maximum and minimum values")
            return False
        self.Results={}
        self.Results['maxStrm']    =maxStrm
        self.Results['maxStrmHost']=maxStrmHost
        self.Results['minStrm']    =minStrm
        self.Results['minStrmHost']=minStrmHost
        self.Results['totalStrm']  =totStrm
        self.Results['avgStrm']    = totStrm/cntr
        self.Results['numHosts']   =cntr
        return True

    def printStrmSummary(self, tRence=0, detailed=False, hostList=[]):
        logger=self.logger
        if not self.computeStrmResults():
            return
        if not self.Results:
            logger.error("Results is empty, Stream test failed, or "\
                "is not performed.")
            return 
        if detailed:
            print
            print "Stream Test Results for each host"
            print 
            maxStream=self.Results['maxStrm']
            maxAllowed = maxStream - (maxStream * tRence)
            thDict=[]
            for eachHost in hostList:
                if not eachHost in self.resDict.keys():
                    continue
                res=self.resDict[eachHost]
                print "%20s %10.2f MB/S" % (eachHost, res)
                if res<maxAllowed:
                    thDict.append(eachHost)

            if tRence and len(thDict):
                print
                print "\tHosts that are below Tolerance %.2f compared to "\
                    "%.2f MB/S." %  (tRence, maxStream)
                print 
                for x in thDict:
                    print "\t\t %s (%.2f)"% (x, self.resDict[x])
                print 
        print
        print "Stream Tests Summary"
        print 
        print 
        print "\t%10s %10.2f MB/S (%s)" % ('Maximum', \
            self.Results['maxStrm'], self.Results['maxStrmHost'])
        print "\t%10s %10.2f MB/S (%s)" % ('Minimum', \
            self.Results['minStrm'], self.Results['minStrmHost'])
        print "\t%10s %10.2f MB/S " % ('Average', self.Results['avgStrm'])
        print 
        print "\t%10s %10.2f MB/S (on %d hosts)" % ('Total',    \
            self.Results['totalStrm'], self.Results['numHosts'])
        print 
        print
