import os
import unittest2 as unittest

from gppylib.commands.base import Command, REMOTE, CommandResult
from gppylib.commands.unix import getLocalHostname
from gppylib.operations.gpcheck import get_host_for_command, get_command, get_copy_command
from mock import patch, MagicMock, Mock

gphome = os.environ.get('GPHOME')
if gphome is None:
    raise Exception('GPHOME needs to be set !')

class GpCheckTestCase(unittest.TestCase):
    def test_get_host_for_command00(self):
        cmd = Command('name', 'hostname', ctxt=REMOTE, remoteHost='foo') 
        result = get_host_for_command(False, cmd)
        expected_result = 'foo'
        self.assertEqual(result, expected_result)

    def test_get_host_for_command01(self):
        cmd = Command('name', 'hostname') 
        cmd.run(validateAfter=True)
        hostname = cmd.get_results().stdout.strip()
        result = get_host_for_command(True, cmd)
        expected_result = hostname 
        self.assertEqual(result, expected_result)

    def test_get_command00(self):
        host = 'foo'
        cmd = 'bar'
        result = get_command(True, cmd, host)
        expected_result = Command(host, cmd)
        self.assertEqual(result.name, expected_result.name)
        self.assertEqual(result.cmdStr, expected_result.cmdStr)

    def test_get_command01(self):
        host = 'foo'
        cmd = 'bar'
        result = get_command(False, cmd, host)
        expected_result = Command(host, cmd, ctxt=REMOTE, remoteHost=host)
        self.assertEqual(result.name, expected_result.name)
        self.assertEqual(result.cmdStr, expected_result.cmdStr)

    def test_get_copy_command00(self):
        host = 'foo'
        datafile = 'bar'
        tmpdir = '/tmp/foobar'
        result = get_copy_command(False, host, datafile, tmpdir)
        expected_result = Command(host, 'scp %s:%s %s/%s.data' % (host, datafile, tmpdir, host))
        self.assertEqual(result.name, expected_result.name)
        self.assertEqual(result.cmdStr, expected_result.cmdStr)

    def test_get_copy_command01(self):
        host = 'foo'
        datafile = 'bar'
        tmpdir = '/tmp/foobar'
        result = get_copy_command(True, host, datafile, tmpdir)
        expected_result = Command(host, 'mv -f %s %s/%s.data' % (datafile, tmpdir, host))
        self.assertEqual(result.name, expected_result.name)
        self.assertEqual(result.cmdStr, expected_result.cmdStr)

