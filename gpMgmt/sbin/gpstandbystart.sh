#!/bin/bash
DATADIR=$1
PORT=$2
exec $GPHOME/bin/gpsyncmaster -D $DATADIR -p $PORT -i >> $DATADIR/pg_log/startup.log 2>&1 &
