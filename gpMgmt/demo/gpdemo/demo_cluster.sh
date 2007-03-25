#!/bin/bash

#set -x


#
# DATABASE PORTS
#

#
# Data Directories
#
DATADIRS=`pwd`/datadirs
QDDIR=$DATADIRS/qddir
declare -a DIRVEC=($DATADIRS/dbfast1 $DATADIRS/dbfast2 $DATADIRS/dbfast3)

declare -a DEMO_SEG_PORTS=(`expr $DEMO_PORT_BASE + 1 `  \
        `expr $DEMO_PORT_BASE + 2` `expr  $DEMO_PORT_BASE + 3 `)



#******************************************************************************
# Functions
#******************************************************************************
checkDemoConfig(){
    echo "Checking for port availability... "
    # Check if Master_DEMO_Port is free
    echo "Checking if port ${MASTER_DEMO_PORT} is available"
    PORT_FILE="/tmp/.s.PGSQL.${MASTER_DEMO_PORT}"
    if [ -f ${PORT_FILE} -o  -S ${PORT_FILE} ] ; then 
        echo ""
        echo -n " Port ${MASTER_DEMO_PORT} appears to be in use. " 
        echo " This port is needed by the Master Database instance. "
        echo ">>> Edit Makefile to correct the port number (MASTER_PORT). <<<" 
        echo -n " Check to see if the port is free by using : "
        echo " 'netstat -an | grep ${MASTER_DEMO_PORT}"
        echo ""
        return 1
    fi

    for PORT_NUM in ${DEMO_SEG_PORTS[@]} 
    do
        echo "Checking if port ${PORT_NUM} is available"
        PORT_FILE="/tmp/.s.PGSQL.${PORT_NUM}"
        if [ -f ${PORT_FILE} -o -S ${PORT_FILE} ] ; then 
            echo ""
            echo -n "Port ${PORT_NUM} appears to be in use."
            echo " This port is needed for segment database instance."
            echo ">>> Edit Makefile to correct the base ports (PORT_BASE). <<<"
            echo -n " Check to see if the port is free by using : "
            echo " 'netstat -an | grep ${PORT_NUM}"
            echo ""
            return 1
        fi
    done
    return 0
}

USAGE(){
    echo ""
    echo " `basename $0` -c -d -u"
    echo " -c : check if demo is possible."
    echo " -d : Delete the demo."
    echo " -u : Usage, prints this message."
    echo ""
}

#
# Clean up the demo
#
cleanDemo(){
    PG_CTL_PATH=${GPHOME}/bin/pg_ctl
    # Data Directories for the segments
    if [ -f ${QDDIR}/demoDataDir-1/postmaster.pid ] ; then 
        echo "stopping postmaster ${PG_CTL_PATH} -D ${QDDIR}/demoDataDir-1 stop"
        ${PG_CTL_PATH} -D ${QDDIR}/demoDataDir-1 stop
    fi
    declare -a SEGDIRS=($DATADIRS/dbfast1/demoDataDir0 $DATADIRS/dbfast2/demoDataDir1 $DATADIRS/dbfast3/demoDataDir2)
    for eDir in ${SEGDIRS[@]}
    do
        if [ -f ${eDir}/postmaster.pid ] ; then 
            echo "stopping postmaster ${PG_CTL_PATH} -D ${eDir} stop"
            ${PG_CTL_PATH} -D ${eDir} stop
        fi
    done
    # Remove the files and directories created
    if [ -f hostfile ];  then
        echo "Deleting hostfile"
        rm -f hostfile
    fi
    if [ -f clusterConfigFile ];  then
        echo "Deleting clusterConfigFile"
        rm -f clusterConfigFile
    fi
    if [ -d ${DATADIRS} ];  then
        echo "Deleting ${DATADIRS}"
        rm -rf ${DATADIRS}
    fi
    if [ -d logs ];  then
        rm -rf logs
    fi
}

#*****************************************************************************
# Main Section
#*****************************************************************************
while getopts ":cd'?'" opt
		do
		case $opt in 
				'?' ) USAGE ;;
                c) checkDemoConfig
                    RETVAL=$?
                    if [ $RETVAL -ne 0 ]; then
                        echo "Checking failed "
                        exit 1
                    fi
                    exit 0
                ;;
                d) cleanDemo
                   exit 0
                ;;
                *) USAGE
                   exit 0
                   ;;
		esac
done


echo ""
echo "**************************************************************************************************"
echo "This is a demo of the Greenplum Database module.  We'll perform a local cluster installation with 1"
echo "master instance and 3 segment instances all on this machine."
echo ""
echo "In order to run the Greenplum Database module, you must either have the BIZHOME environment variable"
echo "set, or the GPHOME environment variable set to the location of the Greenplum install."
echo "You must also have the following port numbers free ${MASTER_DEMO_PORT} ${DEMO_SEG_PORTS[@]}"
echo ""
echo "**************************************************************************************************"
echo ""



if [ x"$GPHOME" = x ]; then
    if [ x"$BIZHOME" = x ]; then
        echo "Neither GPHOME or BIZHOME are set.  Set one of these variables to point to the location"
     echo "of the Greenplum installation directory."
        echo ""
        exit 1
    else
        GPSEARCH=$BIZHOME
    fi
else
    GPSEARCH=$GPHOME
fi

GPPATH=`find $GPSEARCH -name gp_dump| tail -1`
RETVAL=$?

if [ "$RETVAL" -ne 0 ]; then
    echo "Error attempting to find Greenplum executables in $GPSEARCH"
    exit 1
fi

if [ ! -x "$GPPATH" ]; then
    echo "No executables found for Greenplum installation in $GPSEARCH"
    exit 1
fi
GPPATH=`dirname $GPPATH`
GPMGMT=$GPPATH/../gpMgmt/bin
if [ ! -x $GPMGMT/gpcreatecluster.sh ]; then
    echo "No mgmt executables found for Greenplum installation in $GPMGMT"
    exit 1
fi




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
echo "# This file must exist in the same directory that you execute gpcreatecluster.sh in" >> $CLUSTER_CONFIG
echo "MACHINE_LIST_FILE=hostfile" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# This names the data directories for the Segment Instances and the Entry Postmaster" >> $CLUSTER_CONFIG
echo "SEG_PREFIX=$SEG_PREFIX" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# This is the port at which to contact the resulting Greenplum database, e.g." >> $CLUSTER_CONFIG
echo "#   psql -p \$PORT_BASE template1" >> $CLUSTER_CONFIG
echo "PORT_BASE=${DEMO_PORT_BASE}" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Prefix for script created database" >> $CLUSTER_CONFIG
echo "DATABASE_PREFIX=demoDatabase" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Array of data locations for each hosts Segment Instances, the number of directories in this array will" >> $CLUSTER_CONFIG
echo "# set the number of segment instances per host" >> $CLUSTER_CONFIG
echo "declare -a DATA_DIRECTORY=(${DIRVEC[@]})" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Name of host on which to setup the QD" >> $CLUSTER_CONFIG
echo "MASTER_HOSTNAME=$LOCALHOST" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "# Name of directory on that host in which to setup the QD" >> $CLUSTER_CONFIG
echo "MASTER_DIRECTORY=$QDDIR" >> $CLUSTER_CONFIG
echo "MASTER_DATA_DIRECTORY=$QDDIR/${SEG_PREFIX}0" >> $CLUSTER_CONFIG
echo "" >> $CLUSTER_CONFIG
echo "MASTER_PORT=${MASTER_DEMO_PORT}" >> $CLUSTER_CONFIG
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
echo "# Path for Greenplum mgmt utils and Greenplum binaries" >> $CLUSTER_CONFIG
echo "PATH=$GPHOME/bin:$GPHOME/gpMgmt/bin:$PATH" >> $CLUSTER_CONFIG
echo "LD_LIBRARY_PATH=$GPHOME/lib:$LD_LIBRARY_PATH" >> $CLUSTER_CONFIG
echo "export PATH" >> $CLUSTER_CONFIG
echo "export LD_LIBRARY_PATH" >> $CLUSTER_CONFIG
echo "export MASTER_DATA_DIRECTORY" >> $CLUSTER_CONFIG
echo "export TRUSTED_SHELL" >> $CLUSTER_CONFIG

#*****************************************************************************************
# Create cluster
#*****************************************************************************************
$GPMGMT/gpcreatecluster.sh -q -a -c $CLUSTER_CONFIG
