#!/usr/bin/env python
# coding: utf-8
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#

import unittest2 as unittest
import tempfile, os, shutil
from gppylib.operations.restore import RestoreDatabase, create_restore_plan, get_plan_file_contents, \
        get_restore_tables_from_table_file, restore_incremental_data_only, write_to_plan_file, \
        validate_tablenames, create_plan_file_contents, GetDbName, get_dirty_table_file_contents, \
        get_incremental_restore_timestamps, get_partition_list, get_restore_dir, is_begin_incremental_run, \
        is_incremental_restore, get_restore_table_list, validate_restore_tables_list, \
        update_ao_stat_func, update_ao_statistics, _build_gpdbrestore_cmd_line
import gppylib.operations.backup_utils as backup_utils


from gppylib.commands.base import ExecutionError
from gppylib.mainUtils import ExceptionNoStackTraceNeeded
from mock import patch, MagicMock

class restoreTestCase(unittest.TestCase):
    def setUp(self):
        self.restore = RestoreDatabase(restore_timestamp = '20121212121212',
                            no_analyze = True,
                            drop_db = True,
                            restore_global = False,
                            master_datadir = 'foo', 
                            backup_dir = None,
                            master_port = 0,
                            dump_dir = "db_dumps",
                            ddboost = False,
                            no_plan = False,
                            restore_tables = None,
                            batch_default=64,
                            no_ao_stats = False)

    def create_backup_dirs(self, top_dir=os.getcwd(), dump_dirs=[]):
        if dump_dirs is None:
            return

        for dump_dir in dump_dirs:
            backup_dir = os.path.join(top_dir, 'db_dumps', dump_dir)
            if not os.path.isdir(backup_dir):
                os.makedirs(backup_dir)
                if not os.path.isdir(backup_dir):
                    raise Exception('Failed to create directory %s' % backup_dir)
   
    def remove_backup_dirs(self, top_dir=os.getcwd(), dump_dirs=[]):
        if dump_dirs is None:
            return 

        for dump_dir in dump_dirs:
            backup_dir = os.path.join(top_dir, 'db_dumps', dump_dir)
            shutil.rmtree(backup_dir)
            if os.path.isdir(backup_dir):
                raise Exception('Failed to remove directory %s' % backup_dir)

    def test_GetDbName_1(self):
        """ Basic test """
        with tempfile.NamedTemporaryFile() as f:
            f.write("""
--
-- Database creation
--

CREATE DATABASE monkey WITH TEMPLATE = template0 ENCODING = 'UTF8' OWNER = thisguy;
""")
            f.flush()
            self.assertTrue(GetDbName(f.name).run() == "monkey")

    def test_GetDbName_2(self):
        """ Verify that GetDbName looks no further than 50 lines. """
        with tempfile.NamedTemporaryFile() as f:
            for i in range(0, 50):
                f.write("crap\n")
            f.write("CREATE DATABASE monkey")            
            f.flush()
            try:
                GetDbName(f.name).run()
            except GetDbName.DbNameGiveUp, e:
                return
            self.fail("DbNameGiveUp should have been raised.")

    def test_GetDbName_3(self):
        """ Verify that GetDbName fails  when cdatabase file ends prematurely. """
        with tempfile.NamedTemporaryFile() as f:
            f.write("this is the whole file")
            f.flush()
            try:
                GetDbName(f.name).run()
            except GetDbName.DbNameNotFound, e:
                return
            self.fail("DbNameNotFound should have been raised.")


    @patch('gppylib.operations.restore.RestoreDatabase._process_createdb', side_effect=ExceptionNoStackTraceNeeded('Failed to create database'))
    @patch('time.sleep')
    def test_multitry_createdb_1(self, mock1, mock2):
        r = RestoreDatabase('20121219', True, True, False, 'FOO', None, 1234, False, False, None, None, 'db_dumps', False)
        self.assertRaises(ExceptionNoStackTraceNeeded, r._multitry_createdb, '20121219', 'fullbkdb', 'FOO', None, 1234)

    @patch('gppylib.operations.restore.RestoreDatabase._process_createdb')
    def test_multitry_createdb_2(self, mock):
        r = RestoreDatabase('20121219', True, True, False, 'FOO', None, 1234, False, False, None, None, 'db_dumps',  False)
        r._multitry_createdb('20121219', 'fullbkdb', 'FOO', None, 1234)

    @patch('gppylib.operations.restore.get_partition_list', return_value=[('public', 't1'), ('public', 't2'), ('public', 't3')])
    @patch('gppylib.operations.restore.get_full_timestamp_for_incremental', return_value='123456789')
    @patch('gppylib.operations.restore.get_incremental_restore_timestamps', return_value=['20121212121212', '20121212121211'])
    @patch('gppylib.operations.restore.get_dirty_table_file_contents', return_value=['public.t1', 'public.t2'])
    def test_restore_plan_file_00(self, mock1, mock2, mock3, mock4):
        master_datadir = 'foo' 
        db_timestamp = '01234567891234'
        dbname = 'bkdb'
        ddboost = False
        backup_dir = None
        self.create_backup_dirs(master_datadir, [db_timestamp[0:8]])
        plan_file = create_restore_plan(master_datadir, backup_dir, db_timestamp, dbname, ddboost)
        self.assertTrue(os.path.isfile(plan_file))
        self.remove_backup_dirs(master_datadir, [db_timestamp[0:8]])

    @patch('gppylib.operations.restore.get_partition_list', return_value=[])
    @patch('gppylib.operations.restore.get_full_timestamp_for_incremental', return_value='123456789')
    @patch('gppylib.operations.restore.get_incremental_restore_timestamps', return_value=['20121212121212', '20121212121211'])
    @patch('gppylib.operations.restore.get_dirty_table_file_contents', return_value=['public.t1', 'public.t2'])
    def test_restore_plan_file_01(self, mock1, mock2, mock3, mock4):
        master_datadir = 'foo' 
        db_timestamp = '01234567891234'
        dbname = 'bkdb'
        ddboost = False
        backup_dir = None
        self.create_backup_dirs(master_datadir, [db_timestamp[0:8]])
        plan_file = create_restore_plan(master_datadir, backup_dir, db_timestamp, dbname, ddboost)
        self.assertTrue(os.path.isfile(plan_file))
        self.remove_backup_dirs(master_datadir, [db_timestamp[0:8]])


    @patch('gppylib.operations.restore.get_partition_list', return_value=[])
    @patch('gppylib.operations.restore.get_full_timestamp_for_incremental', return_value=None)
    def test_restore_plan_file_02(self, mock1, mock2):
        master_datadir = 'foo' 
        db_timestamp = '01234567891234'
        dbname = 'bkdb'
        ddboost = False
        backup_dir = None
        self.create_backup_dirs(master_datadir, [db_timestamp[0:8]])
        with self.assertRaisesRegexp(Exception, 'Could not locate fullbackup associated with ts'):
            create_restore_plan(master_datadir, backup_dir, db_timestamp, dbname, ddboost)
        self.remove_backup_dirs(master_datadir, [db_timestamp[0:8]])


    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['20121212121210', '20121212121209', '20121212121208', '20121212121207', '20121212121206', '20121212121205', '20121212121204', '20121212121203', '20121212121202', '20121212121201'])
    def test_get_incremental_restore_timestamps_00(self, mock):
        master_data_dir = 'foo'
        latest_full_timestamp = '20121212121201'
        restore_timestamp = '20121212121205'
        backup_dir = None
        increments = get_incremental_restore_timestamps(master_data_dir, backup_dir, latest_full_timestamp, restore_timestamp)
        self.assertEqual(increments, ['20121212121205', '20121212121204', '20121212121203', '20121212121202', '20121212121201']) 

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['20121212121210', '20121212121209', '20121212121208', '20121212121207', '20121212121206', '20121212121205', '20121212121204', '20121212121203', '20121212121202', '20121212121201'])
    def test_get_incremental_restore_timestamps_01(self, mock):
        master_data_dir = 'foo'
        latest_full_timestamp = '20121212121201'
        restore_timestamp = '20121212121210'
        backup_dir = None
        increments = get_incremental_restore_timestamps(master_data_dir, backup_dir, latest_full_timestamp, restore_timestamp)
        self.assertEqual(increments,['20121212121210', '20121212121209', '20121212121208', '20121212121207', '20121212121206', '20121212121205', '20121212121204', '20121212121203', '20121212121202', '20121212121201']) 
        
    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['20121212121210', '20121212121209', '20121212121208', '20121212121207', '20121212121206', '20121212121205', '20121212121204', '20121212121203', '20121212121202', '20121212121201'])
    def test_get_incremental_restore_timestamps_02(self, mock):
        master_data_dir = 'foo'
        latest_full_timestamp = '20121212121201'
        restore_timestamp = '20121212121200'
        backup_dir = None
        increments = get_incremental_restore_timestamps(master_data_dir, backup_dir, latest_full_timestamp, restore_timestamp)
        self.assertEqual(increments,[]) 

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=[])
    def test_get_incremental_restore_timestamps_03(self, mock):
        master_data_dir = 'foo'
        latest_full_timestamp = '20121212121201'
        restore_timestamp = '20121212121200'
        backup_dir = None
        increments = get_incremental_restore_timestamps(master_data_dir, backup_dir, latest_full_timestamp, restore_timestamp)
        self.assertEqual(increments,[]) 

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['public.t1', 'public.t2', 'public.t3'])
    def test_get_dirty_table_file_contents_00(self, mock):
        master_datadir = 'foo' 
        backup_dir = None
        timestamp_key = '20121212121212'
        dirty_tables = get_dirty_table_file_contents(master_datadir, backup_dir, timestamp_key)
        self.assertEqual(dirty_tables, ['public.t1', 'public.t2', 'public.t3'])

    @patch('gppylib.operations.restore.get_lines_from_file', side_effect=[['public.t1'], ['public.t1', 'public.t2', 'public.t3'], ['public.t2', 'public.t4']])
    def test_create_plan_file_contents_00(self, mock):
        master_datadir = 'foo' 
        table_set_from_metadata_file = ['public.t1', 'public.t2', 'public.t3', 'public.t4'] 
        incremental_restore_timestamps = ['20121212121213', '20121212121212', '20121212121211']
        latest_full_timestamp = '20121212121210'
        backup_dir = None
        expected_output = {'20121212121213': ['public.t1'], '20121212121212': ['public.t2', 'public.t3'], '20121212121211': ['public.t4'], '20121212121210': []}
        file_contents = create_plan_file_contents(master_datadir, 
                                                  backup_dir,
                                                  table_set_from_metadata_file, 
                                                  incremental_restore_timestamps,
                                                  latest_full_timestamp)
        self.assertEqual(file_contents, expected_output)
 
    def test_create_plan_file_contents_01(self):
        master_datadir = 'foo' 
        table_set_from_metadata_file = ['public.t1', 'public.t2', 'public.t3', 'public.t4'] 
        incremental_restore_timestamps = []
        latest_full_timestamp = '20121212121210'
        backup_dir = None
        expected_output = {'20121212121210': ['public.t1', 'public.t2', 'public.t3', 'public.t4']}
        file_contents = create_plan_file_contents(master_datadir, 
                                                  backup_dir,
                                                  table_set_from_metadata_file, 
                                                  incremental_restore_timestamps,
                                                  latest_full_timestamp)
        self.assertEqual(file_contents, expected_output)
 
    @patch('gppylib.operations.restore.get_lines_from_file', side_effect=[['public.t1'], ['public.t1', 'public.t2', 'public.t3'], ['public.t2', 'public.t4']])
    def test_create_plan_file_contents_02(self, mock):
        master_datadir = 'foo' 
        table_set_from_metadata_file = []
        incremental_restore_timestamps = ['20121212121213', '20121212121212', '20121212121211']
        latest_full_timestamp = '20121212121210'
        backup_dir = None
        expected_output = {'20121212121212': [], '20121212121213': [], '20121212121211': [], '20121212121210': []}
        file_contents = create_plan_file_contents(master_datadir, 
                                                  backup_dir,
                                                  table_set_from_metadata_file, 
                                                  incremental_restore_timestamps,
                                                  latest_full_timestamp)
        self.assertEqual(file_contents, expected_output)

    @patch('gppylib.operations.restore.write_lines_to_file')
    @patch('gppylib.operations.restore.verify_lines_in_file')
    def test_write_to_plan_file_00(self, mock1, mock2):
        plan_file = 'blah' 
        plan_file_contents = {'20121212121213': ['public.t1'],
                              '20121212121212': ['public.t2', 'public.t3'],
                              '20121212121211': ['public.t4']}

        expected_output = ['20121212121213:public.t1',
                           '20121212121212:public.t2,public.t3',
                           '20121212121211:public.t4']

        file_contents = write_to_plan_file(plan_file_contents, plan_file)
        self.assertEqual(expected_output, file_contents)

    @patch('gppylib.operations.restore.write_lines_to_file')
    @patch('gppylib.operations.restore.verify_lines_in_file')
    def test_write_to_plan_file_01(self, mock1, mock2):
        plan_file = 'blah' 
        plan_file_contents = {}
        expected_output = []
        file_contents = write_to_plan_file(plan_file_contents, plan_file)
        self.assertEqual(expected_output, file_contents)

    @patch('gppylib.operations.restore.write_lines_to_file')
    @patch('gppylib.operations.restore.verify_lines_in_file')
    def test_write_to_plan_file_02(self, mock1, mock2):
        plan_file = None
        plan_file_contents = {}
        with self.assertRaisesRegexp(Exception, 'Invalid plan file .*'):
            write_to_plan_file(plan_file_contents, plan_file) 

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['public.t1', 'public.t2'])
    def test_get_partition_list_00(self, mock):
        master_datadir = 'foo' 
        backup_dir = None
        timestamp = '20121212121212'
        partition_list = get_partition_list(master_datadir, backup_dir, timestamp)
        self.assertEqual(partition_list, [('public', 't1'), ('public', 't2')])
        
    @patch('gppylib.operations.restore.get_lines_from_file', return_value=[])
    def test_get_partition_list_01(self, mock):
        master_datadir = 'foo' 
        backup_dir = None
        timestamp = '20121212121212'
        partition_list = get_partition_list(master_datadir, backup_dir, timestamp)
        self.assertEqual(partition_list, [])

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['Backup Type: Incremental'])
    @patch('os.path.isfile', return_value=True)
    def test_is_incremental_restore_00(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        self.assertTrue(is_incremental_restore(master_datadir, backup_dir, timestamp))

    @patch('gppylib.operations.restore.get_lines_from_file')
    @patch('gppylib.operations.restore.check_backup_type', return_value=True)
    @patch('os.path.isfile', return_value=True)
    def test_is_incremental_restore_01(self, mock1, mock2, mock3):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = '/foo'
        self.assertTrue(is_incremental_restore(master_datadir, backup_dir, timestamp))

    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['Backup Type: Full'])
    def test_is_incremental_restore_02(self, mock1):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        self.assertFalse(is_incremental_restore(master_datadir, backup_dir, timestamp))

    @patch('gppylib.operations.restore.get_lines_from_file')
    @patch('gppylib.operations.restore.check_backup_type', return_value=False)
    def test_is_incremental_restore_03(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        self.assertFalse(is_incremental_restore(master_datadir, backup_dir, timestamp))

    def test_is_incremental_restore_04(self):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        self.assertFalse(is_incremental_restore(master_datadir, backup_dir, timestamp))

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_schema_only_restore_line_00(self, mock1, mock2):
        master_datadir = 'foo' 
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = False 
        master_port = '5432'
        table_filter_file = None
        metadata_file = os.path.join(master_datadir, 'db_dumps', restore_timestamp[0:8], 'gp_dump_1_1_%s.gz' % restore_timestamp)
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p -s %s -d bkdb' % metadata_file

        restore_line = self.restore._build_schema_only_restore_line(restore_timestamp, restore_db, compress, master_port, metadata_file, table_filter_file)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_schema_only_restore_line_01(self, mock1, mock2):
        master_datadir = 'foo' 
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        table_filter_file = None
        metadata_file = os.path.join(master_datadir, 'db_dumps', restore_timestamp[0:8], 'gp_dump_1_1_%s.gz' % restore_timestamp)
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p -s %s --gp-c -d bkdb' % metadata_file

        restore_line = self.restore._build_schema_only_restore_line(restore_timestamp, restore_db, compress, master_port, metadata_file, table_filter_file)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_schema_only_restore_line_02(self, mock1, mock2):
        master_datadir = 'foo' 
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        self.restore.backup_dir = '/foo'
        table_filter_file = None
        metadata_file = os.path.join(master_datadir, 'db_dumps', restore_timestamp[0:8], 'gp_dump_1_1_%s.gz' % restore_timestamp)
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=/foo/db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=/foo/db_dumps/20121212 --gp-l=p -s %s --gp-c -d bkdb' % metadata_file

        restore_line = self.restore._build_schema_only_restore_line(restore_timestamp, restore_db, compress, master_port, metadata_file, table_filter_file)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_schema_only_restore_line_03(self, mock1, mock2):
        master_datadir = 'foo' 
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        self.restore.backup_dir = '/foo'
        backup_utils.dump_prefix = 'bar_'
        table_filter_file = None
        metadata_file = os.path.join(master_datadir, 'db_dumps', restore_timestamp[0:8], 'gp_dump_1_1_%s.gz' % restore_timestamp)
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=/foo/db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=/foo/db_dumps/20121212 --gp-l=p -s %s --prefix=bar_ --gp-c -d bkdb' % metadata_file

        restore_line = self.restore._build_schema_only_restore_line(restore_timestamp, restore_db, compress, master_port, metadata_file, table_filter_file)
        backup_utils.dump_prefix = ''
        self.assertEqual(restore_line, expected_output)

    
    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_schema_only_restore_line_04(self, mock1, mock2):
        master_datadir = 'foo' 
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        self.restore.backup_dir = '/foo'
        backup_utils.dump_prefix = 'bar_'
        table_filter_file = 'filter_file1'
        metadata_file = os.path.join(master_datadir, 'db_dumps', restore_timestamp[0:8], 'gp_dump_1_1_%s.gz' % restore_timestamp)
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=/foo/db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=/foo/db_dumps/20121212 --gp-l=p -s %s --prefix=bar_ --gp-f=%s --gp-c -d bkdb' % (metadata_file, table_filter_file)

        restore_line = self.restore._build_schema_only_restore_line(restore_timestamp, restore_db, compress, master_port, metadata_file, table_filter_file)
        backup_utils.dump_prefix = ''
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_gpdbrestore_cmd_line_00(self, mock1, mock2):
        ts = '20121212121212' 
        backup_utils.dump_prefix = 'bar_'
        expected_output = 'gpdbrestore -t 20121212121212 --table-file foo -a -v --noplan --noanalyze --noaostats --prefix=bar'
        restore_line = _build_gpdbrestore_cmd_line(ts, 'foo', None)
        backup_utils.dump_prefix = '' 
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_gpdbrestore_cmd_line_01(self, mock1, mock2):
        ts = '20121212121212' 
        backup_utils.dump_prefix = 'bar_'
        expected_output = 'gpdbrestore -t 20121212121212 --table-file foo -a -v --noplan --noanalyze --noaostats -u /tmp --prefix=bar'
        restore_line = _build_gpdbrestore_cmd_line(ts, 'foo', '/tmp')
        backup_utils.dump_prefix = '' 
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_00(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = False
        no_plan = True
        no_ao_stats = False
        table_filter_file = None
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p --gp-c -d bkdb -a'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_01(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = False
        no_plan = False
        no_ao_stats = False
        table_filter_file = '/tmp/foo'
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p --gp-f=/tmp/foo --gp-c -d bkdb'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_02(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = True
        no_plan = True
        no_ao_stats = False
        table_filter_file = None
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=foo/db_dumps/20121212 --gp-l=p --gp-c -d bkdb --ddboost -a'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_04(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = False
        no_plan = True
        no_ao_stats = True
        table_filter_file = None
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p --gp-c -d bkdb -a --gp-nostats'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_05(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = False
        no_plan = False
        no_ao_stats = True
        table_filter_file = '/tmp/foo'
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=db_dumps/20121212 --gp-l=p --gp-f=/tmp/foo --gp-c -d bkdb --gp-nostats'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_06(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = True
        no_plan = True
        no_ao_stats = True
        table_filter_file = None
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --gp-k=20121212121212 --gp-r=foo/db_dumps/20121212 --gp-l=p --gp-c -d bkdb --ddboost -a --gp-nostats'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.socket.gethostname', return_value='host')
    @patch('gppylib.operations.restore.getpass.getuser', return_value='user')
    def test_build_restore_line_07(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        restore_db = 'bkdb' 
        compress = True
        master_port = '5432'
        ddboost = True
        no_plan = True
        no_ao_stats = True
        table_filter_file = None
        backup_utils.dump_prefix = 'bar_'
        
        expected_output = 'gp_restore -i -h host -p 5432 -U user --gp-d=db_dumps/20121212 --gp-i --prefix=bar_ --gp-k=20121212121212 --gp-r=foo/db_dumps/20121212 --gp-l=p --gp-c -d bkdb --ddboost -a --gp-nostats'

        restore_line = self.restore._build_restore_line(restore_timestamp, restore_db, compress, master_port, ddboost, no_plan, table_filter_file, no_ao_stats)
        backup_utils.dump_prefix = ''
        self.assertEqual(restore_line, expected_output)

    @patch('gppylib.operations.restore.generate_plan_filename', return_value='foo')
    def test_get_plan_file_contents_00(self, mock1):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        with self.assertRaisesRegexp(Exception, 'Plan file foo does not exist'):
            get_plan_file_contents(master_datadir, backup_dir, timestamp)
    
    @patch('gppylib.operations.restore.generate_plan_filename', return_value='foo')
    @patch('gppylib.operations.restore.get_lines_from_file', return_value=[])
    @patch('os.path.isfile', return_value=True)
    def test_get_plan_file_contents_01(self, mock1, mock2, mock3):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        with self.assertRaisesRegexp(Exception, 'Plan file foo has no contents'):
            get_plan_file_contents(master_datadir, backup_dir, timestamp)

    @patch('gppylib.operations.restore.generate_plan_filename', return_value='foo')
    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['20121212121212:t1,t2', '20121212121211:t3,t4', '20121212121210:t5,t6,t7'])
    @patch('os.path.isfile', return_value=True)
    def test_get_plan_file_contents_02(self, mock1, mock2, mock3):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        expected_output = [('20121212121212','t1,t2'), ('20121212121211','t3,t4'), ('20121212121210','t5,t6,t7')]
        output = get_plan_file_contents(master_datadir, backup_dir, timestamp)
        self.assertEqual(output, expected_output)
    
    @patch('gppylib.operations.restore.generate_plan_filename', return_value='foo')
    @patch('gppylib.operations.restore.get_lines_from_file', return_value=['20121212121212:', '20121212121211', '20121212121210:'])
    @patch('os.path.isfile', return_value=True)
    def test_get_plan_file_contents_03(self, mock1, mock2, mock3):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        with self.assertRaisesRegexp(Exception, 'Invalid plan file format'):
            get_plan_file_contents(master_datadir, backup_dir, timestamp)
    
    @patch('gppylib.operations.restore.get_plan_file_contents', return_value=[('20121212121212','t1,t2'), ('20121212121211','t3,t4'), ('20121212121210','t5,t6,t7')])
    @patch('gppylib.operations.restore.Command.run')
    def test_restore_incremental_data_only_00(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        restore_tables = None
        results = restore_incremental_data_only(master_datadir, backup_dir, timestamp, restore_tables)
        self.assertTrue(results)
    
    @patch('gppylib.operations.restore.get_plan_file_contents', return_value=[('20121212121212',''), ('20121212121211',''), ('20121212121210','')])
    @patch('os.path.isfile', return_value=True)
    def test_restore_incremental_data_only_01(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        restore_tables = None
        with self.assertRaisesRegexp(Exception, 'There were no tables to restore. Check the plan file contents for restore timestamp %s' % timestamp):
            restore_incremental_data_only(master_datadir, backup_dir, timestamp, restore_tables)
    
    @patch('gppylib.operations.restore.get_plan_file_contents', return_value=[('20121212121212','t1,t2'), ('20121212121211','t3,t4'), ('20121212121210','t5,t6,t7')])
    @patch('gppylib.operations.restore.Command.run')
    def test_restore_incremental_data_only_02(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        restore_tables = None
        self.assertTrue(restore_incremental_data_only(master_datadir, backup_dir, timestamp, restore_tables))

    @patch('gppylib.operations.restore.get_plan_file_contents', return_value=[('20121212121212','t1,t2'), ('20121212121211','t3,t4'), ('20121212121210','t5,t6,t7')])
    @patch('gppylib.operations.restore.Command.run', side_effect=Exception('Error executing gpdbrestore'))
    def test_restore_incremental_data_only_04(self, mock1, mock2):
        master_datadir = 'foo' 
        timestamp = '20121212121212'
        backup_dir = None
        restore_tables = None
        with self.assertRaisesRegexp(Exception, 'Error executing gpdbrestore'):
            restore_incremental_data_only(master_datadir, backup_dir, timestamp, restore_tables)

    def test_get_restore_dir_00(self):
        master_datadir = '/foo'
        backup_dir = None
        self.assertEqual(get_restore_dir(master_datadir, backup_dir), '/foo')
        
    def test_get_restore_dir_01(self):
        master_datadir = None
        backup_dir = '/foo'
        self.assertEqual(get_restore_dir(master_datadir, backup_dir), '/foo')

    def test_get_restore_dir_02(self):
        master_datadir = None
        backup_dir = None
        self.assertEqual(get_restore_dir(master_datadir, backup_dir), None)

    @patch('gppylib.operations.restore.is_incremental_restore', return_value=True)
    def test_is_begin_incremental_run_00(self, m):
        mdd = '/foo'
        backup_dir = '/tmp'
        timestamp = '20130204135500'
        noplan = True
        result = is_begin_incremental_run(mdd, backup_dir, timestamp, noplan)
        self.assertFalse(result)

    @patch('gppylib.operations.restore.is_incremental_restore', return_value=True)
    def test_is_begin_incremental_run_01(self, m):
        mdd = '/foo'
        backup_dir = '/tmp'
        timestamp = '20130204135500'
        noplan = False
        result = is_begin_incremental_run(mdd, backup_dir, timestamp, noplan)
        self.assertTrue(result)

    @patch('gppylib.operations.restore.is_incremental_restore', return_value=False)
    def test_is_begin_incremental_run_02(self, m):
        mdd = '/foo'
        backup_dir = '/tmp'
        timestamp = '20130204135500'
        noplan = True
        result = is_begin_incremental_run(mdd, backup_dir, timestamp, noplan)
        self.assertFalse(result)

    @patch('gppylib.operations.restore.is_incremental_restore', return_value=False)
    def test_is_begin_incremental_run_03(self, m):
        mdd = '/foo'
        backup_dir = '/tmp'
        timestamp = '20130204135500'
        noplan = False
        result = is_begin_incremental_run(mdd, backup_dir, timestamp, noplan)
        self.assertFalse(result)

    def test_create_filter_file_00(self):
        self.restore.restore_tables = None
        fname = self.restore.create_filter_file()
        self.assertEquals(fname, None)

    @patch('gppylib.operations.restore.get_all_segment_addresses', return_value=['host1'])
    @patch('gppylib.operations.restore.scp_file_to_hosts')
    def test_create_filter_file_01(self, m1, m2):
        self.restore.restore_tables = ['public.ao1', 'pepper.heap1']
        fname = self.restore.create_filter_file()
        tables = None
        with open(fname) as fd:
            contents = fd.read()
            tables = contents.splitlines()
        self.assertEquals(tables,self.restore.restore_tables)
        os.remove(fname)
  
    @patch('gppylib.operations.restore.get_lines_from_file', return_value = ['public.t1', 'public.t2', 'public.t3'])
    @patch('os.path.isfile', return_value = True) 
    def test_get_restore_tables_from_table_file_00(self, mock1, mock2):
        table_file = '/foo'
        expected_result = ['public.t1', 'public.t2', 'public.t3'] 
        result = get_restore_tables_from_table_file(table_file)
        self.assertEqual(expected_result, result)

    @patch('os.path.isfile', return_value = False) 
    def test_get_restore_tables_from_table_file_01(self, mock):
        table_file = '/foo'
        expected_result = ['public.t1', 'public.t2', 'public.t3'] 
        with self.assertRaisesRegexp(Exception, 'Table file does not exist'): 
            result = get_restore_tables_from_table_file(table_file)

    def test_validate_tablenames_00(self):
        table_list = ['publicao1', 'public.ao2']
        with self.assertRaisesRegexp(Exception, 'No schema name supplied'):
            validate_tablenames(table_list)        

    def test_validate_tablenames_01(self):
        table_list = ['public.ao1', 'public.ao2']
        validate_tablenames(table_list)        

    def test_validate_tablenames_02(self):
        table_list = []
        validate_tablenames(table_list)        

    def test_get_restore_table_list_00(self):
        table_list = ['public.ao_table', 'public.ao_table2', 'public.co_table', 'public.heap_table']
        restore_tables = ['public.ao_table2', 'public.co_table']
        result = get_restore_table_list(table_list, restore_tables)
        with open(result) as fd:
            for line in fd:
                self.assertTrue(line.strip() in restore_tables)

    def test_get_restore_table_list_01(self):
        table_list = ['public.ao_table', 'public.ao_table2', 'public.co_table', 'public.heap_table']
        restore_tables = None 
        result = get_restore_table_list(table_list, restore_tables)
        with open(result) as fd:
            for line in fd:
                self.assertTrue(line.strip() in table_list)
    
    def test_get_restore_table_list_02(self):
        table_list = ['public.ao_table', 'public.ao_table2', 'public.co_table', 'public.heap_table']
        restore_tables = ['public.ao_table2', 'public.co_table', 'public.ao_table3']
        result = get_restore_table_list(table_list, restore_tables)
        with open(result) as fd:
            for line in fd:
                self.assertTrue(line.strip() in restore_tables)

    def test_validate_restore_tables_list_00(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.t4')]
        restore_tables = ['public.t1', 'public.t2']
        validate_restore_tables_list(plan_file_contents, restore_tables)

    def test_validate_restore_tables_list_01(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.t4')]
        restore_tables = ['public.t5', 'public.t2']
        with self.assertRaisesRegexp(Exception, 'Invalid tables for -T option: The following tables were not found in plan file'):
            validate_restore_tables_list(plan_file_contents, restore_tables)

    def test_validate_restore_tables_list_02(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.Ž')]
        restore_tables = ['public.t1', 'public.Áá']
        with self.assertRaisesRegexp(Exception, 'Invalid tables for -T option: The following tables were not found in plan file'):
            validate_restore_tables_list(plan_file_contents, restore_tables)

    def test_validate_restore_tables_list_03(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.测试')]
        restore_tables = ['public.t1', 'public.测试']
        validate_restore_tables_list(plan_file_contents, restore_tables)

    def test_validate_restore_tables_list_04(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.Ž')]
        restore_tables = ['public.t1', 'public.Ž']
        validate_restore_tables_list(plan_file_contents, restore_tables)

    def test_validate_restore_tables_list_05(self):
        plan_file_contents = [('20121212121213', 'public.t1'), ('20121212121212', 'public.t2,public.t3'), ('20121212121212', 'public.Áá')]
        restore_tables = ['public.t1', 'public.Áá']
        validate_restore_tables_list(plan_file_contents, restore_tables)

    @patch('gppylib.operations.unix.CheckFile.run', return_value=False)
    def test_restore_global_00(self, mock):
        restore_timestamp = '20121212121212' 
        master_datadir = 'foo' 
        backup_dir = None
        
        with self.assertRaisesRegexp(Exception, 'Unable to locate global file gp_global_1_1_20121212121212 in dump set'):
            self.restore._restore_global(restore_timestamp, master_datadir, backup_dir)

    @patch('os.path.exists', return_value=True)
    @patch('gppylib.commands.gp.Psql.run')
    def test_restore_global_01(self, mock1, mock2):
        restore_timestamp = '20121212121212' 
        master_datadir = 'foo' 
        backup_dir = None
        
        self.restore._restore_global(restore_timestamp, master_datadir, backup_dir) # should not error out

    @patch('gppylib.operations.restore.execSQLForSingleton')
    @patch('pygresql.pgdb.pgdbCnx.commit')
    def test_update_ao_stat_func_00(self, m1, m2):
        conn = None
        schema = None
        table = None
        counter = 1
        batch_size = 1000
        update_ao_stat_func(conn, schema, table, counter, batch_size)

    @patch('pygresql.pgdb.pgdbCnx.commit')
    @patch('gppylib.operations.restore.execSQLForSingleton')
    def test_update_ao_stat_func_01(self, m1, m2):
        conn = None
        schema = None
        table = None
        counter = 999
        batch_size = 1000
        update_ao_stat_func(conn, schema, table, counter, batch_size)

    @patch('gppylib.operations.restore.execSQLForSingleton')
    @patch('pygresql.pgdb.pgdbCnx.commit')
    def test_update_ao_stat_func_02(self, m1, m2):
        conn = None
        schema = None
        table = None
        counter = 1000
        batch_size = 1000
        with self.assertRaisesRegexp(AttributeError, "'NoneType' object has no attribute 'commit'"):
            update_ao_stat_func(conn, schema, table, counter, batch_size)

    @patch('gppylib.operations.restore.execSQLForSingleton')
    @patch('pygresql.pgdb.pgdbCnx.commit')
    def test_update_ao_stat_func_03(self, m1, m2):
        conn = None
        schema = None
        table = None
        counter = 1001
        batch_size = 1000
        update_ao_stat_func(conn, schema, table, counter, batch_size)

    @patch('gppylib.operations.restore.execSQLForSingleton')
    @patch('pygresql.pgdb.pgdbCnx.commit')
    def test_update_ao_stat_func_04(self, m1, m2):
        conn = None
        schema = None
        table = None
        counter = 2000
        batch_size = 1000
        with self.assertRaisesRegexp(AttributeError, "'NoneType' object has no attribute 'commit'"):
            update_ao_stat_func(conn, schema, table, counter, batch_size)


    @patch('gppylib.operations.restore.execute_sql', return_value=[['t1', 'public']])
    @patch('gppylib.operations.restore.dbconn.connect')
    @patch('gppylib.operations.restore.update_ao_stat_func')
    def test_update_ao_statistics_00(self, m1, m2, m3):
        port = 28888
        db = 'testdb'
        update_ao_statistics(port, db)



if __name__ == '__main__':
    unittest.main()
