#!/bin/bash

#***********************************************************
# Look for MPP executables to find path
#***********************************************************
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

MPPPATH=`find $MPPSEARCH -name mpp_dump | tail -1`
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
#***********************************************************

#***********************************************************
# Create a list of catalog tables to printout
#***********************************************************
declare -a TABLES=(mpp host listener pgdatabase policy version)

#***********************************************************
# Declare the max and min port numbers to probe
#***********************************************************
declare -a PORTS=(5432 10001 10002 10003)
((PORT_MIN=0))
((PORT_MAX=3))

#***********************************************************
# Loop over all ports and all tables, printing out their
# contents
#***********************************************************
for ((i=PORT_MIN; i<PORT_MAX+1; i++))
do
  echo "******************************************************************************************************"
  echo "******************************************************************************************************"
  echo "Probing segment instance at port number $i"
  echo "******************************************************************************************************"
  echo "******************************************************************************************************"
  for table in ${TABLES[@]}
  do
    echo "**************************"
    echo "Table: mpp.$table"
    PGOPTIONS="-c mpp_session_role=utility" $MPPPATH/psql -p ${PORTS[$i]} template1 -c \
    "select * from mpp."$table
	RETVAL=$?
	if [ $RETVAL -ne 0 ]; then
	  echo "$0 failed."
	  exit 1
    fi
  done
  echo "******************************************************************************************************"
  echo "******************************************************************************************************"
done

