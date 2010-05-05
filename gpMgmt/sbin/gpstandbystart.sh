#!/bin/bash
DATADIR=$1
PORT=$2
DBID=$3
CONTENTID=-1 # always to the case for a master
CLUSTERCONTENTS=$4
exec $GPHOME/bin/gpsyncmaster -D $DATADIR \
	-p $PORT \
	-b $DBID \
	-C $CONTENTID \
	-z $CLUSTERCONTENTS \
	-i >> $DATADIR/pg_log/startup.log 2>&1 &
