#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
#
# todo MIRRORING move over checks for newRecoverHost (chk_pool_host) from old code
# todo MIRRORING I may have changed the semantics for 'spareDataDirectory'.  review old semantics
#
# todo MIRRORING check that segment is actually in changetracking mode
# todo MIRRORING if recovering using input file, need to make sure change is still possible to apply
#
# todo MIRRORING what is: multi_home = gpmlib.chk_multi_home(hosts) (from other gprecoverseg)
#
# todo MIRRORING checks for being able to build target directory
# todo MIRRORING make sure we get good errors out when trying to build mirror data directories but can't
#
# todo MIRRORING what about chk_pool_dir_available from gprecoverseg?  Is that needed herE?
# todo MIRRORING include check for passive standby master
#
# todo MIRRORING when failing over to another host we may need to change port numbers as well!
# todO MIRRORING open question: okay to pause fault prober while prompt to user is up?
#
# Options removed when 4.0 gprecoverseg was implemented:
#        --version
#       -S "Primary segment dbid to force recovery": I think this is done now by bringing the primary down, waiting for
#           failover, and then doing recover full
#       -z "Primary segment data dir and host to force recovery" see removed -S option for comment
#       -f        : force Greenplum Database instance shutdown and restart
#       -F (HAS BEEN CHANGED) -- used to mean "force recovery" and now means "full recovery)
# And a change to the -i input file: it now takes replicationPort in list of args (for failover target)
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST
from gppylib.mainUtils import *

from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE
import os, sys, getopt, socket, StringIO, signal

from gppylib import gparray, gplog, pgconf, userinput
from gppylib.commands import base, gp, pg, unix
from gppylib.db import catalog, dbconn
from gppylib.gpparseopts import OptParser, OptChecker
from gppylib.operations.startSegments import *
from gppylib.operations.buildMirrorSegments import *
from gppylib.programs import programIoUtils
from gppylib.system import configurationInterface as configInterface
from gppylib.system.environment import GpMasterEnvironment
from gppylib.test.testUtils import *
from gppylib.utils import toNonNoneString, checkNotNone, readAllLinesFromFile, writeLinesToFile

logger = gplog.get_default_logger()

#-------------------------------------------------------------------------
class GpRecoverSegmentProgram:

    #
    # Constructor:
    #
    # @param options the options as returned by the options parser
    #
    def __init__(self, options):
        self.__options = options
        self.__pool = None

    def outputToFile(self, mirrorBuilder, fileName):
        lines = []
        for mirror in mirrorBuilder.getMirrorsToBuild():
            str = ""
            seg = mirror.getFailedSegment()
            str += ('%s:%d:%s' % (seg.getSegmentAddress(), seg.getSegmentPort(), seg.getSegmentDataDirectory()))

            seg = mirror.getFailoverSegment()
            if seg is not None:
                str += ' '
                str += ('%s:%s:%d:%d:%s' % (seg.getSegmentAddress(), seg.getSegmentHostName(),
                    seg.getSegmentPort(), seg.getSegmentReplicationPort(), seg.getSegmentDataDirectory()))

            lines.append(str)
        writeLinesToFile(fileName, lines)

    def getRecoveryActionsFromConfigFile(self, gpArray):
        """
        getRecoveryActionsFromConfigFile

        returns a GpMirrorListToBuild object
        """

        failedSegments = []
        failoverSegments = []
        for line in readAllLinesFromFile(self.__options.recoveryConfigFile):
            line = line.strip()
            logger.info("Checking %s " % line)
            if line == '':
                continue
            qeArray = line.split()
            qeArrayLen = len(qeArray)
            if qeArrayLen < 1 or  qeArrayLen > 2:
                raise Exception('config file format error. %s' % line)

            arr = [s.strip() for s in qeArray[0].split(':')]
            if len(arr) != 3:
                raise Exception('config file format error. %s' % line)

            # find the failed segment
            (failedAddress, failedPort, failedDataDirectory) = arr
            failedSegment = None
            for segment in gpArray.getDbList():
                if segment.getSegmentAddress() == failedAddress and \
                            str(segment.getSegmentPort()) == failedPort and \
                            segment.getSegmentDataDirectory() == failedDataDirectory:

                    if failedSegment is not None:
                        raise Exception("Segment found twice: %s" % line)
                    failedSegment = segment

            if failedSegment is None:
                raise Exception("Segment not found: %s" % line )

            failoverSegment = None
            if qeArrayLen == 2:
                """
                When the length is two, the caller is going to tell us to where we need to failover, so
                  build a failover segment
                """
                arr = [s.strip() for s in qeArray[1].split(':')]
                if len(arr) != 5:
                    raise Exception('Config file format error. %s' % line)

                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()

                try:
                    port = int(arr[2])
                    replicationPort = int(arr[3])
                except ValueError:
                    raise Exception( 'Config file format error, invalid number in line: %s' % (line))

                # now update values in failover segment
                failoverSegment.setSegmentAddress( arr[0] )
                failoverSegment.setSegmentHostName( arr[1] )
                failoverSegment.setSegmentPort( port )
                failoverSegment.setSegmentReplicationPort( replicationPort )
                failoverSegment.setSegmentDataDirectory( arr[4] )

            # this must come AFTER the if check above because failedSegment can be adjusted to
            #   point to a different object
            failedSegments.append(failedSegment)
            failoverSegments.append(failoverSegment)

        peersForFailedSegments = self.findAndValidatePeersForFailedSegments(gpArray, failedSegments)

        segs = []
        for i in range(len(failedSegments)):
            segs.append( GpMirrorToBuild(failedSegments[i], peersForFailedSegments[i], failoverSegments[i], \
                                self.__options.forceFullResynchronization))
        return GpMirrorListToBuild(segs, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    def findAndValidatePeersForFailedSegments(self, gpArray, failedSegments):
        dbIdToPeerMap = gpArray.getDbIdToPeerMap()
        peersForFailedSegments = [ dbIdToPeerMap.get(seg.getSegmentDbId()) for seg in failedSegments]
        for i in range(len(failedSegments)):
            peer = peersForFailedSegments[i]
            if peer is None:
                raise Exception("No peer found for dbid %s" % failedSegments[i].getSegmentDbId())
            elif peer.isSegmentDown():
                raise Exception("Both segments for content %s are down; Try restarting Greenplum DB and running %s again." %
                        (peer.getSegmentContentId(), getProgramName()))
        return peersForFailedSegments

    def getRecoveryActionsFromConfiguration(self, gpArray):
        """
        getRecoveryActionsFromConfiguration

        returns a GpMirrorListToBuild object
        """
        segments = gpArray.getSegDbList()

        failedSegments = [ seg for seg in segments if seg.isSegmentDown() ]
        peersForFailedSegments = self.findAndValidatePeersForFailedSegments(gpArray, failedSegments)

        forceFull = self.__options.forceFullResynchronization
        segs = []
        for i in range(len(failedSegments)):

            failoverSegment = None
            failedSegment = failedSegments[i]
            liveSegment = peersForFailedSegments[i]

            if self.__options.newRecoverHost is not None:
                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()
                failoverSegment.setSegmentHostName( self.__options.newRecoverHost )

            if self.__options.spareDataDirectory is not None:
                # these two lines make it so that failoverSegment points to the object that is registered in gparray
                failoverSegment = failedSegment
                failedSegment = failoverSegment.copy()
                failoverSegment.setSegmentDataDirectory( self.__options.spareDataDirectory )

            segs.append( GpMirrorToBuild(failedSegment, liveSegment, failoverSegment, forceFull ))

        return GpMirrorListToBuild(segs, self.__pool, self.__options.quiet, self.__options.parallelDegree)

    # San-failback is handled separately from the Filerep-recovery operations.
    # 
    # Apparently the gpmlib and gplib stuff was supposed to be deprecated, but 
    # I cannot find the equivalent functionality anywhere else -- I need to
    # walk through results returned by shell commands digging for information
    # this isn't as easy as I'd like.
    def SanFailback(self, array_config, gpEnv):

        sys.path.append(sys.path[0] + '/lib')
        try:
            import gplib, gpmlib
            import getopt, math, StringIO, stat, subprocess, signal
            from gpmlib import ENV
        except ImportError, e:
            sys.exit('Error: unable to import module: ' + str(e))

        # Verify the commands that we depend on.
        ENV.chk_environ()

        # Get the configuration information maps.
        (san_mounts, san_mount_by_dbid) = array_config.getSanConfigMaps()

        # 1 Get the failed segments
        bad_segs = {}
        for dbid, v in san_mount_by_dbid.iteritems():
            (status, content, mountlist) = v
            if status == 'd':
                logger.info('Bad segment with dbid %d' % dbid)
                bad_segs[dbid] = (status, content, mountlist)

        # 2 Get the failed mountpoints.
        bad_mounts = {}
        for mount_id, v in san_mounts.iteritems():
            if v['active'] == 'm':
                logger.info('Bad mountpoint with id %d' % mount_id)
                bad_mounts[mount_id] = v

        # 3 Verify that the required hosts are back up (this may reduce the number of recoverable segments)
        recoverable_mps = {}
        for mount_id, v in bad_mounts.iteritems():
            tmpfile = os.path.join('/tmp', 'gptmpfile_recovery')
            #gpmlib.log_info('verifying host %s tmpfile %s' % (v['primary_host'], tmpfile))

            (ok, out) = gpmlib.run_warn('%s "%s %s"' % (gplib.ssh_prefix(host=v['primaryhost']), ENV.TOUCH, tmpfile))
            if ok:
                (ok, out) = gpmlib.run('%s "%s -f %s"' % (gplib.ssh_prefix(host=v['primaryhost']), ENV.RM, tmpfile))            
                recoverable_mps[mount_id] = v

        # 4
        # From the recoverable mountpoints, we should now be able to identify the
        # mountpoints required to recover for the segments. A segment is recoverable
        # if all of its mountpoints are recoverable.
        recoverable_segs = {}
        for dbid, v in bad_segs.iteritems():
            (status, content, mountlist) = v
            recoverable = True
            for mount_id in mountlist:
                if not recoverable_mps.has_key(mount_id):
                    recoverable = False
                    break
            if recoverable:
                recoverable_segs[dbid] = v
            else:
                logger.warning('Unrecoverable segment dbid %d' % (dbid))

        # 4 Stop GPDB.
        e = os.system('gpstop -aq -d %s' % (os.environ.get('MASTER_DATA_DIRECTORY')))
        ok = not e
        if not ok:
            logger.error('Failed to shutdown Greenplum Database: segment recovery cannot proceed.')
        else:
            logger.info('Successfully shutdown the Greenplum Database')

        # 5 Move mountpoints
        #   For each recoverable seg, walk its mountlist.
        # 5a
        #   unmount on failover host.            
        # 5b
        #   reconnect to primary.
        # 5c
        #   mount on primary.
        mount_done = {}
        for dbid, v in recoverable_segs.iteritems():
            (status, content, mountlist) = v

            for mount_id in mountlist:
                if mount_done.has_key(mount_id):
                    continue # already moved this

                if self.SanFailback_mountpoint(mount_id, recoverable_mps[mount_id]) == 0:
                    # TODO: some kind of error handling here ??
                    mount_done[mount_id] = True
                else:
                    mount_done[mount_id] = False

        logger.debug('Completed mount-recovery:')
        for mount_id, v in mount_done.iteritems():
            if v:
                logger.debug('mount-id %d ---> TRUE' % mount_id)
            else:
                logger.debug('mount-id %d ---> FALSE' % mount_id)

        # N - 3
        # Start GPDB in admin-mode
        e = os.system('gpstart -m -d %s' % (os.environ.get('MASTER_DATA_DIRECTORY')))
        ok = not e
        if not ok:
            logger.error('Failed to bring Greenplum Database up in management mode: segment recovery failed')
        else:
            logger.info('Greenplum Database restarted for configuration update')

        # N - 2
        # Update configuration

        # Open a connection to the DB.
        conn = None
        try:
            db_url = dbconn.DbURL(port=gpEnv.getMasterPort(), dbname='template1')

            conn = dbconn.connect(db_url, utility=True)

            dbconn.execSQL(conn, "BEGIN")

            logger.debug('Starting Transaction')

            # Update gp_san_configuration
            for mount_id, v in mount_done.iteritems():
                logger.debug('Checking Mount id %d' % mount_id)
                if v:
                    sql = 'UPDATE gp_san_configuration SET active_host=\'p\' WHERE mountid=%d' % mount_id
                    logger.debug('Issuing SQL [%s]' % sql)
                    dbconn.executeUpdateOrInsert(conn, sql, 1)

            # Update gp_segment_configuration
            for dbid, v in recoverable_segs.iteritems():
                (status, content, mountlist) = v

                logger.debug('Checking dbid id %d' % dbid)

                all_mountpoints = True
                for mount_id, v in mount_done.iteritems():
                    logger.debug('Scanning mountid %d in dbid id %d' % (mount_id, dbid))
                    if not v:
                        logger.debug('Mountid %d --> False' % mount_id)
                        all_mountpoints = False
                    else:
                        logger.debug('Mountid %d --> True' % mount_id)

                if all_mountpoints:
                    sql = 'UPDATE gp_segment_configuration SET status = \'u\' where dbid = %d' % dbid

                    logger.debug('Issuing SQL [%s]' % sql)
                    dbconn.executeUpdateOrInsert(conn, sql, 1)

                    sql = 'UPDATE gp_segment_configuration SET role = preferred_role where content = %d' % content
                    logger.debug('Issuing SQL [%s]' % sql)
                    dbconn.executeUpdateOrInsert(conn, sql, 2)
                else:
                    logger.info('Failed to recover sufficient mountpoints for dbid %d' % dbid)

            logger.debug('Committing our updates.')
            dbconn.execSQL(conn, "COMMIT")
        finally:
            if conn:
                conn.close()

        # N - 1 
        # Stop GPDB-admin-mode
        e = os.system('gpstop -m -d %s' % (os.environ.get('MASTER_DATA_DIRECTORY')))
        ok = not e
        if not ok:
            logger.error('Failed to stop Greenplum Database up in management mode: segment recovery failed')
        else:
            logger.info('Greenplum Database stopped, preparing for full restart.')

        # N Start GPDB
        e = os.system('gpstart -aq -d %s' % (os.environ.get('MASTER_DATA_DIRECTORY')))
        ok = not e
        if not ok:
            logger.error('Failed to restart Greenplum Database: segment recovery failed')
        else:
            logger.info('Successfully restarted the Greenplum Database')
        return 0

    def SanFailback_mountpoint(self, mp_id, mp_dict):
        active = mp_dict['active']
        type = mp_dict['type']

        if active == 'm':
            oldhost = mp_dict['primaryhost']
            old_mp = mp_dict['primarymountpoint']

            newhost = mp_dict['mirrorhost']
            new_mp = mp_dict['mirrormountpoint']
        else:
            # failback unnecessary ?
            logger.info('Not failback required for mount id %d primary is active!' % mp_id)
            return 0

        # RUN GP_MOUNT_AGENT HERE ??

        command = 'gp_mount_agent --agent -u -t %c -a %c -p %s -d %s -m %s -q %s -e %s -n %s' % (mp_dict['type'], mp_dict['active'],
                                                                                                 mp_dict['primaryhost'], mp_dict['primarydevice'], mp_dict['primarymountpoint'],
                                                                                                 mp_dict['mirrorhost'], mp_dict['mirrordevice'], mp_dict['mirrormountpoint'])
                                                                                         
        logger.debug('gp_mount_agent command is \'%s\'' % command)

        e = os.system(command)
        ok = not e
        if not ok:
            logger.error('gp_mount_agent: failed to relocate mount point %d' % mp_id)

        return 0

    def getRecoveryActionsBasedOnOptions(self, gpArray):
        if self.__options.recoveryConfigFile is not None:
            return self.getRecoveryActionsFromConfigFile(gpArray)
        else:
            return self.getRecoveryActionsFromConfiguration(gpArray)

    def displayRecovery(self, mirrorBuilder):
        logger.info('Greenplum instance recovery parameters')
        logger.info('---------------------------------------------------------')

        if self.__options.recoveryConfigFile:
            logger.info('Recovery from configuration -i option supplied')
        elif self.__options.newRecoverHost is not None:
            logger.info('Recovery type             = Pool Host')
            logger.info('Pool host for recovery    = %s' % self.__options.newRecoverHost)
            type_text = 'Pool    '
        elif self.__options.spareDataDirectory is not None:
            logger.info('Recovery type             = Pool Directory')
            logger.info('Mirror pool directory     = %s' % self.__options.spareDataDirectory)
            type_text = 'Pool dir'
        else:
            logger.info('Recovery type             = Standard')
            type_text = 'Failed  '

        count = 0
        # logger.info('Recovery parallel batch value = %d' % opt['-B'])
        i = 0
        total = len(mirrorBuilder.getMirrorsToBuild())
        for toRecover in mirrorBuilder.getMirrorsToBuild():
            logger.info('---------------------------------------------------------')
            logger.info('Recovery %d of %d' % (i+1, total))
            logger.info('---------------------------------------------------------')

            syncMode = "Full" if toRecover.isFullSynchronization() else "Incremental"
            logger.info("Synchronization mode                  = %s" % syncMode)

            programIoUtils.displaySegmentInfo("Failed", toRecover.getFailedSegment())
            programIoUtils.displaySegmentInfo("Recovery", toRecover.getLiveSegment())

            if toRecover.getFailoverSegment() is not None:
                programIoUtils.displaySegmentInfo("Failover", toRecover.getFailoverSegment())
            else:
                logger.info("Recover the failed segment in-place")

            i = i + 1

        logger.info('---------------------------------------------------------')

    def run(self):
        if self.__options.parallelDegree < 1 or self.__options.parallelDegree > 64:
            raise ProgramArgumentValidationException("Invalid parallelDegree provided with -B argument: %d" % self.__options.parallelDegree)

        self.__pool = base.WorkerPool(self.__options.parallelDegree)
        gpEnv = GpMasterEnvironment(self.__options.masterDataDirectory, True)

        # verify "where to recover" options
        optionCnt = 0
        if self.__options.newRecoverHost is not None:
            optionCnt += 1
        if self.__options.spareDataDirectory is not None:
            optionCnt += 1
        if self.__options.recoveryConfigFile is not None:
            optionCnt += 1
        if optionCnt > 1:
            raise ProgramArgumentValidationException(\
                    "Only one of -i, -p, -s may be specified")

        # if gpmlib.chk_on_passive_standby(masterPort): # todo: what is this doing?  Proper error message
         #    raise Exception('Unable to process requested command')

        faultProber = faultProberInterface.getFaultProber().initializeProber(gpEnv.getMasterPort())
        try:
            confProvider = configInterface.getConfigurationProvider().initializeProvider(gpEnv.getMasterPort(), False)
            gpArray = confProvider.loadSystemConfig()

            # check that we actually have mirrors
            if gpArray.getFaultStrategy() == gparray.FAULT_STRATEGY_SAN:
                self.SanFailback(gpArray, gpEnv)
                return 0
            elif gpArray.getFaultStrategy() != gparray.FAULT_STRATEGY_FILE_REPLICATION:
                raise ExceptionNoStackTraceNeeded( \
                        'GPDB Mirroring replication is not configured for this Greenplum Database instance.')

            # We have phys-rep/filerep mirrors.

            faultProber.pauseFaultProber()

            # figure out what needs to be done
            mirrorBuilder = self.getRecoveryActionsBasedOnOptions(gpArray)

            if self.__options.outputSampleConfigFile is not None:
                # just output config file and done
                self.outputToFile(mirrorBuilder, self.__options.outputSampleConfigFile)
                logger.info('Configuration file output to %s successfully.' % self.__options.outputSampleConfigFile)
            elif len(mirrorBuilder.getMirrorsToBuild()) == 0:
                logger.info('No segments to recover')
            else:
                mirrorBuilder.checkForPortAndDirectoryConflicts(gpArray)

                if self.__options.interactive:
                    self.displayRecovery(mirrorBuilder)
                    if not userinput.ask_yesno(None, "\nContinue with segment recovery procedure", 'N'):
                        raise UserAbortedException()

                mirrorBuilder.buildMirrors("recover", gpEnv, gpArray )

                logger.info("Segments have been recovered; data synchronization is in progress.")
                logger.info("Use gpstate to check the resynchronization progress.")

        finally:
            if faultProber.isFaultProberPaused():
                faultProber.unpauseFaultProber()

        return 0 # success -- exit code 0!

    def cleanup(self):
        if self.__pool:
            self.__pool.haltWork()

    #-------------------------------------------------------------------------
    @staticmethod
    def createParser():

        description = ("Recover a failed segment")
        help = [""]

        parser = OptParser(option_class=OptChecker,
                    description=' '.join(description.split()),
                    version='%prog version $Revision: #1 $')
        parser.setHelp(help)

        addStandardLoggingAndHelpOptions(parser, True)

        addTo = OptionGroup(parser, "Connection Options")
        parser.add_option_group(addTo)
        addMasterDirectoryOptionForSingleClusterProgram(addTo)

        addTo = OptionGroup(parser, "Recovery Source Options")
        parser.add_option_group(addTo)
        addTo.add_option("-i", None, type="string",
                            dest="recoveryConfigFile", 
                            metavar="<configFile>",
                            help="Recovery configuration file")
        addTo.add_option("-o", None,
                            dest="outputSampleConfigFile",
                            metavar="<configFile>", type="string",
                            help="Sample configuration file name to output; "
                                "this file can be passed to a subsequent call using -i option")

        addTo = OptionGroup(parser, "Recovery Destination Options")
        parser.add_option_group(addTo)
        addTo.add_option("-p", None, type="string",
                            dest="newRecoverHost",
                            metavar="<targetHost>",
                            help="Spare new host to which to recover segments")
        addTo.add_option("-s", None, type="string",
                            dest="spareDataDirectory", 
                            metavar="<targetDirectory>",
                            help="Spare data directories on current hosts")

        addTo = OptionGroup(parser, "Recovery Options")
        parser.add_option_group(addTo)
        addTo.add_option('-F', None, default=False, action='store_true',
                            dest="forceFullResynchronization",
                            metavar="<forceFullResynchronization>",
                            help="Force full segment resynchronization")
        addTo.add_option("-B", None, type="int", default=16,
                            dest="parallelDegree",
                            metavar="<parallelDegree>",
                            help="Max # of workers to use for building recovery segments.  [default: %default]")

        parser.set_defaults()
        return parser

    @staticmethod
    def createProgram(options, args):
        if len(args) > 0 :
            raise ProgramArgumentValidationException(\
                            "too many arguments: only options may be specified", True)
        return GpRecoverSegmentProgram(options)
    
        
    
