#!/usr/bin/env python
import sys
import pickle

class NullDevice():
    def write(self, s):
        pass

if __name__ == '__main__':
    # Prevent use of stdout, as it disrupts pickling mechanism
    old_stdout = sys.stdout
    sys.stdout = NullDevice()

    try:
        # log initialization must be done only AFTER rerouting stdout
        from gppylib import gplog
        from gppylib.mainUtils import getProgramName
        from gppylib.commands import unix
        hostname = unix.getLocalHostname()
        username = unix.getUserName()
        execname = pickle.load(sys.stdin)
        gplog.setup_tool_logging(execname, hostname, username)

        operation = pickle.load(sys.stdin)

        ret = operation.run()
    except Exception, e:
        sys.stdout = old_stdout
        print pickle.dumps(e)
    else:
        sys.stdout = old_stdout
        print pickle.dumps(ret)
