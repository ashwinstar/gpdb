#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2012. All Rights Reserved. 
#

import os
import shutil
import unittest2 as unittest
import gppylib.operations.backup_utils as backup_utils
from gppylib.commands.base import CommandResult
from gppylib.operations.backup_utils import generate_report_filename, validate_timestamp, generate_increments_filename,\
                                            check_cdatabase_exists, check_successful_dump, convert_reportfilename_to_cdatabasefilename,\
                                            get_backup_directory, generate_ao_state_filename, generate_co_state_filename,\
                                            create_temp_file_with_tables, generate_dirtytable_filename, generate_plan_filename,\
                                            generate_metadata_filename, generate_partition_list_filename, verify_lines_in_file,\
                                            write_lines_to_file, get_lines_from_file, get_full_ts_from_report_file, get_incremental_ts_from_report_file,\
                                            check_backup_type, get_timestamp_val, get_dump_dirs, get_latest_full_dump_timestamp, \
                                            generate_pgstatlastoperation_filename, get_latest_report_timestamp, get_latest_report_in_dir, \
                                            create_temp_file_from_list, get_timestamp_from_increments_filename, get_full_timestamp_for_incremental,\
                                            check_funny_chars_in_tablenames, expand_partition_tables, populate_filter_tables, expand_partitions_and_populate_filter_file, \
                                            generate_files_filename, generate_pipes_filename, generate_master_config_filename, generate_segment_config_filename, \
                                            generate_global_prefix, generate_master_dbdump_prefix, \
                                            generate_master_status_prefix, generate_seg_dbdump_prefix, generate_seg_status_prefix, \
                                            generate_dbdump_prefix, generate_createdb_filename

from mock import patch, MagicMock, Mock

class BackupUtilsTestCase(unittest.TestCase):

    def create_backup_dirs(self, top_dir=os.getcwd(), dump_dirs=[]):
        if dump_dirs is None:
            return

        for dump_dir in dump_dirs:
            backup_dir = os.path.join(top_dir, 'db_dumps', dump_dir)
            if not os.path.exists(backup_dir):
                os.makedirs(backup_dir)
                if not os.path.exists(backup_dir):
                    raise Exception('Failed to create directory %s' % backup_dir)
   
    def remove_backup_dirs(self, top_dir=os.getcwd(), dump_dirs=[]):
        if dump_dirs is None:
            return 

        for dump_dir in dump_dirs:
            backup_dir = os.path.join(top_dir, 'db_dumps', dump_dir)
            shutil.rmtree(backup_dir)
            if os.path.exists(backup_dir):
                raise Exception('Failed to remove directory %s' % backup_dir)

    def test00_generate_report_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030.rpt'
        output = generate_report_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test01_generate_report_filename(self):
        master_data_dir = '/data'
        backup_dir = '/datadomain'
        timestamp = '20120731093030'
        expected_output = '/datadomain/db_dumps/20120731/gp_dump_20120731093030.rpt'
        output = generate_report_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test02_generate_report_filename(self):
        master_data_dir = None
        backup_dir = '/datadomain'
        timestamp = '20120731093030'
        expected_output = '/datadomain/db_dumps/20120731/gp_dump_20120731093030.rpt'
        output = generate_report_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test03_generate_report_filename(self):
        master_data_dir = None
        backup_dir = None
        timestamp = '20120731093030'
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory with existing parameters'):
            generate_report_filename(master_data_dir, backup_dir, timestamp)

    def test04_generate_report_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = None
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory without timestamp'):
            generate_report_filename(master_data_dir, backup_dir, timestamp)

    def test05_generate_report_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = 'xx120731093030'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_report_filename(master_data_dir, backup_dir, timestamp)

    def test06_generate_report_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '2012'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_report_filename(master_data_dir, backup_dir, timestamp)

    def test07_validate_timestamp(self):
        ts = "20100729093000"
        result = validate_timestamp(ts)
        self.assertTrue(result)

    def test08_validate_timestamp(self):
        ts = "2010072909300"
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test09_validate_timestamp(self):
        ts = "201007290930000"
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test10_validate_timestamp(self):
        ts = "00000000000000"
        result = validate_timestamp(ts)
        self.assertTrue(result)

    def test11_validate_timestamp(self):
        ts = "0a000000000000"
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test12_validate_timestamp(self):
        ts = "0q000000000000"
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test13_validate_timestamp(self):
        ts = " 00000000000000"
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test14_validate_timestamp(self):
        ts = "00000000000000 "
        result = validate_timestamp(ts)
        self.assertFalse(result);

    def test15_validate_timestamp(self):
        ts = None
        result = validate_timestamp(ts)
        self.assertFalse(result)

    def test16_generate_increments_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030_increments'
        output = generate_increments_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test17_generate_increments_filename(self):
        master_data_dir = '/data'
        backup_dir = '/datadomain'
        timestamp = '20120731093030'
        expected_output = '/datadomain/db_dumps/20120731/gp_dump_20120731093030_increments'
        output = generate_increments_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test18_generate_increments_filename(self):
        master_data_dir = None
        backup_dir = '/datadomain'
        timestamp = '20120731093030'
        expected_output = '/datadomain/db_dumps/20120731/gp_dump_20120731093030_increments'
        output = generate_increments_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test19_generate_increments_filename(self):
        master_data_dir = None
        backup_dir = None
        timestamp = '20120731093030'
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory with existing parameters'):
            generate_increments_filename(master_data_dir, backup_dir, timestamp)

    def test20_generate_increments_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = None
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory without timestamp'):
            generate_increments_filename(master_data_dir, backup_dir, timestamp)

    def test21_generate_increments_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = 'xx120731093030'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_increments_filename(master_data_dir, backup_dir, timestamp)

    def test22_generate_increments_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '2012'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_increments_filename(master_data_dir, backup_dir, timestamp)
    
    def test23_convert_reportfilename_to_cdatabasefilename(self):
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        expected_output = '/tmp/foo/foo/gp_cdatabase_1_1_20130104133924'
        cdatabase_file = convert_reportfilename_to_cdatabasefilename(report_file)
        self.assertEquals(expected_output, cdatabase_file)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['--', '-- Database creation', '--', '', "CREATE DATABASE testdb WITH TEMPLATE = template0 ENCODING = 'UTF8' OWNER = dcddev;"])
    def test24_check_cdatabase_exists(self, mock):
        dbname = 'testdb'
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertTrue(result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['--', '-- Database creation', '--', '', "CREATE DATABASE testdb WITH TEMPLATE = template0 ENCODING = 'UTF8' OWNER = dcddev;"])
    def test25_check_cdatabase_exists(self, mock):
        dbname = 'bkdb'
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertFalse(result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['--', '-- Database creation', '--', '', "CREATE testdb WITH TEMPLATE = template0 ENCODING = 'UTF8' OWNER = dcddev;"])
    def test26_check_cdatabase_exists(self, mock):
        dbname = 'bkdb'
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertFalse(result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=[])
    def test27_check_cdatabase_exists(self, mock):
        dbname = 'bkdb'
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertFalse(result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['--', '-- Database creation', '--', '', 'CREATE DATABASE'])
    def test28_check_cdatabase_exists(self, mock):
        dbname = 'bkdb'
        report_file = '/tmp/foo/foo/gp_dump'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertFalse(result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['--', '-- Database creation', '--', '', "CREATE DATABASE testdb WITH TEMPLATE = template0 ENCODING = 'UTF8' OWNER = dcddev;"])
    def test29_check_cdatabase_exists(self, mock):
        dbname = 'testdb'
        report_file = '/tmp/foo/foo/gp_dump_20130104133924.rpt'
        result = check_cdatabase_exists(dbname, report_file) 
        self.assertTrue(result)

    def test30_get_backup_directory(self):
        mdd = '/data'
        backup_dir = None
        timestamp = '20121204090000'
        expected = '/data/db_dumps/20121204'
        result = get_backup_directory(mdd, backup_dir, timestamp)
        self.assertTrue(result, expected)

    def test31_get_backup_directory(self):
        mdd = '/data'
        backup_dir = '/tmp/foo'
        timestamp = '20121204090000'
        expected = '/tmp/foo/db_dumps/20121204'
        result = get_backup_directory(mdd, backup_dir, timestamp)
        self.assertTrue(result, expected)

    def test32_get_backup_directory(self):
        mdd = None
        backup_dir = None
        timestamp = '20121204090000'
        expected = '/tmp/foo/db_dumps/20121204'
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory with existing parameters'):
            result = get_backup_directory(mdd, backup_dir, timestamp)

    def test33_get_backup_directory(self):
        mdd = '/data'
        backup_dir = None
        timestamp = 'a0121204090000'
        expected = '/tmp/foo/db_dumps/20121204'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            result = get_backup_directory(mdd, backup_dir, timestamp)

    def test34_generate_dirtytable_filename(self):
        mdd = '/data'
        backup_dir = None
        timestamp = '20121204090000'
        expected = '/data/db_dumps/20121204/gp_dump_20121204090000_dirty_list'
        result = generate_dirtytable_filename(mdd, backup_dir, timestamp)
        self.assertEquals(expected, result)

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['t1', 't2'])
    def test35_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        # failure will raise an exception
        verify_lines_in_file(fname, expected)
        

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['t1 ', 't2'])
    def test36_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        with self.assertRaisesRegexp(Exception, 'contents not as expected, suspected IO error'):
            verify_lines_in_file(fname, expected)
 
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=[' t1', 't2'])
    def test37_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        with self.assertRaisesRegexp(Exception, 'contents not as expected, suspected IO error'):
            verify_lines_in_file(fname, expected)
 
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['t1'])
    def test38_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        with self.assertRaisesRegexp(Exception, 'contents not as expected, suspected IO error'):
            verify_lines_in_file(fname, expected)
 
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['t1', 't2', 't3'])
    def test39_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        with self.assertRaisesRegexp(Exception, 'contents not as expected, suspected IO error'):
            verify_lines_in_file(fname, expected)
 
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=[])
    def test40_verify_lines_in_file(self, mock):
        fname = 'foo'
        expected = ['t1', 't2']
        
        with self.assertRaisesRegexp(Exception, 'contents not as expected, suspected IO error'):
            verify_lines_in_file(fname, expected)
 
    def test41_generate_plan_filename(self):
        mdd = '/data'
        timestamp = '20121204090000'
        expected = '/data/db_dumps/20121204/gp_restore_20121204090000_plan'
        backup_dir = None
        result = generate_plan_filename(mdd, backup_dir, timestamp)
        self.assertEquals(expected, result)

    def test42_generate_partition_list_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030_table_list'
        output = generate_partition_list_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test43_generate_metadata_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_1_1_20120731093030.gz'
        output = generate_metadata_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test44_generate_metadata_filename(self):
        master_data_dir = None
        backup_dir = None
        timestamp = '20120731093030'
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory with existing parameters'):
            generate_metadata_filename(master_data_dir, backup_dir, timestamp)

    def test45_generate_metadata_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = None
        with self.assertRaisesRegexp(Exception, 'Can not locate backup directory without timestamp'):
            generate_metadata_filename(master_data_dir, backup_dir, timestamp)

    def test46_generate_metadata_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = 'xx120731093030'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_metadata_filename(master_data_dir, backup_dir, timestamp)

    def test47_generate_metadata_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '2012'
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp'):
            generate_metadata_filename(master_data_dir, backup_dir, timestamp)

    def test48_write_lines_to_file(self):
        lines = ['Hello', 'World\n', '  Greenplum   ']
        filename = os.path.join(os.getcwd(), 'abc')
        write_lines_to_file(filename, lines)
        self.assertTrue(os.path.isfile(filename))
        content = get_lines_from_file(filename)
        expected_output = [s.rstrip() for s in lines]
        self.assertEquals(expected_output, content)
        os.remove(filename)

    def test49_write_lines_to_file(self):
        lines = []
        filename = '/this_directory/doesnot/exist'
        with self.assertRaises(IOError):
            write_lines_to_file(filename, lines) 

    def test50_write_lines_to_file(self):
        lines = []
        filename = os.path.join(os.getcwd(), 'abc')
        write_lines_to_file(filename, lines)
        self.assertTrue(os.path.exists(filename))
        content = get_lines_from_file(filename) 
        self.assertEquals(lines, content)
        os.remove(filename)

    def test51_write_lines_to_file(self):
        lines = ['Hello', 'World', '', 'Greenplum']
        filename = os.path.join(os.getcwd(), 'abc')
        write_lines_to_file(filename, lines)
        self.assertTrue(os.path.isfile(filename))
        content = get_lines_from_file(filename)
        self.assertEquals(lines, content)
        os.remove(filename)

    def test52_check_successful_dump(self):
        successful_dump = check_successful_dump('testdb', ['gp_dump utility finished successfully.'])
        self.assertTrue(successful_dump)

    def test53_check_successful_dump(self):
        successful_dump = check_successful_dump('testdb', ['gp_dump utility finished unsuccessfully.'])
        self.assertFalse(successful_dump)

    def test54_check_successful_dump(self):
        successful_dump = check_successful_dump('testdb', [])
        self.assertFalse(successful_dump)

    def test55_check_successful_dump(self):
        successful_dump = check_successful_dump('testdb', ['gp_dump utility finished successfully.\n'])
        self.assertTrue(successful_dump)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'Timestamp Key: 01234567891234', 'gp_dump utility finished successfully.'])
    def test56_get_full_ts_from_report_file(self, mock1, mock2):
        expected_output = '01234567891234'
        ts = get_full_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=False)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'Timestamp Key: 01234567891234', 'gp_dump utility finished successfully.'])
    def test57_get_full_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_full_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)
        
    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'Timestamp Key: 01234567891234567', 'gp_dump utility finished successfully.'])
    def test58_get_full_ts_from_report_file(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp value found in report_file'):
            get_full_ts_from_report_file('testdb', 'foo')

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'Timestamp Key: xxx34567891234', 'gp_dump utility finished successfully.'])
    def test59_get_full_ts_from_report_file(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp value found in report_file'):
            get_full_ts_from_report_file('testdb', 'foo')

    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full'])
    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    def test60_get_full_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_full_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'gp_dump utility finished successfully.'])
    def test61_get_full_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_full_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental'])
    def test62_get_incremental_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_incremental_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental', 'gp_dump utility finished successfully.'])
    def test63_get_incremental_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_incremental_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Full', 'Timestamp Key: 01234567891234', 'gp_dump utility finished successfully.'])
    def test64_get_incremental_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_incremental_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental', 'Timestamp Key: 01234567891234', 'gp_dump utility finished successfully.'])
    def test65_get_incremental_ts_from_report_file(self, mock1, mock2):
        expected_output = '01234567891234'
        ts = get_incremental_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=False)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental', 'Timestamp Key: 01234567891234', 'gp_dump utility finished successfully.'])
    def test66_get_incremental_ts_from_report_file(self, mock1, mock2):
        expected_output = None
        ts = get_incremental_ts_from_report_file('testdb', 'foo')
        self.assertEqual(ts, expected_output)
        
    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental', 'Timestamp Key: 01234567891234567', 'gp_dump utility finished successfully.'])
    def test67_get_incremental_ts_from_report_file(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp value found in report_file'):
            get_incremental_ts_from_report_file('testdb', 'foo')

    @patch('gppylib.operations.backup_utils.check_cdatabase_exists', return_value=True)
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['Backup Type: Incremental', 'Timestamp Key: xxx34567891234', 'gp_dump utility finished successfully.'])
    def test68_get_incremental_ts_from_report_file(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp value found in report_file'):
            get_incremental_ts_from_report_file('testdb', 'foo')

    def test69_check_backup_type(self):
        backup_type = check_backup_type(['Backup Type: Full'], 'Full')
        self.assertEqual(backup_type, True)

    def test70_check_backup_type(self):
        backup_type = check_backup_type(['Backup Type: Incremental'], 'Full')
        self.assertEqual(backup_type, False)

    def test71_check_backup_type(self):
        backup_type = check_backup_type(['foo'], 'Full')
        self.assertEqual(backup_type, False)

    def test72_check_backup_type(self):
        backup_type = check_backup_type(['Backup Type: FullQ'], 'Full')
        self.assertEqual(backup_type, False)

    def test73_get_timestamp_val(self):
        ts_key = get_timestamp_val(['Timestamp Key: 01234567891234'])
        self.assertEqual(ts_key, '01234567891234')

    def test74_get_timestamp_val(self):
        ts_key = get_timestamp_val(['Time: 00000'])
        self.assertEqual(ts_key, None)

    def test75_get_timestamp_val(self):
        with self.assertRaisesRegexp(Exception, 'Invalid timestamp value found in report_file'):
            get_timestamp_val(['Timestamp Key: '])

    def test76_get_dump_dirs(self):
        dump_dirs = ['20121212']
        self.create_backup_dirs(dump_dirs=dump_dirs)
        expected_output = [os.path.join(os.getcwd(), 'db_dumps', d) for d in dump_dirs]
        try:
            ddir = get_dump_dirs(os.getcwd())
            self.assertEqual(ddir, expected_output)
        finally:
            self.remove_backup_dirs(dump_dirs=dump_dirs)

    def test77_get_dump_dirs(self):
        dump_dir_list = ['20121212', '20121213', '20121214'] 
        expected_output = [os.path.join(os.getcwd(), 'db_dumps', d) for d in dump_dir_list]
        self.create_backup_dirs(dump_dirs=dump_dir_list)
        try:
            ddir = get_dump_dirs(os.getcwd())
            self.assertEqual(ddir.sort(), expected_output.sort())
        finally:
            self.remove_backup_dirs(dump_dirs=dump_dir_list)

    def test78_get_dump_dirs(self):
        dump_dir_list = [] 
        self.create_backup_dirs(dump_dirs=dump_dir_list)
        try:
            self.assertEquals([], get_dump_dirs(os.getcwd()))
        finally:
            self.remove_backup_dirs(dump_dirs=dump_dir_list)

    def test79_get_dump_dirs(self):
        dump_dir_list = ['2012120a', '201212121', 'abcde'] 
        self.create_backup_dirs(dump_dirs=dump_dir_list)
        try:
            self.assertEquals([], get_dump_dirs(os.getcwd()))
        finally:
            self.remove_backup_dirs(dump_dirs=dump_dir_list)

    def test80_get_dump_dirs(self):
        dump_dir_list = ['11111111']
        expected_output = [os.path.join(os.getcwd(), 'db_dumps', d) for d in dump_dir_list]
        self.create_backup_dirs(dump_dirs=dump_dir_list)

        # this file should not get picked up
        fn = os.path.join(os.getcwd(), 'db_dumps', '20121201')
        with open(fn, 'w') as fd:
            fd.write('hello world')

        try:
            ddir = get_dump_dirs(os.getcwd())
            self.assertEqual(ddir, expected_output)
        finally:
            self.remove_backup_dirs(dump_dirs=dump_dir_list)
            os.remove(fn)

    def test81_get_dump_dirs(self):
        dump_dirs = None
        self.create_backup_dirs(dump_dirs=None)
        try:
            self.assertEquals([], get_dump_dirs(os.getcwd()))
        finally:
            self.remove_backup_dirs(dump_dirs=None)

    def test82_get_dump_dirs(self):
        self.assertEquals([], get_dump_dirs('abcdef'))

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=['gp_cdatabase_1_1_20121212111111', 'gp_dump_20121212000000.rpt', 'gp_cdatabase_1_1_20121212000001'])
    @patch('gppylib.operations.backup_utils.get_full_ts_from_report_file', return_value=['000000'])
    def test83_get_latest_full_dump_timestamp(self, mock1, mock2, mock3):
        expected_output = ['000000']
        ts = get_latest_full_dump_timestamp('testdb', '/foo/db_dumps')
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=[])
    def test84_get_latest_full_dump_timestamp(self, mock1):
        with self.assertRaisesRegexp(Exception, 'No full backup found for incremental'):
            get_latest_full_dump_timestamp('testdb', '/foo/db_dumps')

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=[])
    def test85_get_latest_full_dump_timestamp(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'Invalid None param to get_latest_full_dump_timestamp'):
            get_latest_full_dump_timestamp('testdb', None)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=['gp_cdatabase_1_1_2012121211111', 'gp_cdatabase_1_1_201212120000010', 'gp_cdatabase_1_1_2012121a111111'])
    def test86_get_latest_full_dump_timestamp(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'No full backup found for incremental'):
            ts = get_latest_full_dump_timestamp('testdb', '/tmp/foo')

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=['gp_cdatabase_1_1_20121212111111', 'gp_dump_20121212000000.rpt.bk', 'gp_cdatabase_1_1_20121212000001'])
    def test87_get_latest_full_dump_timestamp(self, mock1, mock2):
        with self.assertRaisesRegexp(Exception, 'No full backup found for incremental'):
            ts = get_latest_full_dump_timestamp('testdb', '/tmp/foo')

    def test88_generate_pgstatlastoperation_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030_last_operation'
        output = generate_pgstatlastoperation_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test89_generate_ao_state_filename(self):
        master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '%s/db_dumps/20120731/gp_dump_20120731093030_ao_state_file' % master_data_dir
        output = generate_ao_state_filename(master_data_dir, backup_dir, timestamp)
        self.assertEqual(output, expected_output)
        
    def test90_generate_ao_state_filename(self):
        master_data_dir = None
        backup_dir = '/tmp'
        timestamp = '20120731093030'
        expected_output = '%s/db_dumps/20120731/gp_dump_20120731093030_ao_state_file' % backup_dir
        output = generate_ao_state_filename(master_data_dir, backup_dir, timestamp)
        self.assertEqual(output, expected_output)
        
    def test91_generate_co_state_filename(self):
        master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '%s/db_dumps/20120731/gp_dump_20120731093030_co_state_file' % master_data_dir
        output = generate_co_state_filename(master_data_dir, backup_dir, timestamp)
        self.assertEqual(output, expected_output)
        
    def test92_generate_co_state_filename(self):
        master_data_dir = None
        backup_dir = '/tmp'
        timestamp = '20120731093030'
        expected_output = '%s/db_dumps/20120731/gp_dump_20120731093030_co_state_file' % backup_dir
        output = generate_co_state_filename(master_data_dir, backup_dir, timestamp)
        self.assertEqual(output, expected_output)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=[])
    @patch('gppylib.operations.backup_utils.get_latest_report_in_dir', return_value=[])
    def test93_get_latest_report_timestamp(self, mock1, mock2):
        bdir = '/foo'
        result = get_latest_report_timestamp(bdir)
        self.assertEquals(result, None)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20120930'])
    @patch('gppylib.operations.backup_utils.get_latest_report_in_dir', return_value=None)
    def test94_get_latest_report_timestamp(self, mock1, mock2):
        bdir = '/foo'
        result = get_latest_report_timestamp(bdir)
        self.assertEquals(result, None)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20120930'])
    @patch('gppylib.operations.backup_utils.get_latest_report_in_dir', return_value='20120930093000')
    def test95_get_latest_report_timestamp(self, mock1, mock2):
        bdir = '/foo'
        result = get_latest_report_timestamp(bdir)
        self.assertEquals(result, '20120930093000')

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20120930', '20120929'])
    @patch('gppylib.operations.backup_utils.get_latest_report_in_dir', side_effect=[None, '20120929093000'])
    def test96_get_latest_report_timestamp(self, mock1, mock2):
        bdir = '/foo'
        result = get_latest_report_timestamp(bdir)
        self.assertEquals(result, '20120929093000')

    @patch('os.listdir', return_value=[])
    def test97_get_latest_report_in_dir(self, mock1):
        bdir = '/foo'
        result = get_latest_report_in_dir(bdir)
        self.assertEquals(result, None)

    @patch('os.listdir', return_value=['gp_dump_20130125140013.rpt', 'gp_dump_20140125140013.FOO'])
    def test98_get_latest_report_in_dir(self, mock1):
        bdir = '/foo'
        result = get_latest_report_in_dir(bdir)
        self.assertEquals(result, '20130125140013')

    @patch('os.listdir', return_value=['gp_dump_20130125140013.rpt', 'gp_dump_20140125140013.rpt'])
    def test99_get_latest_report_in_dir(self, mock1):
        bdir = '/foo'
        result = get_latest_report_in_dir(bdir)
        self.assertEquals(result, '20140125140013')

    @patch('os.listdir', return_value=['gp_dump_20140125140013.rpt', 'gp_dump_20130125140013.rpt'])
    def test100(self, mock1):
        bdir = '/foo'
        result = get_latest_report_in_dir(bdir)
        self.assertEquals(result, '20140125140013')

    def test_create_temp_file_with_tables_00(self):
        dirty_tables = ['public.t1', 'public.t2', 'pepper.t3']
        dirty_file = create_temp_file_with_tables(dirty_tables)
        self.assertTrue(os.path.basename(dirty_file).startswith('table_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_create_temp_file_with_tables_01(self):
        dirty_tables = ['']
        dirty_file = create_temp_file_with_tables(dirty_tables)
        self.assertTrue(os.path.basename(dirty_file).startswith('table_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_create_temp_file_with_tables_02(self):
        dirty_tables = ['']
        dirty_file = create_temp_file_with_tables(dirty_tables)
        self.assertTrue(os.path.basename(dirty_file).startswith('table_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_create_temp_file_from_list_00(self):
        dirty_tables = ['public.t1', 'public.t2', 'pepper.t3']
        dirty_file = create_temp_file_from_list(dirty_tables, 'dirty_hackup_list_')
        self.assertTrue(os.path.basename(dirty_file).startswith('dirty_hackup_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_create_temp_file_from_list_01(self):
        dirty_tables = ['']
        dirty_file = create_temp_file_from_list(dirty_tables, 'dirty_hackup_list_')
        self.assertTrue(os.path.basename(dirty_file).startswith('dirty_hackup_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_create_temp_file_from_list_02(self):
        dirty_tables = ['']
        dirty_file = create_temp_file_from_list(dirty_tables, 'dirty_hackup_list_')
        self.assertTrue(os.path.basename(dirty_file).startswith('dirty_hackup_list'))
        self.assertTrue(os.path.exists(dirty_file))
        content = get_lines_from_file(dirty_file) 
        self.assertEqual(dirty_tables, content)
        os.remove(dirty_file)

    def test_get_timestamp_from_increments_filename_00(self):
        fname = '/data/foo/db_dumps/20130207/gp_dump_20130207133000_increments'
        ts = get_timestamp_from_increments_filename(fname)
        self.assertEquals(ts, '20130207133000')

    def test_get_timestamp_from_increments_filename_01(self):
        fname = '/data/foo/db_dumps/20130207/gpdump_20130207133000_increments'
        with self.assertRaisesRegexp(Exception, 'Invalid increments file'):
            get_timestamp_from_increments_filename(fname)

    @patch('glob.glob', return_value=[])
    def test_get_full_timestamp_for_incremental_00(self, m1):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        self.assertEquals(full_ts, None)


    @patch('glob.glob', return_value=['foo'])
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=[])
    def test_get_full_timestamp_for_incremental_01(self, m1, m2):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        self.assertEquals(full_ts, None)


    @patch('glob.glob', return_value=['/tmp/db_dumps/20130207/gp_dump_20130207093000_increments'])
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['20130207133001', '20130207133000'])
    def test_get_full_timestamp_for_incremental_02(self, m1, m2):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        self.assertEquals(full_ts, '20130207093000')

    def test_check_funny_chars_in_tablenames_00(self):
        tablenames = ['hello, world', 'correct']
        with self.assertRaisesRegexp(Exception, 'Tablename has an invalid character'):
            check_funny_chars_in_tablenames(tablenames)

    def test_check_funny_chars_in_tablenames_01(self): 
        tablenames = ['hello\nworld', 'propertablename']
        with self.assertRaisesRegexp(Exception, 'Tablename has an invalid character'):
            check_funny_chars_in_tablenames(tablenames)

    def test_check_funny_chars_in_tablenames_02(self): 
        tablenames = ['hello:world', 'propertablename']
        with self.assertRaisesRegexp(Exception, 'Tablename has an invalid character'):
            check_funny_chars_in_tablenames(tablenames)

    def test_check_funny_chars_in_tablenames_03(self):
        tablenames = ['helloworld', 'propertablename']
        check_funny_chars_in_tablenames(tablenames) #should not raise an exception

    def test_expand_partition_tables_00(self):
        self.assertEqual(expand_partition_tables('foo', None), None)

    @patch('gppylib.operations.backup_utils.dbconn.execSQL')
    @patch('gppylib.operations.backup_utils.dbconn.connect')
    @patch('pygresql.pgdb.pgdbCursor.fetchall', return_value=[['public', 'tl1'], ['public', 'tl2']])
    def test_expand_partition_tables_01(self, mock1, mock2, mock3):
        dbname = 'foo'
        restore_tables = ['public.t1', 'public.t2']
        expected_output = ['public.tl1', 'public.tl2', 'public.t2']
        result = expand_partition_tables(dbname, restore_tables)
        self.assertEqual(result.sort(), expected_output.sort())

    @patch('gppylib.operations.backup_utils.dbconn.execSQL')
    @patch('gppylib.operations.backup_utils.dbconn.connect')
    @patch('pygresql.pgdb.pgdbCursor.fetchall', return_value=[])
    def test_expand_partition_tables_02(self, mock1, mock2, mock3):
        dbname = 'foo'
        restore_tables = ['public.t1', 'public.t2']
        expected_output = ['public.t1', 'public.t2']
        result = expand_partition_tables(dbname, restore_tables)
        self.assertEqual(result.sort(), expected_output.sort())

    def test_populate_filter_tables_00(self):
        table = 'public.t1'
        rows = [['public', 't1'], ['public', 't2'], ['public', 't3']]
        non_partition_tables = []
        partition_leaves = []
        self.assertEqual(populate_filter_tables(table, rows, non_partition_tables, partition_leaves),
                            (([], ['public.t1', 'public.t2', 'public.t3'])))

    def test_populate_filter_tables_01(self):
        table = 'public.t1'
        rows = []
        non_partition_tables = []
        partition_leaves = []
        self.assertEqual(populate_filter_tables(table, rows, non_partition_tables, partition_leaves),
                            ((['public.t1'], [])))

    @patch('gppylib.operations.backup_utils.expand_partition_tables', return_value=['public.t1_p1', 'public.t1_p2', 'public.t1_p3', 'public.t2', 'public.t3'])
    def test_expand_partitions_and_populate_filter_file01(self, mock):
        dbname = 'testdb'
        partition_list = ['public.t1', 'public.t2', 'public.t3']
        file_prefix = 'include_dump_tables_file'
        expected_output = ['public.t2', 'public.t3', 'public.t1', 'public.t1_p1', 'public.t1_p2', 'public.t1_p3']
        result = expand_partitions_and_populate_filter_file(dbname, partition_list, file_prefix)
        self.assertTrue(os.path.basename(result).startswith(file_prefix))
        self.assertTrue(os.path.exists(result))
        contents = get_lines_from_file(result)
        self.assertEqual(contents.sort(), expected_output.sort())
        os.remove(result)

    @patch('gppylib.operations.backup_utils.expand_partition_tables', return_value=['public.t1', 'public.t2', 'public.t3'])
    def test_expand_partitions_and_populate_filter_file02(self, mock):
        dbname = 'testdb'
        partition_list = ['public.t1', 'public.t2', 'public.t3']
        file_prefix = 'exclude_dump_tables_file'
        result = expand_partitions_and_populate_filter_file(dbname, partition_list, file_prefix)
        self.assertTrue(os.path.basename(result).startswith(file_prefix))
        self.assertTrue(os.path.exists(result))
        contents = get_lines_from_file(result)
        self.assertEqual(contents.sort(), partition_list.sort())
        os.remove(result)

    @patch('gppylib.operations.backup_utils.expand_partition_tables', return_value=[])
    def test_expand_partitions_and_populate_filter_file03(self, mock):
        dbname = 'testdb'
        partition_list = []
        file_prefix = 'exclude_dump_tables_file'
        result = expand_partitions_and_populate_filter_file(dbname, partition_list, file_prefix)
        self.assertTrue(os.path.basename(result).startswith(file_prefix))
        self.assertTrue(os.path.exists(result))
        contents = get_lines_from_file(result)
        self.assertEqual(contents.sort(), partition_list.sort())
        os.remove(result)

    def test01_generate_files_filename(self):
        master_data_dir = '/data'
        backup_dir = '/backup'
        timestamp = '20120731093030'
        expected_output = '/backup/db_dumps/20120731/gp_dump_20120731093030_regular_files'
        output = generate_files_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test02_generate_files_filename(self):
        master_data_dir = None
        backup_dir = '/backup'
        timestamp = '20120731093030'
        expected_output = '/backup/db_dumps/20120731/gp_dump_20120731093030_regular_files'
        output = generate_files_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test03_generate_files_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030_regular_files'
        output = generate_files_filename(master_data_dir, backup_dir, timestamp)

    def test01_generate_pipes_filename(self):
        master_data_dir = '/data'
        backup_dir = '/backup'
        timestamp = '20120731093030'
        expected_output = '/backup/db_dumps/20120731/gp_dump_20120731093030_pipes'
        output = generate_pipes_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test02_generate_pipes_filename(self):
        master_data_dir = None
        backup_dir = '/backup'
        timestamp = '20120731093030'
        expected_output = '/backup/db_dumps/20120731/gp_dump_20120731093030_pipes'
        output = generate_pipes_filename(master_data_dir, backup_dir, timestamp)
        self.assertEquals(output, expected_output)

    def test03_generate_pipes_filename(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/gp_dump_20120731093030_pipes'
        output = generate_pipes_filename(master_data_dir, backup_dir, timestamp)

    def test00_generate_report_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/data/db_dumps/20120731/%sgp_dump_20120731093030.rpt' % backup_utils.dump_prefix
        output = generate_report_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)
 
    def test00_generate_ao_state_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%s/db_dumps/20120731/%sgp_dump_20120731093030_ao_state_file' % (master_data_dir, backup_utils.dump_prefix)
        output = generate_ao_state_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)
        
    def test00_generate_co_state_filename_with_prefix(self):
        master_data_dir = os.environ.get('MASTER_DATA_DIRECTORY')
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%s/db_dumps/20120731/%sgp_dump_20120731093030_co_state_file' % (master_data_dir, backup_utils.dump_prefix)
        output = generate_co_state_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEqual(output, expected_output)

    def test00_generate_increments_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/data/db_dumps/20120731/%sgp_dump_20120731093030_increments' % backup_utils.dump_prefix
        output = generate_increments_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_pgstatlastoperation_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/data/db_dumps/20120731/%sgp_dump_20120731093030_last_operation' % backup_utils.dump_prefix
        output = generate_pgstatlastoperation_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_dirtytable_filename_with_prefix(self):
        mdd = '/data'
        backup_dir = None
        timestamp = '20121204090000'
        backup_utils.dump_prefix = 'foo_'
        expected = '/data/db_dumps/20121204/%sgp_dump_20121204090000_dirty_list' % backup_utils.dump_prefix
        result = generate_dirtytable_filename(mdd, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(expected, result)

    def test00_generate_plan_filename_with_prefix(self):
        mdd = '/data'
        timestamp = '20121204090000'
        backup_utils.dump_prefix = 'foo_'
        expected = '/data/db_dumps/20121204/%sgp_restore_20121204090000_plan' % backup_utils.dump_prefix
        backup_dir = None
        result = generate_plan_filename(mdd, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(expected, result)

    def test00_generate_metadata_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/data/db_dumps/20120731/%sgp_dump_1_1_20120731093030.gz' % backup_utils.dump_prefix
        output = generate_metadata_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_partition_list_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/data/db_dumps/20120731/%sgp_dump_20120731093030_table_list' % backup_utils.dump_prefix
        output = generate_partition_list_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test01_generate_files_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = '/backup'
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/backup/db_dumps/20120731/%sgp_dump_20120731093030_regular_files' % backup_utils.dump_prefix
        output = generate_files_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test01_generate_pipes_filename_with_prefix(self):
        master_data_dir = '/data'
        backup_dir = '/backup'
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '/backup/db_dumps/20120731/%sgp_dump_20120731093030_pipes' % backup_utils.dump_prefix
        output = generate_pipes_filename(master_data_dir, backup_dir, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    @patch('os.listdir', return_value=['bar_gp_dump_20140125140013.rpt', 'foo_gp_dump_20130125140013.rpt'])
    def test00_get_latest_report_in_dir_with_prefix(self, mock1):
        bdir = '/foo'
        backup_utils.dump_prefix = 'foo_'
        result = get_latest_report_in_dir(bdir)
        backup_utils.dump_prefix = ''
        self.assertEquals(result, '20130125140013')
    
    @patch('os.listdir', return_value=['gp_dump_20130125140013.rpt'])
    def test01_get_latest_report_in_dir_with_prefix(self, mock1):
        bdir = '/foo'
        backup_utils.dump_prefix = 'foo_'
        result = get_latest_report_in_dir(bdir)
        backup_utils.dump_prefix = ''
        self.assertEquals(result, None)

    @patch('glob.glob', return_value=[])
    def test_get_full_timestamp_for_incremental_with_prefix_00(self, m1):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        backup_utils.dump_prefix = 'foo_'
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        backup_utils.dump_prefix = ''
        self.assertEquals(full_ts, None)


    @patch('glob.glob', return_value=['foo'])
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=[])
    def test_get_full_timestamp_for_incremental_with_prefix_01(self, m1, m2):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        backup_utils.dump_prefix = 'foo_'
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        backup_utils.dump_prefix = ''
        self.assertEquals(full_ts, None)


    @patch('glob.glob', return_value=['/tmp/db_dumps/20130207/foo_gp_dump_20130207093000_increments'])
    @patch('gppylib.operations.backup_utils.get_lines_from_file', return_value=['20130207133001', '20130207133000'])
    def test_get_full_timestamp_for_incremental_with_prefix_02(self, m1, m2):
        dbname = 'hulu'
        backup_dir = 'home'
        ts = '20130207133000' 
        backup_utils.dump_prefix = 'foo_'
        full_ts = get_full_timestamp_for_incremental(dbname, backup_dir, ts)
        backup_utils.dump_prefix = ''
        self.assertEquals(full_ts, '20130207093000')

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=['foo_gp_cdatabase_1_1_20121212111111', 'foo_gp_dump_20121212000000.rpt', 'foo_gp_cdatabase_1_1_20121212000001'])
    @patch('gppylib.operations.backup_utils.get_full_ts_from_report_file', return_value=['000000'])
    def test00_get_latest_full_dump_timestamp_with_prefix(self, mock1, mock2, mock3):
        expected_output = ['000000']
        backup_utils.dump_prefix = 'foo_'
        ts = get_latest_full_dump_timestamp('testdb', '/foo/db_dumps')
        backup_utils.dump_prefix = ''
        self.assertEqual(ts, expected_output)

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=[])
    def test01_get_latest_full_dump_timestamp_with_prefix(self, mock1):
        backup_utils.dump_prefix = 'foo_'
        with self.assertRaisesRegexp(Exception, 'No full backup found for incremental'):
            get_latest_full_dump_timestamp('testdb', '/foo/db_dumps')
        backup_utils.dump_prefix = ''

    @patch('gppylib.operations.backup_utils.get_dump_dirs', return_value=['20121212', '20121213', '20121214'])
    @patch('os.listdir', return_value=[])
    def test02_get_latest_full_dump_timestamp_with_prefix(self, mock1, mock2):
        backup_utils.dump_prefix = 'foo_'
        with self.assertRaisesRegexp(Exception, 'Invalid None param to get_latest_full_dump_timestamp'):
            get_latest_full_dump_timestamp('testdb', None)
        backup_utils.dump_prefix = ''

    def test00_convert_reportfilename_to_cdatabasefilename_with_prefix(self):
        report_file = '/tmp/foo/foo/bar_gp_dump_20130104133924.rpt'
        expected_output = '/tmp/foo/foo/bar_gp_cdatabase_1_1_20130104133924'
        backup_utils.dump_prefix = 'bar_'
        cdatabase_file = convert_reportfilename_to_cdatabasefilename(report_file)
        backup_utils.dump_prefix = ''
        self.assertEquals(expected_output, cdatabase_file)

    def test00_generate_master_config_filename(self):
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_master_config_files_%s.tar' % (backup_utils.dump_prefix, timestamp)
        output = generate_master_config_filename(timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_segment_config_filename(self):
        timestamp = '20120731093030'
        backup_utils.dump_prefix = 'foo_'
        segId = 2
        expected_output = '%sgp_segment_config_files_0_%d_%s.tar' % (backup_utils.dump_prefix, segId, timestamp)
        output = generate_segment_config_filename(segId, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_global_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_global_1_1_' % (backup_utils.dump_prefix)
        output = generate_global_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_master_dbdump_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_dump_1_1_' % (backup_utils.dump_prefix)
        output = generate_master_dbdump_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_master_status_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_dump_status_1_1_' % (backup_utils.dump_prefix)
        output = generate_master_status_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_seg_status_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_dump_status_0_' % (backup_utils.dump_prefix)
        output = generate_seg_status_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)

    def test00_generate_seg_dbdump_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_dump_0_' % (backup_utils.dump_prefix)
        output = generate_seg_dbdump_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)
 
    def test00_generate_dbdump_prefix(self):
        backup_utils.dump_prefix = 'foo_'
        expected_output = '%sgp_dump_' % (backup_utils.dump_prefix)
        output = generate_dbdump_prefix()
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)
 
    def test00_generate_createdb_filename(self):
        backup_utils.dump_prefix = 'foo_'
        master_datadir = '/data'
        backup_dir = None
        timestamp = '20120731093030'
        expected_output = '/data/db_dumps/20120731/%sgp_cdatabase_1_1_%s' % (backup_utils.dump_prefix, timestamp)
        output = generate_createdb_filename(master_datadir, None, timestamp)
        backup_utils.dump_prefix = ''
        self.assertEquals(output, expected_output)
 
