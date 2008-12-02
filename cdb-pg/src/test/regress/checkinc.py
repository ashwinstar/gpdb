#!/usr/bin/env python

import sys, os, re, subprocess

p = subprocess.Popen('pg_config --pkgincludedir', stdout=subprocess.PIPE, shell=True)
pkginc = p.communicate()[0].strip();
include = re.compile('\s*#include\s*["<](\S*)[">]')

# Hard code standard includes that are okay but we can't check via standard methods.
# Mostly port specific includes.
fileset = {
    'abi_mutex.h':       [],
    'alpha/builtins.h':  [],
    'crtdefs.h':         [],
    'cygwin/version.h':  [],
    'direct.h':          [],
    'ia64/sys/inline.h': [],
    'io.h':              [],
    'libintl.h':         [],
    'libc.h':            [],
    'mutex.h':           [],
    'pipe.h':            [],
    'process.h':         [],
    'pthread-win32.h':   [],
    'solaris.h':         [],
    'SupportDefs.h':     [],
    'sys/isa_defs.h':    [],
    'sys/machine.h':     [],
    'sys/utime.h':       [],
    'windows.h':         [],
    'winsock2.h':        [],
    'ws2tcpip.h':        [],
}


# Find all the files in the shipped include directories
inc_dirs = [os.path.join(pkginc, 'server'), os.path.join(pkginc, 'internal')]
for inc_dir in inc_dirs:
    for (root, dirs, files) in os.walk(inc_dir, topdown=False):
        # For every file make a list of #includes
        for f in files:
            froot = os.path.join(root,f)
            fname = froot.replace(inc_dir+'/','')
            file = open(froot, 'r')
            fileset[fname] = []
            for line in file:
                m = include.match(line)
                if m:
                    fileset[fname].append(m.group(1))


# For all files, check to see if the list of includes is in the list
missing = False
keys = fileset.keys()
keys.sort()
for f in keys:
    for i in filter(lambda(x): None == fileset.get(x), fileset[f]):
        # There are a couple reasons an included file might not be listed:

        # 1. It might be a standard include
        if os.path.exists(os.path.join('/usr/include',i)):
            continue

        # 2. It might not have been well qualified
        (d,s) = os.path.split(f)
        if fileset.get(os.path.join(d,i)):
            continue

        # But if it's not one of those then it's probably a file that we have
        # omitted
        missing = True
        print '  %-30s includes missing file "%s" ' % (f, i)

if missing:
    sys.exit(1)
else:
    print "Include files are ok" 
    sys.exit(0)
