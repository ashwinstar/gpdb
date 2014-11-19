#!/usr/bin/env python
#
# Copyright (c) Pivotal Inc 2014. All Rights Reserved. 
#

import os
import re
import shutil
import unittest2 as unittest
from collections import defaultdict
from gppylib.gpversion import GpVersion
from gppylib.commands.base import Command, CommandResult, ExecutionError
from mock import patch, MagicMock, Mock, mock_open
from gppylib.operations.persistent_rebuild import ValidateContentID, DbIdInfo, GetDbIdInfo, BackupPersistentTableFiles,\
                                                  RebuildTable, ResetXlog, RebuildPersistentTables, ValidatePersistentBackup,\
                                                  RunBackupRestore, ValidateMD5Sum

class ValidateContentIDTestCase(unittest.TestCase):
    def setUp(self):
        self.contentid_validator = ValidateContentID(content_id=None,
                                                     contentid_file=None,
                                                     gparray=None)

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_valid_content_ids(self, mock1):
        expected = [1, 2, 3]
        file_contents = '1\n2\n3'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            self.assertEqual(expected, self.contentid_validator._validate_contentid_file()) 

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_spaces_content_ids(self, mock1):
        expected = [1, 2, 3]
        file_contents = '    1\n2   \n3   \n'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            self.assertEqual(expected, self.contentid_validator._validate_contentid_file()) 

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_invalid_content_ids(self, mock1):
        expected = [1, 2, 3]
        file_contents = '1\nb\n3'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            with self.assertRaisesRegexp(Exception, 'Found non integer content id "b" in contentid file "/tmp/contentid_file"'):
                self.contentid_validator._validate_contentid_file()

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_empty_file(self, mock1):
        file_contents = ''
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            with self.assertRaisesRegexp(Exception, 'Please make sure there is atleast one integer content ID in the file'):
                self.contentid_validator._validate_contentid_file()

    @patch('os.path.isfile', return_value=False)
    def test_validate_contentid_file_with_non_existent_file(self, mock1):
        expected = [1, 2, 3]
        file_contents = '1\nb\n3'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        with self.assertRaisesRegexp(Exception, 'Unable to find contentid file "/tmp/contentid_file"'):
            self.contentid_validator._validate_contentid_file()

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_blank_lines(self, mock1):
        expected = [1, 2]
        file_contents = '1\n\n\n2'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            self.assertEqual(expected, self.contentid_validator._validate_contentid_file())

    @patch('os.path.isfile', return_value=True)
    def test_validate_contentid_file_with_negative_integers(self, mock1):
        expected = [-1, 2]
        file_contents = '-1\n2'
        self.contentid_validator.contentid_file = '/tmp/contentid_file'
        m = MagicMock()
        m.return_value.__enter__.return_value.__iter__.return_value = iter(file_contents.split())
        with patch('__builtin__.open', m, create=True):
            self.assertEqual(expected, self.contentid_validator._validate_contentid_file())

    def test_validate_content_id_with_valid_segments(self):
        expected = [1, 2, 3]
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId = Mock()
            m.getSegmentContentId.return_value = (i % 3) + 1
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs 
        self.contentid_validator.gparray = gparray
        self.contentid_validator.content_id = [1, 2, 3]
        self.assertEqual(expected, self.contentid_validator._validate_content_id())

    def test_validate_content_id_with_invalid_segments(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId = Mock()
            m.getSegmentContentId.return_value = i + 10
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs 
        self.contentid_validator.gparray = gparray
        self.contentid_validator.content_id = [1, 2, 3]
        with self.assertRaisesRegexp(Exception, 'The following content ids are not present in gp_segment_configuration: 1, 2, 3'):
            self.contentid_validator._validate_content_id()

    def test_validate_content_id_with_primary_segment_down(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId = Mock()
            m.getSegmentContentId.return_value = (i % 3) + 1
            if i == 1:
                m.getSegmentStatus = Mock()
                m.getSegmentStatus.return_value = 'd'
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs 
        self.contentid_validator.gparray = gparray
        self.contentid_validator.content_id = [1, 2, 3]
        self.contentid_validator._validate_content_id()

    def test_validate_content_id_with_change_tracking_segments(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId = Mock()
            m.getSegmentContentId.return_value = (i % 3) + 1
            if i == 1:
                m.getSegmentStatus = Mock()
                m.getSegmentStatus.return_value = 'c'
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs 
        self.contentid_validator.gparray = gparray
        self.contentid_validator.content_id = [1, 2, 3]
        self.assertEqual([1, 2, 3], self.contentid_validator._validate_content_id())

    def test_parse_content_id(self):
        self.contentid_validator.content_id = '1, 2, 3'
        self.assertEqual([1, 2, 3], self.contentid_validator._parse_content_id())

    def test_parse_content_id_valid_single_content_id(self):
        self.contentid_validator.content_id = '-1'
        self.assertEqual([-1], self.contentid_validator._parse_content_id())

    def test_parse_content_id_invalid_comma_separated_list(self):
        self.contentid_validator.content_id = '1, 2, 3,,'
        with self.assertRaisesRegexp(Exception, 'Some content ids are not integers:'):
            self.contentid_validator._parse_content_id()

    def test_parse_content_id_invalid_integers(self):
        self.contentid_validator.content_id = '1, 2, a, x,'
        with self.assertRaisesRegexp(Exception, 'Some content ids are not integers:'):
            self.contentid_validator._parse_content_id()

    @patch('gppylib.operations.persistent_rebuild.ValidateContentID._validate_content_id', return_value=[1, 2, 3])
    def test_validate_with_only_content_id(self, mock1):
        self.contentid_validator.content_id = '1, 2, 3'
        self.contentid_validator.contentid_file = None 
        self.assertEqual([1, 2, 3], self.contentid_validator.validate())

    @patch('gppylib.operations.persistent_rebuild.ValidateContentID._validate_content_id', side_effect=Exception('ERROR'))
    def test_validate_with_only_content_id_with_error(self, mock1):
        self.contentid_validator.content_id = '1, 2, 3'
        self.contentid_validator.contentid_file = None 
        with self.assertRaisesRegexp(Exception, 'ERROR'):
            self.contentid_validator.validate()

    @patch('gppylib.operations.persistent_rebuild.ValidateContentID._validate_contentid_file', return_value=[1, 2, 3])
    @patch('gppylib.operations.persistent_rebuild.ValidateContentID._validate_content_id', return_value=[1, 2, 3])
    def test_validate_with_only_content_id_file(self, mock1, mock2):
        self.contentid_validator.contentid_file = '/tmp/f1'
        self.contentid_validator.content_id = None
        self.assertEqual([1, 2, 3], self.contentid_validator.validate())

    @patch('gppylib.operations.persistent_rebuild.ValidateContentID._validate_contentid_file', side_effect=Exception('ERROR'))
    def test_validate_with_only_content_id_file_with_error(self, mock1):
        self.contentid_validator.contentid_file = '/tmp/f1'
        self.contentid_validator.content_id = None
        with self.assertRaisesRegexp(Exception, 'ERROR'):
            self.contentid_validator.validate()

class GetDbIdInfoTestCase(unittest.TestCase):
    def setUp(self):
        self.dbid_info = GetDbIdInfo(gparray=None, content_id=None)

    @patch('gppylib.operations.persistent_rebuild.dbconn.execSQL', return_value=[(1000, '2000'), (1001, '2001 2002')])
    @patch('gppylib.operations.persistent_rebuild.dbconn.connect')
    @patch('gppylib.operations.persistent_rebuild.dbconn.DbURL')
    def test_get_filespace_to_tablespace_map(self, mock1, mock2, mock3):
        m = Mock()
        m.getSegmentFilespaces.return_value = {1000: '/tmp/fs1', 1001: '/tmp/fs2'}
        self.assertEqual({1000: [2000], 1001: [2001, 2002]}, self.dbid_info._get_filespace_to_tablespace_map(m))

    @patch('gppylib.operations.persistent_rebuild.dbconn.execSQL', return_value=[])
    @patch('gppylib.operations.persistent_rebuild.dbconn.connect')
    @patch('gppylib.operations.persistent_rebuild.dbconn.DbURL')
    def test_get_filespace_to_tablespace_map_empty_filespaces(self, mock1, mock2, mock3):
        m = Mock()
        m.getSegmentFilespaces.return_value = {}
        self.assertEqual({}, self.dbid_info._get_filespace_to_tablespace_map(m))

    @patch('gppylib.operations.persistent_rebuild.dbconn.execSQL', return_value=[(1000, '2000'), (1001, '2001 2002')])
    @patch('gppylib.operations.persistent_rebuild.dbconn.connect')
    @patch('gppylib.operations.persistent_rebuild.dbconn.DbURL')
    def test_get_tablespace_to_dboid_map(self, mock1, mock2, mock3):
        ts_oids = [1000, 1001]
        self.assertEqual({1000: [2000], 1001: [2001, 2002]}, self.dbid_info._get_tablespace_to_dboid_map(ts_oids))

    @patch('gppylib.operations.persistent_rebuild.dbconn.execSQL', return_value=[])
    @patch('gppylib.operations.persistent_rebuild.dbconn.connect')
    @patch('gppylib.operations.persistent_rebuild.dbconn.DbURL')
    def test_get_tablespace_to_dboid_map_empty_tablespaces(self, mock1, mock2, mock3):
        ts_oids = []
        self.assertEqual({}, self.dbid_info._get_tablespace_to_dboid_map(ts_oids))

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_no_matching_content_id(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'u'
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [11, 12]
        expected = []
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={1000: [2000, 2002], 1001: [2001, 2003]})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map',
            return_value={2000: [12345], 2001: [2345, 4567], 2002: [8765, 4634], 2003: [3456]})
    def test_get_info_with_single_matching_content_id(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {1000: '/tmp/f1', 1001: '/tmp/f2'}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = [DbIdInfo(1, 2, 5001, 'mdw1', {1000: '/tmp/f1', 1001: '/tmp/f2'}, {1000: [2000, 2002], 1001: [2001, 2003]},
                    {2000: [12345], 2001: [2345, 4567], 2002: [8765, 4634], 2003: [3456]})]
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_single_matching_content_id_and_no_filespaces(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = [DbIdInfo(1, 2, 5001, 'mdw1', {}, {}, {})]
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_single_matching_content_id_and_no_tablespaces(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {1000: '/tmp/f1', 1001: '/tmp/f2'}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = [DbIdInfo(1, 2, 5001, 'mdw1', {1000: '/tmp/f1', 1001: '/tmp/f2'}, {}, {})]
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_single_matching_content_id_and_down_segments(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'd' if i == 3 else 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {1000: '/tmp/f1', 1001: '/tmp/f2'}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = [DbIdInfo(1, 2, 5001, 'mdw1', {1000: '/tmp/f1', 1001: '/tmp/f2'}, {}, {})]
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_single_matching_content_id_and_segment_in_ct(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'c' if i == 3 else 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {1000: '/tmp/f1', 1001: '/tmp/f2'}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = [DbIdInfo(1, 2, 5001, 'mdw1', {1000: '/tmp/f1', 1001: '/tmp/f2'}, {}, {})]
        self.assertEqual(expected, self.dbid_info.get_info())

    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_filespace_to_tablespace_map', return_value={})
    @patch('gppylib.operations.persistent_rebuild.GetDbIdInfo._get_tablespace_to_dboid_map', return_value={})
    def test_get_info_with_single_matching_content_id_and_content_down(self, mock1, mock2):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentDbId.return_value = i + 2
            m.getSegmentRole.return_value = 'p' if i < 3 else 'm'
            m.getSegmentStatus.return_value = 'd' if i == 3 or i == 0 else 'u'
            m.getSegmentHostName.return_value = 'mdw1'
            m.getSegmentPort.return_value = 5001 + i
            m.getSegmentFilespaces.return_value = {1000: '/tmp/f1', 1001: '/tmp/f2'}
            mock_segs.append(m)
        gparray = Mock()
        gparray.getDbList = Mock()
        gparray.getDbList.return_value = mock_segs
        self.dbid_info.gparray = gparray
        self.dbid_info.content_id = [1, 10]
        expected = []
        self.assertEqual(expected, self.dbid_info.get_info())

class BackupPersistentTableFilesTestCase(unittest.TestCase):
    def setUp(self):
        self.backup_persistent_files = BackupPersistentTableFiles(dbid_info=None, perdb_pt_filenames=['5094', '5095'],
                                                                  global_pt_filenames=['5090', '5091', '5092', '5093'],
                                                                  timestamp='20140604101010')

    @patch('os.makedirs')
    def test_copy_pt_files(self, mock1):
        src_ptfiles = ['/tmp/global/5090', '/tmp/global/5091']
        dst_ptfiles = ['/tmp1/global/5090', '/tmp1/global/5091']
        host = 'mdw1'
        self.backup_persistent_files.pool = Mock()
        m = Mock()
        m.validate.side_effect = [{'f1': 'abdfe'}, {'f1': 'abdfe'}] 
        self.backup_persistent_files.md5_validator = m 
        self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host)

    @patch('os.makedirs')
    @patch('gppylib.operations.persistent_rebuild.Command.run')
    def test_copy_pt_files_with_restore(self, mock1, mock2):
        src_ptfiles = ['/tmp/global/5090', '/tmp/global/5091']
        dst_ptfiles = ['/tmp1/global/5090', '/tmp1/global/5091']
        host = 'mdw1'
        self.backup_persistent_files.pool = Mock()
        m = Mock()
        m.validate.side_effect = [{'f1': 'abdfe'}, {'f1': 'abdfe'}] 
        self.backup_persistent_files.md5_validator = m 
        self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host, restore=True)

    @patch('os.makedirs')
    def test_copy_pt_files_without_errors_with_no_files(self, mock1):
        src_ptfiles = []
        dst_ptfiles = []
        host = 'mdw1'
        self.backup_persistent_files.pool = Mock() 
        m = Mock()
        m.validate.side_effect = [{}, {}]
        self.backup_persistent_files.md5_validator = m
        self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host)

    @patch('os.makedirs')
    @patch('gppylib.operations.persistent_rebuild.Command.run')
    def test_copy_pt_files_without_errors_with_no_files_with_restore(self, mock1, mock2):
        src_ptfiles = []
        dst_ptfiles = []
        host = 'mdw1'
        self.backup_persistent_files.pool = Mock() 
        m = Mock()
        m.validate.side_effect = [{}, {}]
        self.backup_persistent_files.md5_validator = m
        self.backup_persistent_files.restore=True
        self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host)

    @patch('os.makedirs')
    def test_copy_pt_files_with_md5_mismatch(self, mock1):
        src_ptfiles = []
        dst_ptfiles = []
        host = 'mdw1'
        self.backup_persistent_files.pool = Mock()
        m = Mock()
        m.validate.side_effect = [{'f1': 'asdfads'}, {'f1': 'asdfadsf'}]
        self.backup_persistent_files.md5_validator = m 
        with self.assertRaisesRegexp(Exception, 'MD5 sums do not match! Expected md5 = "{\'f1\': \'asdfads\'}",\
 but actual md5 = "{\'f1\': \'asdfadsf\'}"'):
            self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host)

    @patch('os.makedirs')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.validate', return_value={'5090': 'sdfadsf', '5091': 'sdfadsf'})
    def test_copy_pt_files_with_errors(self, mock1, mock2):
        src_ptfiles = ['/tmp/global/5090', '/tmp/global/5091']
        dst_ptfiles = ['/tmp1/global/5090', '/tmp1/global/5091']
        host = 'mdw1'
        m = Mock()
        m.check_results.side_effect = ExecutionError('Error !!!', Mock())
        self.backup_persistent_files.pool = m
        m.validate.return_value = {'5090': 'sdfadsf', '5091': 'sdfadsf'}
        self.backup_persistent_files.md5_validator = m
        with self.assertRaisesRegexp(ExecutionError, 'Error !!!'):
            self.backup_persistent_files._copy_pt_files(src_ptfiles, dst_ptfiles, host)

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_global_pt_files(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_global_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files',
            side_effect=[Mock(), Exception('Error while backing up files')])
    def test_copy_global_pt_files_with_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Backup of global persistent files failed'):
            self.backup_persistent_files._copy_global_pt_files()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_global_pt_files_without_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_global_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_global_pt_files_with_restore_without_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_global_pt_files(restore=True))

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files',
            side_effect=[Mock(), Exception('Error while backing up files')])
    def test_copy_global_pt_files_with_restore_with_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Restore of global persistent files failed'):
            self.backup_persistent_files._copy_global_pt_files(restore=True)

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_per_db_pt_files(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_per_db_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files', 
            side_effect=[Mock(), Exception('Error while backing up files')])
    def test_copy_per_db_pt_files_with_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Backup of per db persistent files failed'):
            self.backup_persistent_files._copy_per_db_pt_files()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_per_db_pt_files_without_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_per_db_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_per_db_pt_files_with_unused_filespace(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_per_db_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_per_db_pt_files_with_unused_tablespace(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_per_db_pt_files())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    def test_copy_per_db_pt_files_with_restore_without_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files._copy_per_db_pt_files(restore=True))

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files',
            side_effect=[Mock(), Mock(), Mock(), Exception('Error while backing up files')])
    def test_copy_per_db_pt_files_with_restore_with_errors(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Restore of per db persistent files failed'):
            self.backup_persistent_files._copy_per_db_pt_files(restore=True)

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_pt_files')
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_without_errors(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files.restore())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_with_global_file_bkup_error(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Error'):
            self.backup_persistent_files.restore()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_with_per_db_bkup_error(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Error'):
            self.backup_persistent_files.restore()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_without_errors(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        self.assertEqual(None, self.backup_persistent_files.restore())

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_with_global_file_bkup_error(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Error'):
            self.backup_persistent_files.restore()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', return_value=None)
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_with_per_db_bkup_error(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Error'):
            self.backup_persistent_files.restore()

    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_global_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.BackupPersistentTableFiles._copy_per_db_pt_files', side_effect=Exception('Error'))
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.Command')
    @patch('gppylib.operations.persistent_rebuild.ValidateMD5Sum.init')
    def test_restore_with_global_and_per_db_bkup_error(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        self.backup_persistent_files.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Error'):
            self.backup_persistent_files.restore()

class RebuildTableTestCase(unittest.TestCase):
    def setUp(self):
        self.rebuild_table = RebuildTable(dbid_info=None)

    def test_get_valid_dbids(self):
        content_ids = [1, 2]
        expected = [0, 1]
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u' 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        self.assertEqual(expected, self.rebuild_table._get_valid_dbids(content_ids))

    def test_get_valid_dbids_empty_contents(self):
        content_ids = []
        expected = []
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u' 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        self.assertEqual(expected, self.rebuild_table._get_valid_dbids(content_ids))

    def test_get_valid_dbids_non_matching_content_ids(self):
        content_ids = [3, 4, 5]
        expected = []
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u' 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        self.assertEqual(expected, self.rebuild_table._get_valid_dbids(content_ids))

    def test_get_valid_dbids_content_ids_down(self):
        content_ids = [1, 2, 3]
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u' if i % 2 else 'd' 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        with self.assertRaisesRegexp(Exception, 'Segment .* is down. Cannot continue with persistent table rebuild'):
            self.rebuild_table._get_valid_dbids(content_ids)

    def test_get_valid_dbids_content_ids_resync(self):
        content_ids = [1, 2, 3]
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u' 
            m.getSegmentMode.return_value = 'r' if i % 2 else 's'
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        with self.assertRaisesRegexp(Exception, 'Segment .* is in resync. Cannot continue with persistent table rebuild'):
            self.rebuild_table._get_valid_dbids(content_ids)

    @patch('gppylib.operations.persistent_rebuild.ValidatePersistentBackup.validate', return_value=Mock())
    def test_get_valid_dbids_content_ids_are_mirrors(self, mock1):
        content_ids = [1, 2, 3]
        expected = [1]
        mock_segs = []
        for i in range(2):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.getSegmentRole.return_value = 'p' if i % 2 else 'm'
            m.getSegmentDbId.return_value = i 
            m.getSegmentPort.return_value = 5000 + i
            m.getSegmentHostName.return_value = 'mdw%d' % (i + 1) 
            m.getSegmentStatus.return_value = 'u'
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_table.gparray = m
        self.assertEqual(expected, self.rebuild_table._get_valid_dbids(content_ids))

    def test_get_host_to_dbid_info_map(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        expected = {'h1': [d1]}
        self.assertEqual(expected, self.rebuild_table._get_host_to_dbid_info_map([d1]))

    def test_get_host_to_dbid_info_map_empty(self):
        self.assertEqual({}, self.rebuild_table._get_host_to_dbid_info_map([]))

    @patch('gppylib.operations.persistent_rebuild.GpArray')
    @patch('gppylib.operations.persistent_rebuild.RebuildTable._validate_backups')
    @patch('gppylib.operations.persistent_rebuild.RebuildTable._get_valid_dbids', return_value=[1, 2, 3])
    @patch('gppylib.operations.persistent_rebuild.ParallelOperation.run')
    def test_rebuild(self, mock1, mock2, mock3, mock4):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.rebuild_table.dbid_info = [d1, d2]
        expected_success = [d1, d2]
        expected_failure = []
        self.assertEqual((expected_success, expected_failure), self.rebuild_table.rebuild())

    @patch('gppylib.operations.persistent_rebuild.GpArray')
    @patch('gppylib.operations.persistent_rebuild.RebuildTable._validate_backups')
    @patch('gppylib.operations.persistent_rebuild.RebuildTable._get_valid_dbids', return_value=[1, 2, 3])
    @patch('gppylib.operations.persistent_rebuild.ParallelOperation.run')
    @patch('gppylib.operations.persistent_rebuild.RemoteOperation.get_ret', side_effect=[Mock(), Exception('Error')])
    def test_rebuild_with_errors(self, mock1, mock2, mock3, mock4, mock5):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.rebuild_table.dbid_info = [d1, d2]
        expected_success = [d1]
        expected_failure = [(d2, 'Error')]
        self.assertEqual((expected_success, expected_failure), self.rebuild_table.rebuild())

class ResetXlogTestCase(unittest.TestCase):
    def setUp(self):
        self.reset_xlog = ResetXlog(failures=None, gparray=None)

    @patch('gppylib.operations.persistent_rebuild.Command')
    def test_reset_with_no_failures(self, mock1):
        mock_segs = []
        for i in range(3):
            m = Mock()
            m.getSegmentContentId.return_value = i - 1
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.reset_xlog.gparray = m
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.reset_xlog.failures = []
        with patch('gppylib.operations.persistent_rebuild.WorkerPool', return_value=m):
            self.assertEquals([], self.reset_xlog.reset())

    @patch('gppylib.operations.persistent_rebuild.Command')
    def test_reset_with_failures(self, mock1):
        d = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        mock_segs = []
        for i in range(3):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.reset_xlog.gparray = m
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.reset_xlog.failures = [(d, Exception('Error'))]
        with patch('gppylib.operations.persistent_rebuild.WorkerPool', return_value=m):
            self.assertEqual([], self.reset_xlog.reset())

    @patch('gppylib.operations.persistent_rebuild.Command.get_results', side_effect=[Mock(), CommandResult(1, '/tmp/f1', '', True, False)])
    def test_reset_with_failures_with_error(self, mock1):
        d = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        mock_segs = []
        for i in range(3):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.reset_xlog.gparray = m
        m1 = Mock()
        m1.get_results.return_value = CommandResult(1, 'Error', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.reset_xlog.failures = [(d, Exception('Error'))]
        with patch('gppylib.operations.persistent_rebuild.WorkerPool', return_value=m):
            self.assertEqual([(d, 'Error')], self.reset_xlog.reset())

    @patch('gppylib.operations.persistent_rebuild.Command.run')
    def test_reset_with_mirrors(self, mock1):
        d = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.isSegmentMirror.return_vale = True if i > 3 else False
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.reset_xlog.gparray = m
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.reset_xlog.failures = [(d, Exception('Error'))]
        with patch('gppylib.operations.persistent_rebuild.WorkerPool', return_value=m):
            self.assertEqual([], self.reset_xlog.reset())

    @patch('gppylib.operations.persistent_rebuild.Command.run')
    def test_reset_without_mirrors(self, mock1):
        d = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i + 1
            m.isSegmentMirror.return_vale = True
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.reset_xlog.gparray = m
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.reset_xlog.failures = [(d, Exception('Error'))]
        with patch('gppylib.operations.persistent_rebuild.WorkerPool', return_value=m):
            self.assertEqual([], self.reset_xlog.reset())

class ValidatePersistentBackupTestCase(unittest.TestCase):
    def setUp(self):
        self.validate_persistent_backup = ValidatePersistentBackup(dbid_info=None, timestamp='20140605101010')

    def test_process_results(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m = Mock()
        m.getCompletedItems.return_value = [m1, m2]
        self.validate_persistent_backup._process_results(d1, m)

    def test_process_results_with_errors(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(1, '/tmp/f1', '', True, False)
        m = Mock()
        m.getCompletedItems.return_value = [m1, m2]
        with self.assertRaisesRegexp(Exception, 'Failed to validate backups'):
            self.validate_persistent_backup._process_results(d1, m)

    def test_process_results_with_missing_backup(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '', '', True, False)
        m = Mock()
        m.getCompletedItems.return_value = [m1, m2]
        with self.assertRaisesRegexp(Exception, 'Failed to validate backups'):
            self.validate_persistent_backup._process_results(d1, m)

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    def test_validate(self, mock1):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.validate_persistent_backup.dbid_info = [d1, d2]
        self.validate_persistent_backup.validate()

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.ValidatePersistentBackup._process_results', side_effect=Exception('Failed to validate backups'))
    def test_validate_error_in_workerpool(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        self.validate_persistent_backup.dbid_info = [d1, d2]
        with self.assertRaisesRegexp(Exception, 'Failed to validate backups'):
            self.validate_persistent_backup.validate()

class RunBackupRestoreTestCase(unittest.TestCase):
    def setUp(self):
        self.run_backup_restore = RunBackupRestore(dbid_info=None, timestamp=None)

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results')
    def test_run_backup_restore(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        self.run_backup_restore._run_backup_restore(host_to_dbid_info_map)

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results', side_effect=Exception('ERROR'))
    def test_run_backup_restore_with_errors(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        with self.assertRaisesRegexp(Exception, 'ERROR'):
            self.run_backup_restore._run_backup_restore(host_to_dbid_info_map)

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results')
    def test_run_backup_restore_with_restore(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        self.run_backup_restore._run_backup_restore(host_to_dbid_info_map, restore=True)
    
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results', side_effect=Exception('ERROR'))
    def test_run_backup_restore_with_errors_with_restore(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        with self.assertRaisesRegexp(Exception, 'ERROR'):
            self.run_backup_restore._run_backup_restore(host_to_dbid_info_map, restore=True)

    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results')
    def test_run_backup_restore_with_validate(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        self.run_backup_restore._run_backup_restore(host_to_dbid_info_map, validate=True)
    
    @patch('gppylib.operations.persistent_rebuild.WorkerPool')
    @patch('gppylib.operations.persistent_rebuild.RunBackupRestore._process_results', side_effect=Exception('ERROR'))
    def test_run_backup_restore_with_errors_with_validate(self, mock1, mock2):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        host_to_dbid_info_map = {'h1': [d1], 'h2': [d2]}
        with self.assertRaisesRegexp(Exception, 'ERROR'):
            self.run_backup_restore._run_backup_restore(host_to_dbid_info_map, validate=True)

    def test_get_host_to_dbid_info_map(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h2', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        expected = {'h1': [d1], 'h2': [d2]}
        self.assertEqual(expected, self.run_backup_restore._get_host_to_dbid_info_map([d1, d2]))

    def test_get_host_to_dbid_info_map_empty(self):
        self.assertEqual({}, self.run_backup_restore._get_host_to_dbid_info_map([]))

    def test_get_host_to_dbid_info_map_multiple_entries_per_host(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        d2 = DbIdInfo(2, 3, 5002, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]}) 
        expected = {'h1': [d1, d2]}
        self.assertEqual(expected, self.run_backup_restore._get_host_to_dbid_info_map([d1, d2]))

    def test_process_results(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m = Mock()
        m.getCompletedItems.return_value = [m1, m2]
        self.run_backup_restore._process_results(m, 'ERR')

    def test_process_results_with_errors(self):
        d1 = DbIdInfo(1, 2, 5001, 'h1', {1000: '/tmp/p1', 3052: '/tmp/p2'}, {1000: [2000], 3052: [2001]}, {2000: [123], 2001: [234]})
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, '/tmp/f1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(1, 'ERR', '', True, False)
        m = Mock()
        m.getCompletedItems.return_value = [m1, m2]
        with self.assertRaisesRegexp(Exception, 'ERR'):
            self.run_backup_restore._process_results(m, 'ERR')

class ValidateMD5SumTestCase(unittest.TestCase):
    def setUp(self):
        self.validate_md5sum = ValidateMD5Sum(pool=None)

    @patch('platform.system', return_value='Darwin')
    def test_get_md5_prog_for_osx(self, mock1):
        self.assertEqual('md5', self.validate_md5sum._get_md5_prog())

    @patch('platform.system', return_value='Linux')
    def test_get_md5_prog_for_linux(self, mock1):
        self.assertEqual('md5sum', self.validate_md5sum._get_md5_prog())

    @patch('platform.system', return_value='Solaris')
    def test_get_md5_prog_for_invalid_os(self, mock1):
        with self.assertRaisesRegexp(Exception, 'Cannot determine the md5 program since Solaris platform is not supported'):
            self.validate_md5sum._get_md5_prog()

    @patch('platform.system', return_value='Darwin')
    def test_get_md5_results_pat_for_osx(self, mock1):
        pat = re.compile('MD5 \((.*)\) = (.*)')
        self.assertEqual(pat, self.validate_md5sum._get_md5_results_pat())

    @patch('platform.system', return_value='Linux')
    def test_get_md5_results_pat_for_osx(self, mock1):
        pat = re.compile('(.*) (.*)')
        self.assertEqual(pat, self.validate_md5sum._get_md5_results_pat())

    @patch('platform.system', return_value='Solaris')
    def test_get_md5_results_pat_for_invalid_os(self, mock1):
        with self.assertRaisesRegexp(Exception, 'Cannot determine the pattern for results of md5 program since Solaris platform is not supported'):
            self.validate_md5sum._get_md5_results_pat()

    @patch('platform.system', return_value='Darwin')
    def test_process_results_on_osx(self, mock1):
        m = Mock()
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, 'MD5 (foo) = afsdfasdf', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, 'MD5 (foo1) = sdfadsff', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.validate_md5sum.pool = m
        self.validate_md5sum.md5_results_pat = re.compile('MD5 \((.*)\) = (.*)')
        expected = {'foo': 'afsdfasdf', 'foo1': 'sdfadsff'}
        self.assertEqual(expected, self.validate_md5sum._process_md5_results())

    @patch('platform.system', return_value='Darwin')
    def test_process_results_on_osx_with_error(self, mock1):
        m = Mock()
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, 'MD5 (foo1) = sdfadsff', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(1, '', 'Error', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.validate_md5sum.pool = m
        self.validate_md5sum.md5_results_pat = re.compile('MD5 \((.*)\) = (.*)')
        with self.assertRaisesRegexp(Exception, 'Unable to calculate md5sum'):
            self.validate_md5sum._process_md5_results()

    @patch('platform.system', return_value='Linux')
    def test_process_results_on_linux(self, mock1):
        m = Mock()
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, 'afsdfasdf foo', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(0, 'sdfadsff foo1', '', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.validate_md5sum.pool = m
        self.validate_md5sum.md5_results_pat = re.compile('(.*) (.*)')
        expected = {'foo': 'afsdfasdf', 'foo1': 'sdfadsff'}
        self.assertEqual(expected, self.validate_md5sum._process_md5_results())

    @patch('platform.system', return_value='Linux')
    def test_process_results_on_linux_with_error(self, mock1):
        m = Mock()
        m1 = Mock()
        m1.get_results.return_value = CommandResult(0, 'sdfadsff fo1', '', True, False)
        m2 = Mock()
        m2.get_results.return_value = CommandResult(1, '', 'Error', True, False)
        m.getCompletedItems.return_value = [m1, m2]
        self.validate_md5sum.pool = m
        self.validate_md5sum.md5_results_pat = re.compile('(.*) (.*)')
        with self.assertRaisesRegexp(Exception, 'Unable to calculate md5sum'):
            self.validate_md5sum._process_md5_results()

class RebuildPersistentTableTestCase(unittest.TestCase):
    def setUp(self):
        self.rebuild_persistent_table = RebuildPersistentTables(content_id = None,
                                                                contentid_file = None,
                                                                backup=None,
                                                                restore=None,
                                                                batch_size=None)

    @patch('gppylib.operations.persistent_rebuild.platform.system', return_value='Linux')
    def test_check_platform_linux(self, mock1):
        self.rebuild_persistent_table._check_platform()

    @patch('gppylib.operations.persistent_rebuild.platform.system', return_value='Solaris')
    def test_check_platform_non_linux(self, mock1):
        with self.assertRaisesRegexp(Exception, 'This tool is only supported on Linux and OSX platforms'):
            self.rebuild_persistent_table._check_platform()

    def test_validate_has_mirrors(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i - 1
            m.isSegmentMirror.return_value = True if i < 3 else False
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_persistent_table.gparray = m
        self.assertTrue(self.rebuild_persistent_table._validate_has_mirrors())

    def test_validate_has_mirrors_with_no_mirrors(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i - 1
            m.isSegmentMirror.return_value = False 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_persistent_table.gparray = m
        self.assertFalse(self.rebuild_persistent_table._validate_has_mirrors())

    def test_validate_has_mirrors_with_mirrors_for_master(self):
        mock_segs = []
        for i in range(6):
            m = Mock()
            m.getSegmentContentId.return_value = i - 1
            m.isSegmentMirror.return_value = True if i == -1 else False 
            mock_segs.append(m)
        m = Mock()
        m.getDbList.return_value = mock_segs 
        self.rebuild_persistent_table.gparray = m
        self.assertFalse(self.rebuild_persistent_table._validate_has_mirrors())

    @patch('gppylib.operations.persistent_rebuild.findCmdInPath', return_value=True)
    def test_check_md5_prog(self, mock1):
        self.rebuild_persistent_table._check_md5_prog()

    @patch('gppylib.operations.persistent_rebuild.findCmdInPath', return_value=False)
    def test_check_md5_prog_no_md5(self, mock1):
        with self.assertRaisesRegexp(Exception, 'Unable to find md5.* program. Please make sure it is in PATH'):
            self.rebuild_persistent_table._check_md5_prog()

    @patch('gppylib.operations.persistent_rebuild.GpVersion.local', return_value=GpVersion('4.2.7.3'))
    def test_check_database_version(self, mock1):
        self.rebuild_persistent_table._check_database_version()

    @patch('gppylib.operations.persistent_rebuild.GpVersion.local', return_value=GpVersion('4.0.1.0'))
    def test_check_database_version_with_lower_version(self, mock1):
        with self.assertRaisesRegexp(Exception, 'This tool is not supported on Greenplum version lower than 4.1.0.0'):
            self.rebuild_persistent_table._check_database_version()

    @patch('gppylib.operations.persistent_rebuild.dbconn.execSQL', side_effect=[[[5090], [5091], [5092], [5093]], [[123, 'template1']],
                                                                               [[5094], [16992]]])
    @patch('gppylib.operations.persistent_rebuild.dbconn.connect')
    @patch('gppylib.operations.persistent_rebuild.dbconn.DbURL')
    def test_get_persistent_table_filenames(self, mock1, mock2, mock3):
        self.rebuild_persistent_table._get_persistent_table_filenames()
        from gppylib.operations.persistent_rebuild import GLOBAL_PERSISTENT_FILES, PER_DATABASE_PERSISTENT_FILES
        self.assertEqual(GLOBAL_PERSISTENT_FILES, [5090, 5091, 5092, 5093])
        self.assertEqual(PER_DATABASE_PERSISTENT_FILES, defaultdict(list, {123: [5094, 16992]}))

if __name__ == '__main__':
    unittest.main()
