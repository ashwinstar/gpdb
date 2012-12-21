#!/usr/bin/env python

import os, signal, time, re
import unittest2 as unittest
import psi.process, subprocess

class GpsshTestCase(unittest.TestCase):

    # return true if any processes remaining
    def searchForProcessOrChildren(self):

        euid = os.getuid()

        for p in psi.process.ProcessTable().values():
            if p.euid != euid:
                continue
            if not re.search('ssh', p.command):
                continue
            if p.ppid != 1:
                continue

            return p.pid


    def test00_gpssh_sighup(self):
        """Verify that gppsh handles sighup 
           and terminates cleanly.
        """

        p = subprocess.Popen("gpssh -h localhost", shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pid = p.pid

        time.sleep(3)

        try:
            os.kill(int(pid), signal.SIGHUP)
        except Exception:
            pass

        max_attempts = 6
        for i in range(max_attempts):
            pid = self.searchForProcessOrChildren()
            if pid:
                if (i + 1) == max_attempts:
                    self.fail("Found stranded gpssh process %s after issuing sig HUP" % pid)
                time.sleep(.5)
 
if __name__ == "__main__":
    unittest.main()
