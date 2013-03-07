from gppylib.commands.base import Command
import unittest2 as unittest
import os
import tinctest
from tinctest.models.gpdb.upgrade import UpgradeTestCase
from tinctest.runner import TINCTextTestResult

'''
@upgrade
'''

class test_upgrade(UpgradeTestCase):
    DEFAULT_PULSE_HOST = 'pulse.greenplum.com'
    DEFAULT_BUILD_PROJECT = 'GPDB-main-opt-l1'
    DEFAULT_PROJECT_VERSION = 'dev'

    def tearDown(self):
        self.cleanup_upgrade()

    def construct_pulse_link(self):
        """Construct download link from pulse for the new binary.
        The values are overloaded by environment variables.
        """

        pulse_host = os.environ.get('TINC_UPGRADE_PULSE_HOST',
                                     self.DEFAULT_PULSE_HOST)
        build_project = os.environ.get('TINC_UPGRADE_BUILD_PROJECT',
                                        self.DEFAULT_BUILD_PROJECT)
        project_version = os.environ.get('TINC_UPGRADE_PROJECT_VERSION',
                                          self.DEFAULT_PROJECT_VERSION)
        return ('http://{host}/browse/projects/{project}/builds/success/'
                'downloads/rhel5_x86_64/Build%20GPDB/GPDB%20installer/'
                'greenplum-db-{ver}-RHEL5-x86_64.zip').format(
                        host=pulse_host,
                        project=build_project,
                        ver=project_version)

    def setupGPDB42and43(self):
        """Download GPDB from pulse and install it.
        The 4.2 will be retrieved from the default url,
        while 4.3 can be configured by environment variables.
        """

        url = self.construct_pulse_link()
        self.install_GPDB("4.2.2.4 1 rc",
                          "4.3_MAIN latest continuous",
                          new_gpdb_download_link=url)
        self.setup_upgrade()

    def __init__(self, methodName):
        super(test_upgrade, self).__init__(methodName)
        self.cluster = "init"

    @unittest.skipIf('BLDWRAP_TOP' in os.environ,
                     'skip in pulse(BLDWRAP_TOP is set)')
    def test_upgrade_42_43_existing_binaries(self):
        # path is relative to tincrepo/main
        if os.path.exists(os.getcwd() + '/upgrade/gpdb_4.2.2.4') and os.path.exists(os.getcwd() + '/upgrade/gpdb_4.3_MAIN'):
            self.setup_upgrade(old_gpdb_path='gpdb_4.2.2.4', new_gpdb_path='gpdb_4.3_MAIN', old_version='4.2.2.4', new_version='4.3_MAIN')
            self.run_workload("test_dir")
            self.validate_workload("test_dir")
            self.run_migrator()
            self.validate_upgrade_correctness()
            self.validate_workload("test_dir", use_diff_ans_file=True)
            self.cleanup_upgrade()

    def test_upgrade_41_43_negative(self):
        url = self.construct_pulse_link()
        self.install_GPDB("4.1.2.6 1 rc",
                          "4.3_MAIN latest continuous",
                          new_gpdb_download_link=url)
        self.setup_upgrade()
        self.run_migrator(expected_error_string="ERROR:  could not open relation with OID 3220 (heapam.c:884)")
        self.cleanup_upgrade()

    def test_upgrade_42_43_sanity(self):
        self.setupGPDB42and43()
        self.run_workload("test_dir")
        self.validate_workload("test_dir")
        self.run_migrator()
        self.validate_upgrade_correctness()
        self.validate_workload("test_dir", use_diff_ans_file=True)
        self.cleanup_upgrade()

    def test_upgrade_42_43_sanity_perfmon(self):
        self.setupGPDB42and43()
        self.run_workload("test_dir")
        self.validate_workload("test_dir")
        self.run_migrator(enable_perfmon=True)
        self.validate_upgrade_correctness()
        self.validate_workload("test_dir", use_diff_ans_file=True)
        self.cleanup_upgrade()

    def test_upgrade_42_43_mirror(self):
        self.setupGPDB42and43()
        self.run_workload("test_dir")
        self.validate_workload("test_dir")
        self.run_migrator()
        self.validate_upgrade_correctness()
        self.validate_workload("test_dir", use_diff_ans_file=True)
        self.cleanup_upgrade()

    def test_backup_restore_with_upgrade(self):
        self.setupGPDB42and43()
        self.run_workload("bkup_dir")
        cmd = Command(name='run gpcrondump', cmdStr='export MASTER_DATA_DIRECTORY=%s; export PGPORT=%s; source %s;gpcrondump -x %s -a' % (self.mdd + "/gpseg-1", self.db_port, self.old_gpdb_path + '/greenplum_path.sh', 'bkdb'))
        cmd.run()
        if cmd.get_results().rc != 0:
            raise Exception("gpcrondump failed with rc:  (%d)" % (cmd.get_results().rc))
        self.run_migrator()
        self.validate_upgrade_correctness()
        cmd = Command(name='run gpdbrestore', cmdStr='export MASTER_DATA_DIRECTORY=%s; export PGPORT=%s;export PGDATABASE=%s;source %s;gpdbrestore -e -s %s -a' % (self.mdd + "/gpseg-1", self.db_port, 'gptest',  self.new_gpdb_path + '/greenplum_path.sh', 'bkdb'))
        cmd.run()
        if cmd.get_results().rc != 0:
            raise Exception("gpdbrestore failed with rc:  (%d)" % (cmd.get_results().rc))
        self.validate_workload("bkup_dir")
        self.cleanup_upgrade()
