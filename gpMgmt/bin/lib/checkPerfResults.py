#!/usr/bin/env python
"""
checkPerfResults -- Collect the results of each command.

"""

import os, sys, threading, time, string, getpass, getopt, signal, commands
import traceback

class checkPerfResults:
    def __init__(self, cmdTitle, cmd):
        self.results= None
        self.hosts=None
        self.cmdTitle=cmdTitle
        self.cmd = cmd
        self.hostsTSize=None

    def addHost(self, host, tSize):
        """ Add a host to the list """
        if not self.hosts:
            self.hosts=[]
        if not self.results:
            self.results={}

        if not self.hostsTSize:
            self.hostsTSize={}

        if host not in self.results.keys():
            self.hosts.append(host)
            self.results[host]={}

        self.hostsTSize[host]=tSize


    def addResults(self, host, iteration, real, user, sys):
        """ Add Results to Host """
        if not self.results:
            self.results={}
        if not self.hosts:
            self.hosts=[]

        results = self.results
        if not host in results.keys():
            results[host]={}
            self.hosts.append(host)

        results[host][iteration]={}
        results[host][iteration]['real']=real
        results[host][iteration]['user']=user
        results[host][iteration]['sys']=sys

    def computeItSummary(self):
        """ Compute the maximum and minimum for all the iterations """
        for eachHost in self.results.keys():
            hDict=self.results[eachHost]
            hDict['Average']=None
            hDict['Minimum']=None
            hDict['Maximum']=None
            maxReal=maxUser=maxSys=None
            minReal=minUser=minSys=None
            totUser=totSys=totReal=0
            cntr=0
            for eachIt in hDict.keys():
                if eachIt == 'Average' or eachIt=='Maximum' or eachIt=='Minimum':
                    continue
                itDict=hDict[eachIt]
                real=itDict['real']
                user=itDict['user']
                sys=itDict['sys']
                totUser+=user
                totSys+=sys
                totReal+=real
                # Check The Maximum
                if not maxReal or maxReal<real:
                    maxReal=real
                if not maxUser or maxUser<user:
                    maxUser=user
                if not maxSys or maxSys<sys:
                    maxSys=sys
                # Check The Minimum
                if not minReal or minReal > real:
                    minReal=real
                if not minUser or minUser > user:
                    minUser=user
                if not minSys or minSys > sys:
                    minSys=sys
                cntr+=1
            if not cntr:
                break
            avgUser=totUser/cntr
            avgSys=totSys/cntr
            avgReal=totReal/cntr
            hDict['Average']={}
            hDict['Minimum']={}
            hDict['Maximum']={}
            hDict['Average']['real']=avgReal
            hDict['Average']['user']=avgUser
            hDict['Average']['sys']=avgSys

            hDict['Maximum']['real']=maxReal
            hDict['Maximum']['user']=maxUser
            hDict['Maximum']['sys']=maxSys

            hDict['Minimum']['real']=minReal
            hDict['Minimum']['user']=minUser
            hDict['Minimum']['sys']=minSys

            #print avgReal, avgUser, avgSys
            #print maxReal, maxUser, maxSys
            #print minReal, minUser, minSys

    def computeHostSummary(self):
        """ Compute the averages across all the hosts """
        self.computeItSummary()
        maxReal=minReal=None
        totReal=cntr=0
        for eachHost in self.hosts:
            hDict=self.results[eachHost]
            if 'Average' in hDict.keys():
                hDict=self.results[eachHost]['Average']
            else:
                continue
            if not maxReal or maxReal<hDict['real']:
                maxReal=hDict['real']
                self.maxHost=eachHost
            if not minReal or minReal>hDict['real']:
                minReal=hDict['real']
                self.minHost=eachHost
            totReal+=hDict['real']
            cntr+=1
        if cntr:
            self.realAvg= totReal/cntr
        self.iterations=cntr
        self.realMax= maxReal
        self.realMin= minReal
    
    def printIOResults(self, tHSize, tRence=0, detailed=False):
        """ Print the real times for I/O results """
        try:
            self.computeHostSummary()
        except Exception, e:
            print "*** Caught exception printIOResults  (%s) message: (%s )" \
                % (str(e.__class__), str(e))
            return
        #print "===== %s ======" % self.cmdTitle
        if detailed:
            print 
            print "\t cmd : %s " % self.cmd
            print "%8s HOST Results "  % ("")
            print "%8s %20s %10s %10s %10s " % ("", "Host", 
                "Maximum", "Minimum", "Average")
            print
        factor=1000000
        totalMbps=0
        numHosts=0
        for eachHost in self.hosts:
            hDict=self.results[eachHost]
            if hDict['Average'] and hDict['Maximum'] and hDict['Minimum']:
                pass
            else :
                hDict['avgMbps'] = None
                continue
            try:
                realAvg=hDict['Average']['real']
                realMax=hDict['Maximum']['real']
                realMin=hDict['Minimum']['real']
            except KeyError, e:
                print "Unable to obtain %s value for Host %s" % (str(e), eachHost)
                continue
            avgMbps=maxMbps=minMbps=0
            if not tHSize:
                tSize=self.hostsTSize[eachHost]
            else:
                tSize= tHSize
            if realAvg:
                avgMbps=tSize/(realAvg *factor)
                hDict['avgMbps']=avgMbps
            if realMax:
                maxMbps=tSize/(realMin*factor)
            if realMin:
                minMbps=tSize/(realMax*factor)
            if detailed:
                print "%8s %20s %10.2f %10.2f %10.2f MB/Second" %  \
                    ("", eachHost, maxMbps, minMbps, avgMbps)
            totalMbps+= avgMbps
            numHosts+=1
        print
        avgMbps=maxMbps=minMbps=0

        if self.realMax:
            maxMbps=tSize/(self.realMin*factor)
        if self.realMin:
            minMbps=tSize/(self.realMax*factor)
        if self.realAvg:
            avgMbps=tSize/(self.realAvg*factor)

        print "\t\t Maximum %10.2f MB/Sec(%s)" % (maxMbps, self.minHost)
        print "\t\t Minimum %10.2f MB/Sec(%s)" % (minMbps, self.maxHost)
        print "\t\t Average %10.2f MB/Sec"% avgMbps
        print 
        print "\t\t Total   %10.2f MB/Sec (on %d hosts)"% \
            (totalMbps, len(self.hosts))
        print
        if tRence and detailed:
            print
            thDict=[]
            maxAllowed= maxMbps - (maxMbps * tRence)
            for eachHost in self.hosts:
                hDict=self.results[eachHost]
                if 'avgMbps' in hDict.keys() and  hDict['avgMbps']<maxAllowed:
                    thDict.append(eachHost)
            if len(thDict):
                print "\tHosts that are below Tolerance %.2f compared to %10.2f" % \
                    (tRence, maxMbps)
                print 
                for x in thDict:
                    print "\t\t %s (%.2f)"% (x, self.results[x]['avgMbps'])
                print 
        return
                    
                    

if __name__ == '__main__':
    res=checkPerfResults()
    res.addResults("ddOne", "host1", 1, 1.2, 3.3, 4.4)
    res.addResults("ddOne", "host1", "it2", 1.2, 3.3, 4.4)
    res.addResults("ddOne", "host1", "it3", 1.2, 3.3, 4.40001)
    res.addResults("ddOne", "host1", "it4", 1.2, 3.3, 4.42222)

    res.addResults("ddOne", "host2", "it1", 2.2, 3.3, 4.4)
    res.addResults("ddOne", "host2", "it2", 2.2, 3.3, 4.4)
    res.addResults("ddOne", "host2", "it3", 2.2, 3.3, 4.4)
    res.addResults("ddOne", "host2", "it4", 2.2, 3.3, 4.4)


    res.addResults("ddTwo", "host1", "it1", 1.2, 6.3, 5.4)
    res.addResults("ddTwo", "host1", "it2", 1.2, 3.3, 5.4)
    res.addResults("ddTwo", "host1", "it3", 1.2, 3.3, 5.4)
    res.addResults("ddTwo", "host1", "it4", 1.2, 3.3, 5.4)

    res.addResults("ddTwo", "host2", "it1", 2.2, 3.3, 5.4)
    res.addResults("ddTwo", "host2", "it2", 2.2, 3.3, 5.4)
    res.addResults("ddTwo", "host2", "it3", 2.2, 3.3, 5.4)
    res.addResults("ddTwo", "host2", "it3", 2.2, 3.3, 5.4)

    res.addResults("ddThree", "host2", "it1", 2.2, 3.3, 5.4)
    res.addResults("ddThree", "host2", "it2", 2.2, 3.3, 5.4)
    res.addResults("ddFour", "host2", "it3", 2.2, 3.3, 5.4)
    res.addResults("ddFour", "host2", "it3", 2.2, 3.3, 5.4)


    res.printResults()
    res.printResults()
    res.printSumResults()


