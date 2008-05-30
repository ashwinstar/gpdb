#!/usr/bin/env python

import sys, httplib
if len(sys.argv) != 2: 
    sys.exit('usage: %s uri' % sys.argv[0])

uri = sys.argv[1]
x = ''

try:
    conn = httplib.HTTPConnection(uri)
    conn.request('GET', '/')
    r = conn.getresponse()
    x = r.getheader('X-GPFDIST-VERSION', '')
except:
    sys.exit('failed: gpfdist is not running (reason 1).')

if not x:
    sys.exit('failed: gpfdist is not running (reason 2).')

print 'Okay, gpfdist version "%s" is running on %s.' % (x, uri)
