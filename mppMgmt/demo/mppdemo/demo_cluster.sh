#!/bin/bash

echo ""
echo "**************************************************************************************************"
echo "This is a demo of the Bizgres MPP module.  We'll perform a local cluster installation with 1"
echo "master instance and 3 segment instances all on this machine."
echo ""
echo "In order to run the Bizgres MPP module, you must either have the BIZHOME environment variable"
echo "set, or the MPPHOME environment variable set to the location of the MPP install."
echo ""
echo "**************************************************************************************************"
echo ""

if [ x"$MPPHOME" = x ]; then
  if [ x"$BIZHOME" = x ]; then
    echo "Neither MPPHOME or BIZHOME are set.  Set one of these variables to point to the location"
	echo "of the MPP installation directory."
    echo ""
	exit 1
  else
    MPPSEARCH=$BIZHOME
  fi
else
  MPPSEARCH=$MPPHOME
fi

MPPPATH=`find $MPPSEARCH -name cdb_dump | tail -1`
RETVAL=$?

if [ "$RETVAL" -ne 0 ]; then
  echo "Error attempting to find MPP executables in $MPPSEARCH"
  exit 1
fi

if [ ! -x "$MPPPATH" ]; then
  echo "No executables found for MPP installation in $MPPSEARCH"
  exit 1
fi
MPPPATH=`dirname $MPPPATH`
MPPMGMT=$MPPPATH/../mppMgmt/bin
if [ ! -x $MPPMGMT/mppcreatecluster.sh ]; then
  echo "No mgmt executables found for MPP installation in $MPPMGMT"
  exit 1
fi

#*****************************************************************************************
# Set up data directories
#*****************************************************************************************
DATADIRS=`pwd`/datadirs
QDDIR=$DATADIRS/qddir
declare -a DIRVEC=($DATADIRS/dbfast1 $DATADIRS/dbfast2 $DATADIRS/dbfast3)

if [ -d $DATADIRS ]; then
  rm -rf $DATADIRS
fi
mkdir $DATADIRS
mkdir $QDDIR

for dir in ${DIRVEC[@]}
do
  if [ ! -d $dir ]; then
    mkdir $dir
  fi
done

#*****************************************************************************************
# Host configuration
#*****************************************************************************************
LOCALHOST=`hostname`
echo "localhost" > hostfile

#*****************************************************************************************
# Name of the cluster configuration file.
#*****************************************************************************************
CLUSTER_CONFIG=clusterConfigFile
rm -f $CLUSTER_CONFIG

SEG_PREFIX=demoDataDir

#*****************************************************************************************
# Create the cluster configuration file
#*****************************************************************************************
echo "# Set this to anything you like" >> $CLUSTER_CONFIG
echo "CLUSTER_NAME=\"Demo $HOSTNAME Cluster\"" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# This file must exist in the same directory that you execute mppcreatecluster.sh in" >> $CLUSTER_CONFIG
echo "MACHINE_LIST_FILE=hostfile" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# This names the data directories for the Segment Instances and the Entry Postmaster" >> $CLUSTER_CONFIG
echo "SEG_PREFIX=$SEG_PREFIX" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# This is the port at which to contact the resulting MPP database, e.g." >> $CLUSTER_CONFIG
echo "#   psql -p \$PORT_BASE template1" >> $CLUSTER_CONFIG
echo "PORT_BASE=10000" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Prefix for script created database" >> $CLUSTER_CONFIG
echo "DATABASE_PREFIX=demoDatabase" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Array of data locations for each hosts Segment Instances, the number of directories in this array will" >> $CLUSTER_CONFIG
echo "# set the number of segment instances per host" >> $CLUSTER_CONFIG
echo "declare -a DATA_DIRECTORY=(${DIRVEC[@]})" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Name of host on which to setup the QD" >> $CLUSTER_CONFIG
echo "QD_HOSTNAME=$LOCALHOST" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Name of directory on that host in which to setup the QD" >> $CLUSTER_CONFIG
echo "QD_DIRECTORY=$QDDIR" >> $CLUSTER_CONFIG
echo "QD_DATA_DIRECTORY=$QDDIR/${SEG_PREFIX}0" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "QD_PORT=5432" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Hosts to allow to connect to the QD (and Segment Instances)" >> $CLUSTER_CONFIG
echo "# By default, allow everyone to connect (0.0.0.0/0)" >> $CLUSTER_CONFIG
echo "IP_ALLOW=0.0.0.0/0" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Shell to use to execute commands on all hosts" >> $CLUSTER_CONFIG
echo "TRUSTED_SHELL=\"`pwd`/lalshell\"" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "CHECK_POINT_SEGMENTS=8" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "ENCODING=UNICODE" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Path for MPP mgmt utils and MPP binaries" >> $CLUSTER_CONFIG
echo "PATH=$MPPHOME/bin:$MPPHOME/mppMgmt/bin:$PATH" >> $CLUSTER_CONFIG
echo "LD_LIBRARY_PATH=$MPPHOME/lib:$LD_LIBRARY_PATH" >> $CLUSTER_CONFIG
echo "export PATH" >> $CLUSTER_CONFIG
echo "export LD_LIBRARY_PATH" >> $CLUSTER_CONFIG
echo "export QD_DATA_DIRECTORY" >> $CLUSTER_CONFIG
echo "export TRUSTED_SHELL" >> $CLUSTER_CONFIG

#*****************************************************************************************
# Create cluster
#*****************************************************************************************
$MPPMGMT/mppcreatecluster.sh -q -a -c $CLUSTER_CONFIG
