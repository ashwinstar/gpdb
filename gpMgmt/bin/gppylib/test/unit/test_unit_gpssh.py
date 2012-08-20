#!/usr/bin/env python

import os, signal, time
import unittest2 as unittest

from gppylib.commands.base import Command, WorkerPool
from gppylib.commands.unix import getUserName

class GpsshTestCase(unittest.TestCase):

    SECONDS_TO_SLEEP = 5 
    SSH_COMMAND = '/usr/bin/ssh -o BatchMode yes -o StrictHostKeyChecking no -l %s localhost' % getUserName() 
    def getpid(self, cmd):

        ps_cmd = Command(name='grep for cmd', cmdStr='ps aux | grep "%s$"' % cmd)

        try:
            ps_cmd.run(validateAfter=True)
        except Exception, e:
            pass

        if ps_cmd.get_results().rc != 0:
            return None

        ps_info = ps_cmd.get_results().stdout.strip() 
        return ps_info.split()[1]

    def test00_gpssh_sighup(self):
        """Verify that gppsh handles sighup 
           and terminates cleanly.
        """
        cmd = Command(name='run ssh', cmdStr='gpssh -h localhost -e sleep %s' % self.SECONDS_TO_SLEEP)

        pool = WorkerPool()
        pool.addCommand(cmd)

        pid = self.getpid(cmd.cmdStr)
        self.assertNotEqual(pid, None) 
        self.assertNotEqual(None, self.SSH_COMMAND) 

        os.kill(int(pid), signal.SIGHUP)

        self.assertEqual(None, self.getpid(self.SSH_COMMAND))

if __name__ == "__main__":
    unittest.main()
