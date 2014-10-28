#!/usr/bin/env python

import os
import unittest2 as unittest

from gppylib.mainUtils import ExceptionNoStackTraceNeeded
from gppylib.programs.clsRecoverSegment import GpRecoverSegmentProgram
from mock import MagicMock, Mock, mock_open, patch

class GpRecoverSegmentProgramTestCase(unittest.TestCase):

    def _get_mock_segment(self, name, port, address, datadir):
        m = Mock()
        m.getSegmentHostName.return_value = name
        m.getSegmentAddress.return_value = address 
        m.getSegmentPort.return_value = port
        m.getSegmentDatadirectory.return_value = datadir
        return m

    @patch('gppylib.programs.clsRecoverSegment.GpRecoverSegmentProgram._get_dblist', return_value=[['template1']])
    def test_check_persistent_tables(self, mock1):
        options = Mock()
        segments = [self._get_mock_segment('seg1', '1234', 'seg1', '/tmp/seg1'), self._get_mock_segment('seg2', '2345', 'seg2', '/tmp/seg2')]
        mock_pool = Mock()
        m1 = Mock()
        m1.get_results.return_value = ''
        m2 = Mock()
        m2.get_results.return_value = ''
        mock_pool.getCompletedItems.return_value = [m1, m2]
        gprecover_prog = GpRecoverSegmentProgram(options)
        gprecover_prog._GpRecoverSegmentProgram__pool = mock_pool 
        gprecover_prog._check_persistent_tables(segments)

    @patch('gppylib.programs.clsRecoverSegment.GpRecoverSegmentProgram._get_dblist', return_value=[['template1']])
    def test_check_persistent_tables_no_segments(self, mock1):
        options = Mock()
        segments = []
        mock_pool = Mock() 
        mock_pool.getCompletedItems.return_value = []
        gprecover_prog = GpRecoverSegmentProgram(options)
        gprecover_prog._GpRecoverSegmentProgram__pool = mock_pool 
        gprecover_prog._check_persistent_tables(segments)

    @patch('gppylib.programs.clsRecoverSegment.GpRecoverSegmentProgram._get_dblist', return_value=[['template1']])
    def test_check_persistent_tables_error(self, mock1):
        options = Mock()
        segments = [self._get_mock_segment('seg1', '1234', 'seg1', '/tmp/seg1'), self._get_mock_segment('seg2', '2345', 'seg2', '/tmp/seg2')]
        mock_pool = Mock()
        m1 = Mock()
        m1.get_results.return_value = ['sdfsdf']
        m2 = Mock()
        m2.get_results.return_value = ['asdfas']
        mock_pool.getCompletedItems.return_value = [m1, m2]
        gprecover_prog = GpRecoverSegmentProgram(options)
        gprecover_prog._GpRecoverSegmentProgram__pool = mock_pool 
        with self.assertRaisesRegexp(Exception, 'Please fix the persistent tables issue'):
            gprecover_prog._check_persistent_tables(segments)
