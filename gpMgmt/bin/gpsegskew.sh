#!/bin/bash
#	Filename:-		gpsegskew.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Dec 2006
#	Release stat:-		Released
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright MPP database cluster
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code

WORKDIR=`dirname $0`

# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
if [ -f $FUNCTIONS ]; then
		. $FUNCTIONS
else
		echo "[FATAL]:-Cannot source $FUNCTIONS file Script Exits!"
		exit 2
fi
#******************************************************************************
# Script Specific Variables
#******************************************************************************
# Log file that will record script actions
CUR_DATE=`$DATE +%Y%m%d`
TIME=`$DATE +%H%M%S`
PROG_NAME=`$BASENAME $0`
# Level of script feedback 0=small 1=verbose
unset VERBOSE
GP_USER=$USER_NAME
EXIT_STATUS=0
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpskew.sh, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}


GET_TABLE_COUNT () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	QE_BASE_DIR=`$BASENAME $QE_DIR`
	#Test access
	ACCESS_TEST=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c\"select version();\" 2>/dev/null"`
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		LOG_MSG "[WARN][$INST_COUNT]:-Failed to get table count on $QE_NAME $QE_DIR QE_PORT"
		QE_LINE="FAILED|0|${QE_PORT}|${QE_BASE_DIR}|${QE_NAME}|0m0s|"
	else
		COUNT_LINE=`$TRUSTED_SHELL -n $QE_NAME "${EXPORT_LIB_PATH}; time env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c'select count(*),'\''$QE_PORT'\'','\''$QE_BASE_DIR'\'','\''$QE_NAME'\'' from $TARGETTBL;'" 2>&1|$HEAD -3|$TR -s '\n'|$TR ' ' '\n'|$TR -d '.'|$TR -cs '[A-Z][a-z][0-9]' '[\n*]'|$GREP -v real|$TR  '\n' '|'|$AWK '{print $1}'` 
		QE_LINE="COMPLETED|$COUNT_LINE"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}


#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED|0|${QE_PORT}|${QE_BASE_DIR}|${QE_NAME}|0m0s|" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
PARENT_PID=$1;shift		#PID of gpstate.sh process calling this script
CHK_CALL
QE_NAME=$1;shift		#Hostname holding database segment
QE_PORT=$1;shift		#Segment port
QE_DIR=$1;shift			#Segment directory
QE_VALID=$1;shift		#Status of segment either t or f
QE_PRIMARY=$1;shift
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift
QD_DBNAME=$1;shift
TARGETTBL=$1;shift
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
#QE_LINE="${QE_NAME}:${QE_PORT}:${QE_DIR}:${QE_PRIMARY}:${QE_VALID}"
GET_TABLE_COUNT
$ECHO "$QE_LINE" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
