#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
#
# Internal Use Function.
#
#
# THIS IMPORT MUST COME FIRST
#
# import mainUtils FIRST to get python version check
from gppylib.mainUtils import *
import os, sys

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

import pickle, base64

from gppylib.gpparseopts import OptParser, OptChecker
from gppylib import gparray, gplog
from gppylib.commands import base, gp, pg, unix
from gppylib.commands.gp import SEGMENT_TIMEOUT_DEFAULT
from gppylib.commands.gp import *
from gppylib.db import catalog, dbconn
from gppylib import pgconf
from gppylib.gpcoverage import GpCoverage
from gppylib.utils import parseKeyColonValueLines


description = (""" 
This utility is NOT SUPPORTED and is for internal-use only.

Starts a set of one or more segment databases.
""")

_help = ["""
Utility should only be used by other GP utilities.  

Return codes:
  0 - All segments started successfully
  1 - At least one segment didn't start successfully
  2 - Fatal error to script during startup.  unknown state.

"""]

logger = gplog.get_default_logger()

#-------------------------------------------------------------------------
class SegStartStatus:

    def __init__(self,datadir,started,reason, reasonCode):
        """
        @param reasonCode one of the gp.SEGSTART_* values
        """
        self.datadir=datadir
        self.started=started
        self.reason=reason
        self.reasonCode = reasonCode
    
    def __str__(self):
        return "STATUS--DIR:%s--STARTED:%s--REASONCODE:%s--REASON:%s" % \
                    (self.datadir,self.started,self.reasonCode,self.reason)

#-------------------------------------------------------------------------    
class GpSegStart:
    ######
    def __init__(self, dblist, gpversion, collation, mirroringMode, numContentsInCluster, timeout, pickledTransitionData, specialMode, wrapper, wrapper_args):

        self.dblist = map(gparray.GpDB.initFromString, dblist)
        self.mirroringMode = mirroringMode
        self.expected_gpversion=gpversion
        self.numContentsInCluster=numContentsInCluster
        self.timeout = timeout
        self.pickledTransitionData = pickledTransitionData

        assert(specialMode in [None, 'upgrade', 'maintenance'])
        self.specialMode = specialMode

        #
        # There was an issue with Linux that required the numWorkers to be 1,
        # but that problem doesn't seem to exist any more. In order to
        # assure proper startup of primary and mirror, we must start all
        # segments on a node at the same time.
        #
        self.pool = base.WorkerPool(numWorkers=len(dblist))
        self.expected_lc_collate=None
        self.expected_lc_monetary=None
        self.expected_lc_numeric=None
        self.wrapper=wrapper
        self.wrapper_args=wrapper_args
        self.gphome=os.path.abspath(os.pardir)
        
        self.actual_gpversion=gp.GpVersion.local('local GP software version check',self.gphome)
        if self.actual_gpversion != self.expected_gpversion:
            raise Exception("Local Software Version does not match what is expected.\n"
                            "The local software version is: '%s'\n"
                            "But we were expecting it to be: '%s'\n"
                            "Please review and correct" % (self.actual_gpversion,self.expected_gpversion))
                            
        
        collation_strings=collation.split(':')
        if len(collation_strings) != 3:
            raise Exception("Invalid collation string specified!")
        (self.expected_lc_collate,self.expected_lc_monetary,self.expected_lc_numeric)=collation_strings
        pass

    def __markFailed(self, directoryToSegStartDataMap, results, dir, msg, reasonCode ):
        """
        Mark a segment as failed during some startup process and remove
          the entry for the segment from directoryToSegStartDataMap

        @param reasonCode one of the gp.SEGSTART_* constant values
        """

        results.append(SegStartStatus(dir, False, msg, reasonCode))
        del directoryToSegStartDataMap[dir]


    def __validateDirectoriesAndSetupRecoveryStartup(self, directoryToSegStartDataMap, results):
        """
        validate that the directories all exist and run recovery startup if needed

        @param directoryToSegStartDataMap the directories to start.  Any failed ones will be removed from this map
        @param results any failures will have segment statuses appended here
        """
        for dir in directoryToSegStartDataMap.keys():
            if os.path.isdir(dir):
                if not os.path.exists(dir + '/pg_log'):
                    os.mkdir(dir + '/pg_log')

                if os.path.exists(dir + '/postmaster.pid'):
                    logger.warning("postmaster.pid file exists, checking if recovery startup required")
                    res = gp.recovery_startup(dir)
                    if res:
                        self.__markFailed(directoryToSegStartDataMap, results, dir, res, gp.SEGSTART_ERROR_STOP_RUNNING_SEGMENT_FAILED)
            else:
                logger.info("Segment data directory does not exist for: '%s'" % dir)
                self.__markFailed(directoryToSegStartDataMap, results, dir, "segment data directory does not exist",
                                    gp.SEGSTART_ERROR_DATA_DIRECTORY_DOES_NOT_EXIST)

    def __startSegments(self, directoryToSegStartDataMap, results):
        """
        Start the segments themselves 

        @param directoryToSegStartDataMap the directories to start.  Any failed ones will be removed from this map
        @param results any failures will have segment statuses appended here
        """

        # this is the mirroring mode for the pre-conversion part of running
        mirroringModeForStart = self.mirroringMode

        if mirroringModeForStart == 'quiescent':
            nowait = True
        else:
            nowait = False

        logger.info("Starting segments...")
        for dir, seg in directoryToSegStartDataMap.iteritems():
            cmd=gp.SegmentStart("Starting seg at dir %s" % dir, seg,
                                self.numContentsInCluster,
                                mirroringModeForStart,
                                noWait=nowait,
                                timeout=self.timeout,
                                specialMode=self.specialMode,
                                wrapper=self.wrapper,
                                wrapper_args=self.wrapper_args)
            self.pool.addCommand(cmd)

        self.pool.join()

        for cmd in self.pool.getCompletedItems():
            res=cmd.get_results()
            if res.rc != 0:
                #TODO: read in last entries in startup.log
                self.__markFailed(directoryToSegStartDataMap, results, cmd.segment.getSegmentDataDirectory(),
                                  "PG_CTL failed.\nstdout:%s\nstderr:%s\n" % (res.stdout,res.stderr),
                                   gp.SEGSTART_ERROR_PG_CTL_FAILED)

        self.pool.empty_completed_items()

    def __checkLocaleAndConnect(self, directoryToSegStartDataMap, results):

        if self.mirroringMode == 'quiescent':
            logger.info("Awaiting segment startup and validating segments...")
        else:
            logger.info("Validating segments...")

        #
        # we CANNOT validate using a psql connection because this may hang (see MPP-9974).
        #    so we validate these items using a postmaster 'transition' message
        #
        dataDirToCmd = {}
        for datadir, seg in directoryToSegStartDataMap.iteritems():
            if seg.isSegmentPrimary(True):
                cmd = gp.SendFilerepTransitionStatusMessage("Check Status", "getCollationAndDataDirSettings",
                                        seg.getSegmentDataDirectory(), seg.getSegmentPort())
                dataDirToCmd[datadir] = cmd
                self.pool.addCommand(cmd)

        self.pool.join()

        for datadir, cmd in dataDirToCmd.iteritems():
            cmd.get_results()
            line = cmd.unpackSuccessLine()
            if line is None:
                self.__markFailed(directoryToSegStartDataMap, results, datadir, "Unable to connect to server",
                            gp.SEGSTART_ERROR_CHECKING_CONNECTION_AND_LOCALE_FAILED)
                continue
            dict = parseKeyColonValueLines(line)

            # verify was parsed, and we got all needed data
            if dict is None or \
                [s for s in ["datadir", "lc_collate", "lc_monetary", "lc_numeric"] if s not in dict]:
                self.__markFailed(directoryToSegStartDataMap, results, datadir, "Invalid response from server",
                            gp.SEGSTART_ERROR_CHECKING_CONNECTION_AND_LOCALE_FAILED)
                continue

            errorStr = ""
            if dict["lc_collate"] != self.expected_lc_collate:
                errorStr += "Segment's value of lc_collate does not match the master.\n" \
                                " Master had value: '%s' while this segment has: '%s'\n" %\
                                (self.expected_lc_collate,dict["lc_collate"])
            if dict["lc_monetary"] != self.expected_lc_monetary:
                errorStr += "Segment's value of lc_monetary does not match the master.\n"\
                                  " Master had value: '%s' while this segment has: '%s'\n" %\
                                (self.expected_lc_monetary,dict["lc_monetary"])
            if dict["lc_numeric"] != self.expected_lc_numeric:
                errorStr += "Segment's value of lc_numeric does not match the master.\n" \
                                  " Master had value: '%s' while this segment has: '%s'\n" %\
                                  (self.expected_lc_numeric,dict["lc_numeric"])

            if not os.path.samefile(dict["datadir"], datadir):
                errorStr += "Segment's data directory does not match. " \
                              "Expected value %s, Actual value %s\n" % \
                              (datadir, dict["datadir"])


            if len(errorStr) > 0:
                self.__markFailed(directoryToSegStartDataMap, results, datadir, errorStr,
                            gp.SEGSTART_ERROR_CHECKING_CONNECTION_AND_LOCALE_FAILED)

    def __convertSegments(self, directoryToSegStartDataMap, results, transitionData):

        if self.mirroringMode == 'quiescent':

            logger.info("Changing segments...")
            for dir, seg in directoryToSegStartDataMap.iteritems():
                #
                # This cmd will deliver a message to the postmaster using gp_primarymirror
                #
                # (look for the protocol message type PRIMARY_MIRROR_TRANSITION_REQUEST_CODE )
                #
                cmd = gp.SendFilerepTransitionMessage.buildTransitionMessageCommand(transitionData,
                                                                                    seg.getSegmentDataDirectory(),
                                                                                    seg.getSegmentPort())
                self.pool.addCommand(cmd)
            self.pool.join()

            dataDirToSeg = gparray.GpArray.getSegmentsGroupedByValue(\
                                directoryToSegStartDataMap.values(), gparray.GpDB.getSegmentDataDirectory)

            toStop = []
            cmds=self.pool.getCompletedItems()
            for cmd in cmds:
                res=cmd.get_results()
                if res.rc != 0:
                    stdoutFromFailure = res.stdout.replace("\n", " ").strip()
                    stderrFromFailure = res.stderr.replace("\n", " ").strip()

                    shouldStop = False

                    if res.rc == gp.SendFilerepTransitionMessage.TRANSITION_ERRCODE_ERROR_SERVER_DID_NOT_RETURN_DATA:
                        reason = "Segment did not respond to startup request; check segment logfile"
                        reasonCode = gp.SEGSTART_ERROR_SERVER_DID_NOT_RESPOND

                        #
                        # server crashed when sending response, should ensure it's stopped completely!
                        #
                        shouldStop = True

                    elif stderrFromFailure.endswith("failure: Error: MirroringFailure"):
                        reason = "Failure in segment mirroring; check segment logfile"
                        reasonCode = gp.SEGSTART_ERROR_MIRRORING_FAILURE

                    elif stderrFromFailure.endswith("failure: Error: PostmasterDied"):
                        reason = "Segment postmaster has exited; check segment logfile"
                        reasonCode = gp.SEGSTART_ERROR_POSTMASTER_DIED

                    elif stderrFromFailure.endswith("failure: Error: InvalidStateTransition"):
                        reason = "Not a valid operation at this time; check segment logfile"
                        reasonCode = gp.SEGSTART_ERROR_INVALID_STATE_TRANSITION

                        #
                        # This should never happen, but if it does then we will ensure process is gone
                        #
                        shouldStop = True

                    elif stderrFromFailure.endswith("failure: Error: ServerIsInShutdown"):
                        reason = "System is shutting down"
                        reasonCode = gp.SEGSTART_ERROR_SERVER_IS_IN_SHUTDOWN

                    else:

                        if res.rc == gp.SendFilerepTransitionMessage.TRANSITION_ERRCODE_ERROR_SOCKET:
                            #
                            # Couldn't connect to server to do transition or got another problem
                            #     communicating, must make sure it's halted!
                            #
                            shouldStop = True

                        reason = "Start failed; check segment logfile.  \"%s%s\"" % (stdoutFromFailure, stderrFromFailure)
                        reasonCode = gp.SEGSTART_ERROR_OTHER

                    self.__markFailed(directoryToSegStartDataMap, results, cmd.dataDir, reason, reasonCode)

                    if shouldStop:
                        assert len(dataDirToSeg[cmd.dataDir]) == 1, "Multiple segments with dir %s" % cmd.dataDir
                        toStop.append(dataDirToSeg[cmd.dataDir][0])

            #
            # 
            #
            for seg in toStop:
                logger.info("Stopping segment %s, %s because of failure sending transition" % \
                                (seg.getSegmentDataDirectory(), seg.getSegmentPort()))
                cmd=gp.SegmentStop('stop segment', seg.getSegmentDataDirectory(), mode="immediate")
                cmd.run(validateAfter=False)
                res = cmd.get_results()
                if res.rc == 0:
                    logger.info("Stop of segment succeeded")
                else:
                    stdoutFromFailure = res.stdout.replace("\n", " ").strip()
                    stderrFromFailure = res.stderr.replace("\n", " ").strip()
                    logger.info("Stop of segment failed: rc: %s\nstdout:%s\nstderr:%s" % \
                                    (res.rc, stdoutFromFailure, stderrFromFailure))
            

    ######
    def run(self):
        results=[]
        failures=False
        directoryToSegStartDataMap = {}
        if self.pickledTransitionData is None:
            transitionData = None
        else:
            transitionData = pickle.loads(base64.urlsafe_b64decode(self.pickledTransitionData))

        for seg in self.dblist:
            directoryToSegStartDataMap[seg.getSegmentDataDirectory()] = seg
        numToStart = len(directoryToSegStartDataMap)

        self.__validateDirectoriesAndSetupRecoveryStartup(directoryToSegStartDataMap, results)
        self.__startSegments(directoryToSegStartDataMap, results)
        self.__convertSegments(directoryToSegStartDataMap, results, transitionData)
        self.__checkLocaleAndConnect(directoryToSegStartDataMap, results)

        # any remaining items succeeded!
        for datadir,segmentToStart in directoryToSegStartDataMap.iteritems():
            results.append(SegStartStatus(datadir,True,"Start Succeeded", gp.SEGSTART_SUCCESS ))

        #Log the results!
        status = '\nCOMMAND RESULTS\n'
        for result in results:
            status += str(result) + "\n"
        logger.info(status)

        # return exit code depending on if there were failures
        return 1 if len(directoryToSegStartDataMap) != numToStart else 0
    
    def cleanup(self):
        if self.pool:
            self.pool.haltWork()
    
    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():
        parser = OptParser(option_class=OptChecker,
                    description=' '.join(description.split()),
                     version='%prog version $Revision: #12 $')
        parser.setHelp(_help)

    #
    # Note that this mirroringmode parameter should only be either mirrorless or quiescent.
    #   If quiescent then it is implied that there is pickled transition data that will be
    #   provided (using -p) to immediately convert to a primary or a mirror.
    #

        addStandardLoggingAndHelpOptions(parser, includeNonInteractiveOption=False)

        parser.add_option("-C", "--collation", type="string",
                            help="values for lc_collate, lc_monetary, lc_numeric separated by :")
        parser.add_option("-D","--dblist",dest="dblist", action="append", type="string")
        parser.add_option("-M","--mirroringmode",dest="mirroringMode", type="string")
        parser.add_option("-p","--pickledTransitionData",dest="pickledTransitionData", type="string")
        parser.add_option("-V", "--gp-version", dest="gpversion",metavar="GP_VERSION",
                            help="expected software version")
        parser.add_option("-n", "--numsegments", dest="numsegments",
                          help="number of distinct content ids in cluster")
        parser.add_option("-t", "--timeout", dest="timeout", type="int", default=SEGMENT_TIMEOUT_DEFAULT,
                          help="seconds to wait")
        parser.add_option('-U', '--specialMode', type='choice', choices=['upgrade', 'maintenance'],
                           metavar='upgrade|maintenance', action='store', default=None,
                           help='start the instance in upgrade or maintenance mode')
        parser.add_option('', '--wrapper', dest="wrapper", default=None, type='string');
        parser.add_option('', '--wrapper-args', dest="wrapper_args", default=None, type='string');
        
        return parser

    @staticmethod
    def createProgram(options, args):
        return GpSegStart(options.dblist,
                                    options.gpversion,
                                    options.collation,
                                    options.mirroringMode,
                                    options.numsegments,
                                    options.timeout,
                                    options.pickledTransitionData,
                                    options.specialMode,
                                    options.wrapper,
                                    options.wrapper_args)

#------------------------------------------------------------------------- 
if __name__ == '__main__':
    mainOptions = { 'setNonuserOnToolLogger':True}
    simple_main( GpSegStart.createParser, GpSegStart.createProgram, mainOptions)
