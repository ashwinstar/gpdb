#!/bin/bash
#
# gpmigrator requires calling itself on individual hosts, but it is a python script
# so it needs certain environment variables to be setup before it can successfully
# execute itself.
#
# This is a bash script (since /bin/bash is guaranteed) that sets up the environment
# needed to execute a python script.  It uses the path to itself as the basis from
# which it determines GPHOME.

# Get the absolute path to this script
absPath=`cd $(dirname $0); pwd`

# absPath is $GPHOME/bin/lib, so backup two directories to find $GPHOME
GPHOME=$(dirname $(dirname $absPath))

# get the PATH setup
PATH=$GPHOME/bin:$GPHOME/ext/python/bin:$PATH

# Add in library paths apropriate for this system
UNAME=`uname`

case $UNAME in
  Darwin) DYLD_LIBRARY_PATH=$GPHOME/ext/python/lib:$DYLD_LIBRARY_PATH ;;
  *) LD_LIBRARY_PATH=$GPHOME/ext/python/lib:$LD_LIBRARY_PATH ;;
esac

# execute whatever was given to us

if [ "$#" -gt 0 ]
then
  $*
else
  env
fi