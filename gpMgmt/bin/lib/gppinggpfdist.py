#!/usr/bin/env python

import sys, httplib, getopt

def usage(exitarg):
    print 'usage: %s [-q] uri' % sys.argv[0]
    print 
    print '   -q: quiet mode'
    print
    print 'e.g. %s localhost:8080' % sys.argv[0]
    print
    sys.exit(exitarg)

gpfdist = ''
quiet = 0
uri = ''

try:
    (options, args) = getopt.getopt(sys.argv[1:], 'q')
except Exception, e:
    usage('Error: ' + str(e))

for (switch, val) in options:
    if (switch == '-q'):  quiet = 1

if len(args) != 1:
    usage('Error: please specify uri.')

uri = args[0]

try:
    conn = httplib.HTTPConnection(uri)
    conn.request('GET', '/')
    r = conn.getresponse()
    gpfdist = r.getheader('X-GPFDIST-VERSION', '')
except:
    sys.exit(quiet and 1 or 'failed: gpfdist is not running (reason 1).')

if not gpfdist:
    sys.exit(quiet and 2 or 'failed: gpfdist is not running (reason 2).')

if not quiet:
    print 'Okay, gpfdist version "%s" is running on %s.' % (gpfdist, uri)
sys.exit(0)
