#!/bin/bash
#	Filename:-		gpsegupdate.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Nov 2007
#	Release stat:-		Released
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) 2007 Greenplum Inc
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
BACKOUT_FILE=/tmp/${PROG_NAME}_backout.$$
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpdbupgrade, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

PING_CHK () {
        PING_HOST $QE_NAME 1
        if [ $RETVAL -ne 0 ];then
                $ECHO "FAILED:${QE_LINE}:PING_FAILED" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[INFO][$INST_COUNT]:-End Main"
		exit $RETVAL
        fi
}

QE_DIR_CHK () {
	CHK_DIR ${QE_DIR} $QE_NAME
	if [ $EXISTS -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:NO_DIR" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[INFO][$INST_COUNT]:-End Main"
		exit 1
	fi
}

START_SEG () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO][$INST_COUNT]:-Running control file upgrader"
	EXTRA_START_OPTS=" -c allow_system_table_mods=true "
	$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};$GPCONTROLMOD --catversion $UPGRADE_TO $QE_DIR" >> $LOG_FILE 2>&1
	RETVAL=$?
	if [ $RETVAL -ne 0 ]; then
		LOG_MSG "[FATAL][$INST_COUNT]:-Upgrade $QE_NAME directory $QE_DIR version $UPGRADE_TO failed"
		QE_LINE="FAILED:${QE_LINE}:UPGRADE_FAILED"
		$ECHO $QE_LINE  >> $PARALLEL_STATUS_FILE
		exit 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Upgrade $QE_NAME directory $QE_DIR version $UPGRADE_TO completed"
		BACKOUT_COMMAND "$ECHO \"Reverting QE $QE_NAME:$QE_DIR control file to $OLD_VERSION\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $QE_NAME \"$GPCONTROLMOD --catversion $OLD_VERSION $QE_DIR\""	
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Starting Segment instance $QE_NAME directory $QE_DIR"
	$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};export PGPATH=${APP_PATH};export PGPORT=${QE_PORT};$PG_CTL -w -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT} $EXTRA_START_OPTS\" start >/dev/null 2>&1"
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		LOG_MSG "[WARN][$INST_COUNT]:-Startup request returned error code $RETVAL" 1
		LOG_MSG "[INFO][$INST_COUNT]:-Command string called which resulted in error" 1
		LOG_MSG "[INFO][$INST_COUNT]:-$TRUSTED_SHELL $QE_NAME ${EXPORT_LIB_PATH};export PGPATH=${APP_PATH};export PGPORT=${QE_PORT};$PG_CTL -w -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT} $EXTRA_START_OPTS\" start"  1
		LOG_MSG "[INFO][$INST_COUNT]:-Tail'ing Segment instance log file" 1
		$TRUSTED_SHELL $QE_NAME "$LS $QE_DIR" > /dev/null 2>&1
		if [ $? -ne 0 ]; then
			LOG_MSG "[WARN][$INST_COUNT]:-Segment directory $QE_DIR on $QE_NAME does not exist" 1
			LOG_MSG "[WARN][$INST_COUNT]:-Unable to tail segment log file" 1
		else
			LOG_MSG "[INFO][$INST_COUNT]:-Start diagnostic data dump to log file" 1
			LOG_MSG "[INFO][$INST_COUNT]:-------------------------------------------------" 1
			$TRUSTED_SHELL $QE_NAME "$TAIL ${QE_DIR}.log" >> $LOG_FILE
			LOG_MSG "[INFO][$INST_COUNT]:-End of diagnostic data dump" 1
			LOG_MSG "[INFO][$INST_COUNT]:-------------------------------------------------" 1
		fi
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed to start segment $QE_HOST $DE_DIR $QE_PORT" 1
		QE_LINE="FAILED:${QE_LINE}:START_FAIL"
		$ECHO "FATAL:$QE_LINE" >> $PARALLEL_STATUS_FILE
		exit 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed pg_ctl start $QE_HOST $DE_DIR $QE_PORT"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED:$QE_LINE" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
#Setting APP_PATH
if [ x"" != x"$GPHOME" ];then
        APP_PATH=${GPHOME}/bin
else
        APP_PATH=${BIZHOME}/bin
fi
PARENT_PID=$1;shift		#PID of gpstart process calling this script
CHK_CALL
QE_NAME=$1;shift		#Hostname holding database segment
QE_PORT=$1;shift		#Segment port
QE_DIR=$1;shift			#Segment directory
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift
UPGRADE_TO=$1; shift
PGCONTROL_UPGRADE=$1; shift
GPCONTROLMOD=$1; shift
PARALLEL_STATUS_FILE=$1; shift
QE_LINE="${QE_NAME}:${QE_PORT}:${QE_DIR}"
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
LOG_MSG "[INFO][$INST_COUNT]:-Command line options passed to utility = $*"
PING_CHK
QE_DIR_CHK
START_SEG
$ECHO "COMPLETED:$QE_LINE" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
