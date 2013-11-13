#!/usr/bin/env python

# DANL: shirnked the import lines to a single line
import glob
import platform
import os
import datetime
import subprocess
import shutil
import tarfile
from optparse import OptionParser
from datetime import date
from contextlib import closing
from gppylib.commands import unix
from gppylib import gplog

logger = gplog.get_default_logger()
gplog.setup_tool_logging("gp_log_nodecollector", unix.getLocalHostname(),
                         unix.getUserName(),
                         os.path.join(os.path.expanduser('~'),
                                      'gpAdminLogs'))
# gplog.setup_tool_logging("gp_log_nodecollector",
# unix.getLocalHostname(), unix.getUserName(),
# "/home/gpadmin/gpAdminLogs")

class NodeCollector:

    def __init__(self):
        # Violating OOP in this case is just much quicker & easier
        # to set the inst vars from the command line optios this way
        parser = self.getOptParser()
        (options, args) = parser.parse_args()
        self.__dict__.update(vars(options))

        # If there is an error logged during collection return
        # erro back to master.
        self.fault = False

        logger.debug(options)  # print the options passed into node collector

        if self.quiet is True:
            gplog.quiet_stdout_logging()

        if self.verbose is True:
            gplog.enable_verbose_logging()

        if self.logDir == "":
            logger.error("Failed --logdir not defined")
            print "--logDir not defined"
            exit(1)

        # the OS value
        self.ostype = platform.system()

    def createTempDir(self):
        self.tempDir = os.path.join(self.tempDir, 'gp_log_nodecollect_' + str(
            datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')))
        if not os.path.isdir(self.tempDir):
            os.makedirs(self.tempDir)
        os.chdir(self.tempDir)  # Set set as our base working direcoty

    # --------------------------------------------------------------------
    # Generats the logrange for defined start to end dates
    def logRange(self, startDate, endDate=date.today()):
        delta = datetime.timedelta(1)
        cur_date = startDate
        while cur_date <= endDate:
            yield cur_date
            cur_date += delta

    # --------------------------------------------------------------------
    # collect segment specific data here:
    # - pg_controldata
    # - postgres.conf, pg_hba.conf
    # - gpdb instance logs
    # - xlogs ( default false )
    # - pg_clog ( default false )
    # - pg_changetracker ( default false )
    def collectSegInfo(self, segDef):
        # segDef is an array of strings of the form: [conent, role, location]
        content = segDef[0]
        role = segDef[1]
        location = segDef[2]
        logger.info(
            "Collecting logs for content id " + content + ": " + location)
        # make life easy - create a copy file prefix
        prefix = os.path.join(
            self.tempDir, platform.node() + '_seg' + content + '-' + role + '-')

        # DANL: make sure location direcotry actually exists before executing
        if not os.path.isdir(location):
            logger.error("directory " + location + " does not exist")
            self.fault = True
            return

        # OK - start by collecting the stuff we are always going to want:
        # controldata, postgresql.conf and pg_hba.conf
        subprocess.call('pg_controldata ' + location +
                        ' > ' + prefix + 'pg_controldata.out', shell=True)
        shutil.copy(os.path.join(
            location, 'postgresql.conf'), prefix + 'postgresql.conf')
        shutil.copy(os.path.join(
            location, 'pg_hba.conf'), prefix + 'pg_hba.conf')

        # Now for the DB logs
        for date in self.logRange(self.startDate, self.endDate):
            for log in glob.glob(os.path.join(location, 'pg_log', 'gpdb-' + date.isoformat() + '_*')):
                shutil.copy(log, prefix + os.path.basename(log))

        # Now for some of the optional bits
        if self.collectXLogs:
            logger.info("Collecting xlogs for content " + content)
            with closing(tarfile.open(prefix + 'pg_xlogs.tgz', 'w:gz')) as t:
                t.add(os.path.join(location, 'pg_xlog'))

        if self.collectCLogs:
            logger.info("Collecting Clogs for content " + content)
            with closing(tarfile.open(prefix + 'pg_clogs.tgz', 'w:gz')) as t:
                t.add(os.path.join(location, 'pg_clog'))

        if self.collectChangetracking:
            logger.info("Collecting ChangeTracking for content " + content)
            with closing(tarfile.open(prefix + 'pg_changetracking.tgz', 'w:gz')) as t:
                t.add(os.path.join(location, 'pg_changetracking'))

    # --------------------------------------------------------------------
    # Reorganises the self.tempDir and generates the final tarbal.
    # Once the tarball is uploaded to the master this functions removes
    # self.tempDir
    def pushToMaster(self):

        # get a directory listing becfore we creat a new dir
        dirlisting = os.listdir(self.tempDir)

        # make a direcotry for this nodes hostname
        hostname = platform.node()
        newDir = (os.path.join(self.tempDir, hostname))
        os.mkdir(newDir)

        tfilename = hostname + '_log_collection_' + str(
            datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')) + '.tgz'
        logger.info("Creating tarball: " + tfilename)

        # move all files to the new direcotry
        for f in dirlisting:
            shutil.move(os.path.join(self.tempDir, f), os.path.join(newDir, f))

        # tar everything up
        os.chdir(self.tempDir)
        with closing(tarfile.open(tfilename, 'w:gz')) as t:
            for f in os.listdir(self.tempDir):
                if not f == tfilename:
                    t.add(f)

        # push file to master
        logger.info("Pushing tarball to master")
        rt = subprocess.call(
            'scp ' + tfilename + ' ' + self.logDir, shell=True)
        if rt > 0:
            logger.error("Failed to push " + tfilename + " to master")
            self.fault = True
            return

    # --------------------------------------------------------------------
    # collect only host related information here
    # - ~/gpadmin|~/root gpAdminLogs
    # - ps info
    # - core file info ( default false )
    def collectHostInfo(self):
        hostname = platform.node()
        prefix = os.path.join(self.tempDir, hostname + '-')

        # start by collecting the gpAdminLogs
        # we check for root incase we have acess to these logs from gpadmin
        gpadmin_logs = os.path.join(
            os.path.expanduser('~gpadmin'), 'gpAdminLogs')
        root_logs = os.path.join(os.path.expanduser('~root'), 'gpAdminLogs')
        if os.path.isdir(gpadmin_logs):
            dest = os.path.join(self.tempDir, "gpadmin_gpAdminLogs")
            os.mkdir(dest)
            for f in os.listdir(gpadmin_logs):
                shutil.copy(os.path.join(gpadmin_logs, f), dest)
        if os.path.isdir(root_logs):
            dest = os.path.join(self.tempDir, "root_gpAdminLogs")
            os.mkdir(dest)
            for f in os.listdir(root_logs):
                shutil.copy(os.path.join(root_logs, f), dest)

        # now some basic OS stuff:
        corePath = self.matchCrashDir()
        subprocess.call('ps -eflyww > ' + prefix + 'ps_efww.out', shell=True)
        subprocess.call('pstree > ' + prefix + 'pstree.out', shell=True)
        if corePath:
            subprocess.call(
                'ls -lh ' + corePath + ' >' + prefix + 'ls_cores.out', shell=True)


# --------------------------------------------------------------------
# Main function of the node collector class that initiates the data collection
    def collectData(self):
        try:
            # Grab the host-wide info first
            self.createTempDir()
            self.checkFreeSpace()
            self.collectHostInfo()

            # now we grab each of the segment info
            if self.segsToCollect:
                for segDef in self.segsToCollect.split(','):
                    self.collectSegInfo(segDef.split(':'))

            # Push files to master
            self.pushToMaster()
        except (OSError, IOError) as e:
            logger.error('Exception: ' + str(e))
            self.fault = True
        finally:
            # clean up tempdir
            os.chdir(os.path.expanduser('~'))  # change back to homedir first
            logger.info("removing temp direcotry " + self.tempDir)
            if os.path.isdir(self.tempDir):
                subprocess.call('rm -rf ' + self.tempDir, shell=True)

# --------------------------------------------------------------------
    def checkFreeSpace(self):
        st = os.statvfs(self.tempDir)
        freespace = (float(st.f_bfree) / float(st.f_blocks)) * 100.0
        logger.debug(str(freespace) + '% space available on ' + self.tempDir)
        if freespace < 20.0:
            if self.skipFreeSpaceCheck == True:
                logger.warn('Less than 20% space left on ' + self.tempDir)
            else:
                logger.error('Less than 20% space left on ' + self.tempDir)
                print 'Less than 20% speace avaiable on ' + self.tempDir
                exit(1)

# --------------------------------------------------------------------
    def getOptParser(self):
        parser = OptionParser()
        parser.add_option(
            "-s", "--startDate", dest='startDate', action='callback',
            callback=self.setDateOption, nargs=1, type='string', metavar='START_DATE', default=date.today())
        parser.add_option("-e", "--endDate", dest='endDate', action='callback',
                          callback=self.setDateOption, nargs=1, type='string', metavar='END_DATE', default=date.today())
        parser.add_option(
            '-a', '--segs', dest='segsToCollect', action='store', default='')
        parser.add_option(
            '-x', '--xlogs', dest='collectXLogs', action='store_true', default=False)
        parser.add_option(
            '-y', '--clogs', dest='collectCLogs', action='store_true', default=False)
        parser.add_option(
            '-z', '--changetrack', dest='collectChangetracking', action='store_true', default=False)
        parser.add_option(
            '-l', '--logDir', dest='logDir', action='store', default="")
        parser.add_option(
            '-t', '--tempDir', dest='tempDir', action='store', default='/data1')
        parser.add_option(
            '-q', '--quiet', dest='quiet', action='store_true', default='False')
        # DANL: do not log to STDOUT when master executes
        # this script.  But log to STDOUT for user execution
        # DANL: this enable debug logging
        parser.add_option(
            '-v', '--verbose', dest='verbose', action='store_true', default='False')
        parser.add_option(
            '--skip-freespace-check', dest='skipFreeSpaceCheck', action='store_true', default='False')
        return parser

# --------------------------------------------------------------------
    def setDateOption(self, option, opt_str, value, parser, *args, **kwargs):
        setattr(parser.values, option.dest,  datetime.datetime.strptime(
            value, "%Y-%m-%d").date())

# --------------------------------------------------------------------
# Matches the core file crash location
    def matchCrashDir(self):
        core_path = False
        # get core pattern from /proc/sys/kernel/core_pattern)
        cp = subprocess.Popen(
            '/sbin/sysctl  -n kernel.core_pattern', shell=True, stdout=subprocess.PIPE)
        core_path = os.path.dirname(cp.stdout.read())

        if os.path.isdir(core_path):
            return core_path
        else:
            return False


def main():
    nc = NodeCollector()
    logger.info(
        "Initializing gp_log_collector collector class on segment node")
    logger.info("Starting data collection")
    nc.collectData()

    # anything to STDOUT is a return code for the master script
    # return value of "success" means script completed ok.
    # any other return value will result in failure reported by master script
    if nc.fault:
        print "Failed please see log file under ~/gpAdminLogs on segment node"
        exit(1)
    else:
        print "success"


if __name__ == "__main__":
    main()
