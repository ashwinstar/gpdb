#!/bin/bash
#	Filename:-		gpsegstop.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Dec 2006
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
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpstop, this should not be run directly"
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
                exit 2
        fi
}

FORCE_CLEAR () {
        LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
        LOG_MSG "[INFO][$INST_COUNT]:-Killing postmaster process on $QE_NAME for port=$QE_PORT" 1
	FORCE_CLEAR_WARN=0
	PING_HOST $QE_NAME 1
	if [ $RETVAL -eq 0 ];then
	#Have contacted host, so continue
        	GET_PG_PID_ACTIVE $QE_PORT $QE_NAME
        	if [ $PID -ne 0 ]; then
                	$TRUSTED_SHELL $QE_NAME "$KILL -9 $PID" > $LOG_FILE 2>&1
        	else
                	LOG_MSG "[INFO][$INST_COUNT]:-No active pid found for $QE_NAME for port=$QE_PORT"
        	fi
		IS_THERE=`$TRUSTED_SHELL $QE_NAME "if [ -f /tmp/.s.PGSQL.${QE_PORT}.lock ];then echo 1;else echo 0;fi"`
		if [ $IS_THERE -eq 1 ];then
        		LOG_MSG "[INFO][$INST_COUNT]:-Clearing lock files on $QE_NAME for directory $QE_DIR" 1
        		$TRUSTED_SHELL $QE_NAME "$RM -f /tmp/.s.PGSQL.${QE_PORT}.lock /tmp/.s.PGSQL.${QE_PORT}"
			if [ $? -ne 0 ];then
        			LOG_MSG "[WARN][$INST_CONT]:-Issue with removal of lock files on $QE_NAME for directory $QE_DIR" 
				FORCE_CLEAR_WARN=1
			fi
		else
			LOG_MSG "[INFO][$INST_COUNT]:-No /tmp lock file located"
		fi
		IS_THERE=`$TRUSTED_SHELL $QE_NAME "if [ -f ${QE_DIR}/postmaster.pid ];then echo 1;else echo 0;fi"`
		if [ $IS_THERE -eq 1 ];then
        		LOG_MSG "[INFO][$INST_COUNT]:-Clearing postmaster.pid file" 1
        		$TRUSTED_SHELL $QE_NAME "$RM -f ${QE_DIR}/postmaster.pid"
			if [ $? -ne 0 ];then
        			LOG_MSG  "[WARN][$INST_CONT]:-Issue with clearing postmaster.pid file on $QE_NAME for directory $QE_DIR" 1
				FORCE_CLEAR_WARN=1
			fi
		else
			LOG_MSG "[INFO][$INST_COUNT]:-No ${QE_DIR}/postmaster.pid file located"
		fi
	else
		LOG_MSG "[WARN][$INST_COUNT]:-Failed to establish contact with $QE_NAME"
		FORCE_CLEAR_WARN=1
	fi
        LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

STOP_SEG () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO][$INST_COUNT]:-Stopping Segment Instance $QE_NAME directory $QE_DIR"
	$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};$PG_CTL -w stop -D $QE_DIR -m $MODE -l ${QE_DIR}.log >/dev/null 2>&1"
	RETVAL=$?
	if [ $RETVAL -ne 0 ] && [ $STOP_INVALID -eq 1 ];then
		FORCE_CLEAR
		if [ $FORCE_CLEAR_WARN -ne 0 ];then
			$ECHO "FAILED:${QE_LINE}:FORCE_CLEAR_WARN" >> $PARALLEL_STATUS_FILE
			exit 2
		else
			$ECHO "COMPLETED:${QE_LINE}" >> $PARALLEL_STATUS_FILE	
			exit
		fi
	else
		if [ $RETVAL -ne 0 ];then
			LOG_MSG "[FATAL][$INST_COUNT]:-Failed to stop Segment Instance $QE_NAME directory $QE_DIR"
			$ECHO "FAILED:${QE_LINE}:NO_FORCE_CLEAR" >> $PARALLEL_STATUS_FILE
			exit 2
		else
			LOG_MSG "[INFO][$INST_COUNT]:-Successfully stopped Segment Instance $QE_NAME directory $QE_DIR"
		fi
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED:${QE_LINE}" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
PARENT_PID=$1;shift		#PID of gpstart process calling this script
CHK_CALL
STOP_INVALID=$1;shift		#0=No 1=Yes
QE_NAME=$1;shift		#Hostname holding database segment
QE_PORT=$1;shift		#Segment port
QE_DIR=$1;shift			#Segment directory
QE_VALID=$1;shift		#Status of segment either t or f
MODE=$1;shift			#smart, fast or immediate
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift
QE_LINE="${QE_NAME}:${QE_PORT}:${QE_DIR}"
PING_CHK
STOP_SEG
$ECHO "COMPLETED:$QE_LINE" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
