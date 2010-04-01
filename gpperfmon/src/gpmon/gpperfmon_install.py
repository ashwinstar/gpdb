#!/usr/bin/env python

import os, sys
from subprocess import Popen

def run_command(cmdstr):
    p = Popen(cmdstr, shell=True)
    sts = os.waitpid(p.pid, 0)[1]
    if sts:
        raise Exception("error on cmd " + cmdstr)

###### main()
if __name__ == '__main__':

    gphome = os.environ.get('GPHOME')
    if not gphome:
        raise Exception("GPHOME not set")

    # 3.X gpperfmon schema
    cmd1 = "psql -f %s/lib/gpperfmon/gpperfmon3.sql template1" % gphome
    
    # 4.x gpperfmon schema additions
    cmd2 = "psql -f %s/lib/gpperfmon/gpperfmon4.sql template1" % gphome
    
    # gpperfmon schema completion tasks
    cmd3 = "psql -f %s/lib/gpperfmon/gpperfmonC.sql template1" % gphome
    
command = None
try:
    for c in (cmd1, cmd2, cmd3):
        command = c
        run_command(command)
except:
    print "\n==============================================="
    print "error on command: %s" % command
    print "check to make sure the DB is running and accessible"
    print "gpperfmon schema not successfully installed"
    sys.exit(1)

print "--gpperfmon schema successfully installed--"
