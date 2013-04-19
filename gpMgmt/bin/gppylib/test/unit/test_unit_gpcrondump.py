#!/usr/bin/env python

import os, sys
import unittest2 as unittest
from gppylib import gplog
from gpcrondump import GpCronDump, FREE_SPACE_PERCENT
from gppylib.operations.utils import DEFAULT_NUM_WORKERS
from mock import patch
from gppylib.mainUtils import ExceptionNoStackTraceNeeded


logger = gplog.get_unittest_logger()

class GpCronDumpTestCase(unittest.TestCase):

    class Options:
        def __init__(self):
            self.masterDataDirectory = ""
            self.interactive = False
            self.clear_dumps_only = False
            self.post_script = None
            self.dump_config = False
            self.history = False
            self.pre_vacuum = False
            self.post_vacuum = False
            self.rollback = False
            self.compress = True
            self.free_space_percent = None
            self.clear_dumps = False
            self.dump_schema = False
            self.dump_databases = 'testdb'
            self.bypass_disk_check = True
            self.backup_set = None
            self.dump_global = False
            self.clear_catalog_dumps = False
            self.batch_default = DEFAULT_NUM_WORKERS
            self.include_dump_tables = None
            self.exclude_dump_tables = None
            self.include_dump_tables_file = None
            self.exclude_dump_tables_file = None
            self.backup_dir = None
            self.encoding = None
            self.output_options = None
            self.incremental = False
            self.ddboost = False
            self.ddboost_hosts = None
            self.ddboost_user = None
            self.ddboost_config_remove = False
            self.ddboost_verify = False
            self.ddboost_remote = None
            self.ddboost_ping = None
            self.ddboost_backupdir = None
            self.replicate = None
            self.max_streams = None
            self.report_dir = None
 
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options1(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables = 'foo'
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'include table list can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options2(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.exclude_dump_tables = 'foo'
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'exclude table list can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options3(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables_file = 'foo'
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'include table file can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options4(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.exclude_dump_tables_file = 'foo'
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'exclude table file can not be selected with incremental backup'):
            cron = GpCronDump(options, None)
  
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options5(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = True 
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'ddboost can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options6(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost_verify = True 
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'ddboost can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options7(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost_config_remove = True 
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'ddboost can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options8(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost_user = 'abcde' 
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'ddboost can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options9(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost_hosts = 'abcde' 
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'ddboost can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gppylib.operations.backup_utils.get_latest_full_dump_timestamp', return_value='20121225090000')
    def test_options10(self, mock, mock2, mock3):
        options = GpCronDumpTestCase.Options()
        options.incremental = True
        cron = GpCronDump(options, None)
        self.assertEquals(cron.full_dump_timestamp, '20121225090000')

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options12(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.incremental = True
        options.dump_databases = 'bkdb,fulldb'
        with self.assertRaisesRegexp(Exception, 'multi-database backup is not supported with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gppylib.operations.backup_utils.get_latest_full_dump_timestamp', return_value='20120330090000')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.GpCronDump._get_master_port')
    def test_options13(self, mock, mock2, mock3):
        options = GpCronDumpTestCase.Options()
        options.incremental = True
        options.dump_databases = 'bkdb'

        #If this is successful then it should not raise an exception
        GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options14(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.incremental = False
        options.dump_databases = 'bkdb'

        #If this is successful then it should not raise an exception
        GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options15(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.incremental = False
        options.dump_databases = 'bkdb,fulldb'

        #If this is successful then it should not raise an exception
        GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options16(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.backup_dir = '/foo1'
        gpcd = GpCronDump(options, None)
        self.assertEquals(gpcd.getBackupDirectoryRoot(), '/foo1')

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options17(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.backup_dir = None
        gpcd = GpCronDump(options, None)
        self.assertEquals(gpcd.getBackupDirectoryRoot(), '/tmp/foobar')

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options18(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.dump_schema = 'foo'
        options.incremental = True
        with self.assertRaisesRegexp(Exception, '-s option can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options19(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.clear_dumps = True
        options.incremental = True
        with self.assertRaisesRegexp(Exception, '-c option can not be selected with incremental backup'):
            cron = GpCronDump(options, None)

 
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options20(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.dump_databases = None
        options.incremental = True
        with self.assertRaisesRegexp(Exception, 'Must supply -x <database name> with incremental option'):
            cron = GpCronDump(options, None)
 
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options21(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = True
        options.replicate = False
        options.max_streams = 20
        with self.assertRaisesRegexp(Exception, '--max-streams must be specified along with --replicate'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options22(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = True
        options.replicate = True
        options.max_streams = None
        with self.assertRaisesRegexp(Exception, '--max-streams must be specified along with --replicate'):
            cron = GpCronDump(options, None)
        
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options23(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = True
        options.replicate = True
        options.max_streams = 0
        with self.assertRaisesRegexp(Exception, '--max-streams must be a number greater than zero'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options24(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = True
        options.replicate = True
        options.max_streams = "abc"
        with self.assertRaisesRegexp(Exception, '--max-streams must be a number greater than zero'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options25(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.ddboost = False
        options.replicate = False
        options.max_streams = 20
        with self.assertRaisesRegexp(Exception, '--replicate and --max-streams cannot be used without --ddboost'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_get_include_exclude_for_dump_database00(self, mock1, mock2):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertEquals(inc, None)
        self.assertEquals(exc, None)
 
    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.expand_partitions_and_populate_filter_file', return_value='/tmp/include_dump_tables_file')
    @patch('gpcrondump.get_lines_from_file', return_value=['public.t1', 'public.t2'])
    def test_get_include_exclude_for_dump_database01(self, mock1, mock2, mock3, mock4):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.include_dump_tables_file = '/mydir/incfile'
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertTrue(inc.startswith('/tmp/include_dump_tables_file'))

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.expand_partitions_and_populate_filter_file', return_value='/tmp/include_dump_tables_file')
    @patch('gpcrondump.get_lines_from_file')
    def test_get_include_exclude_for_dump_database02(self, mock1, mock2, mock3, mock4):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.include_dump_tables = ['public.t1', 'public.t2', 'public.t3']
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertTrue(inc.startswith('/tmp/include_dump_tables_file'))

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gppylib.operations.backup_utils.get_latest_full_dump_timestamp', return_value='20121225090000')
    def test_get_include_exclude_for_dump_database03(self, mock1, mock2, mock3):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.incremental = True
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertEquals(inc, '/tmp/dirty')
        self.assertEquals(exc, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.expand_partitions_and_populate_filter_file', return_value='/tmp/exclude_dump_tables_file')
    @patch('gpcrondump.get_lines_from_file', return_value=['public.t1', 'public.t2'])
    def test_get_include_exclude_for_dump_database04(self, mock1, mock2, mock3, mock4):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.exclude_dump_tables_file = '/odir/exfile'
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertTrue(exc.startswith('/tmp/exclude_dump_tables_file'))

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.expand_partitions_and_populate_filter_file', return_value='/tmp/exclude_dump_tables_file')
    @patch('gpcrondump.get_lines_from_file')
    def test_get_include_exclude_for_dump_database06(self, mock1, mock2, mock3, mock4):
        options = GpCronDumpTestCase.Options()
        options.masterDataDirectory = '/tmp/foobar'
        options.exclude_dump_tables = ['public.t4', 'public.t5', 'public.t6']
        gpcd = GpCronDump(options, None)
        dirtyfile = '/tmp/dirty'
        dbname = 'foo'
        (inc, exc) = gpcd.get_include_exclude_for_dump_database(dirtyfile, dbname)
        self.assertTrue(exc.startswith('/tmp/exclude_dump_tables_file'))

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_inserts_with_incremental(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.output_options = ['--inserts']
        options.incremental = True
        with self.assertRaisesRegexp(Exception, '--inserts, --column-inserts, --oids cannot be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_inserts_with_incremental(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.output_options = ['--oids']
        options.incremental = True
        with self.assertRaisesRegexp(Exception, '--inserts, --column-inserts, --oids cannot be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_inserts_with_incremental(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.output_options = ['--column-inserts']
        options.incremental = True
        with self.assertRaisesRegexp(Exception, '--inserts, --column-inserts, --oids cannot be selected with incremental backup'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.GpCronDump._get_table_names_from_partition_list', side_effect = [['public.aot1', 'public.aot2'], ['public.cot1', 'public.cot2'], ['public.heapt1', 'public.heapt2']])
    def test_verify_tablenames_00(self, mock1, mock2, mock3):
        options = GpCronDumpTestCase.Options()
        cron = GpCronDump(options, None)
        ao_partition_list = ['public, aot1, 2190', 'public, aot2, 3190']
        co_partition_list = ['public, cot1, 2190', 'public, cot2, 3190']
        heap_partition_list = ['public, heapt1, 2190', 'public, heapt2, 3190']
        cron._verify_tablenames(ao_partition_list, co_partition_list, heap_partition_list) #Should not raise an exception

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    @patch('gpcrondump.GpCronDump._get_table_names_from_partition_list', side_effect = [['public.aot1:asd', 'public.aot2'], ['public.cot1', 'public.cot2:asd'], ['public.heapt1', 'public.heapt2,asdasd']])
    def test_verify_tablenames_00(self, mock1, mock2, mock3):
        options = GpCronDumpTestCase.Options()
        cron = GpCronDump(options, None)
        ao_partition_list = ['public, aot1:asd, 2190', 'public, aot2, 3190']
        co_partition_list = ['public, cot1, 2190', 'public, cot2:asd, 3190']
        heap_partition_list = ['public, heapt1, 2190', 'public, heapt2,asdasd , 3190']
        with self.assertRaisesRegexp(Exception, ''):
            cron._verify_tablenames(ao_partition_list, co_partition_list, heap_partition_list) #Should not raise an exception

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_get_table_names_from_partition_list_00(self, mock1, mock2):
        options = GpCronDumpTestCase.Options()
        cron = GpCronDump(options, None)
        partition_list = ['public, aot1, 2190', 'public, aot2:aot, 3190']
        expected_output = ['public.aot1', 'public.aot2:aot']
        result = cron._get_table_names_from_partition_list(partition_list)
        self.assertEqual(result, expected_output)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_get_table_names_from_partition_list_01(self, mock1, mock2):
        options = GpCronDumpTestCase.Options()
        cron = GpCronDump(options, None)
        partition_list = ['public, aot1, 2190', 'public, aot2,aot, 3190']
        with self.assertRaisesRegexp(Exception, 'Invalid partition entry "public, aot2,aot, 3190"'):
            cron._get_table_names_from_partition_list(partition_list)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter1(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables = 'foo'
        options.include_dump_tables_file = 'foo'
        with self.assertRaisesRegexp(Exception, '-t can not be selected with --table-file option'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter2(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables = 'foo'
        options.exclude_dump_tables_file = 'foo'
        with self.assertRaisesRegexp(Exception, '-t can not be selected with --exclude-table-file option'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter3(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.exclude_dump_tables = 'foo'
        options.exclude_dump_tables_file = 'foo'
        with self.assertRaisesRegexp(Exception, '-T can not be selected with --exclude-table-file option'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter4(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.exclude_dump_tables = 'foo'
        options.include_dump_tables_file = 'foo'
        with self.assertRaisesRegexp(Exception, '-T can not be selected with --table-file option'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter5(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables = 'foo'
        options.exclude_dump_tables = 'foo'
        with self.assertRaisesRegexp(Exception, '-t can not be selected with -T option'):
            cron = GpCronDump(options, None)

    @patch('gpcrondump.GpCronDump._get_master_port')
    @patch('gpcrondump.validate_current_timestamp')
    def test_options_table_filter6(self, mock, mock2):
        options = GpCronDumpTestCase.Options()
        options.include_dump_tables_file = 'foo'
        options.exclude_dump_tables_file = 'foo'
        with self.assertRaisesRegexp(Exception, '--table-file can not be selected with --exclude-table-file option'):
            cron = GpCronDump(options, None)

#------------------------------- Mainline --------------------------------
if __name__ == '__main__':
    unittest.main()
