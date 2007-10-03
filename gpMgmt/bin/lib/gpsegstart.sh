#!/bin/bash
#	Filename:-		gpsegstart.sh
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
# ED_FILE search text values
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpstart, this should not be run directly"
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
	UPGRADE=0

	if [ "$UPGRADE_TO" == "3.0" ] || [ "$UPGRADE_TO" == "3.1" ]
	then
		LOG_MSG "[INFO][$INST_COUNT]:-Switching database to version $UPGRADE_TO"
		UPGRADE=1
	fi 
	if [ $QE_SWITCH_LOG -eq 1 ];then
	#*************************************************************************************************************
	# Start of switch log section
	#*************************************************************************************************************
		IS_THERE=`$TRUSTED_SHELL $QE_NAME "if [ -f ${QE_DIR}.log_$CUR_DATE ]; then $ECHO 1;else $ECHO 0;fi"`
		if [ $IS_THERE -eq 0 ];then
			LOG_MSG "[INFO][$INST_COUNT]:-Processing switch log request for $QE_NAME ${QE_DIR}.log"
			LOG_MSG "[INFO][$INST_COUNT]:-Moving log file to ${QE_DIR}.log_$CUR_DATE"
			$TRUSTED_SHELL $QE_NAME "$MV ${QE_DIR}.log ${QE_DIR}.log_$CUR_DATE"
			if [ $? -ne 0 ];then
				LOG_MSG "[WARN][$INST_COUNT]:-Issue with Moving log file to ${QE_DIR}.log_$CUR_DATE" 1
				EXIT_STATUS=1
			else
				LOG_MSG "[INFO][$INST_COUNT]:-Completed Moving log file to ${QE_DIR}.log_$CUR_DATE"
			fi
		else
			LOG_MSG "[INFO][$INST_COUNT]:-Appending log file to ${QE_DIR}.log_$CUR_DATE"
			$TRUSTED_SHELL $QE_NAME "$CAT ${QE_DIR}.log >> ${QE_DIR}.log_$CUR_DATE"
			if [ $? -ne 0 ];then
				LOG_MSG "[WARN][$INST_COUNT]:-Issue with Appending log file to ${QE_DIR}.log_$CUR_DATE" 1
				EXIT_STATUS=1
			else
				LOG_MSG "[INFO][$INST_COUNT]:-Completed Appending log file to ${QE_DIR}.log_$CUR_DATE"
			fi
			$TRUSTED_SHELL $QE_NAME "$RM ${QE_DIR}.log;$TOUCH ${QE_DIR}.log"
			if [ $? -ne 0 ];then
				LOG_MSG "[WARN][$INST_COUNT]:-Issue with $RM ${QE_DIR}.log;$TOUCH ${QE_DIR}.log" 1
				EXIT_STATUS=1
			else
				LOG_MSG "[INFO][$INST_COUNT]:-Completed $RM ${QE_DIR}.log;$TOUCH ${QE_DIR}.log"
			fi
		fi
	fi
	#*************************************************************************************************************
	# End of switch log section
	#*************************************************************************************************************

	#*************************************************************************************************************
	# Start of segment startup section
	#*************************************************************************************************************
	if [ $START_INVALID -eq 1 ];then
		#Check to see if there is a stale postmaster.pid file
		IS_THERE=`$TRUSTED_SHELL $QE_NAME "if [ -f ${QE_DIR}/$PG_PID ];then echo 1;else echo 0;fi"`
		if [ $IS_THERE -eq 1 ];then
			LOG_MSG "[WARN][$INST_COUNT]:-Located stale $PG_PID on $QE_NAME in ${QE_DIR} directory"
			LOG_MSG "[INFO][$INST_COUNT]:-Removing $PG_PID on $QE_NAME in ${QE_DIR} directory"
			$TRUSTED_SHELL $QE_NAME "$RM -f ${QE_DIR}/$PG_PID"
		fi
	fi

	EXTRA_START_OPTS=""
	# upgrade mode?
	if [ $UPGRADE -eq 1 ]; then
		LOG_MSG "[INFO][$INST_COUNT]:-Starting in upgrade mode"

		EXTRA_START_OPTS=" -c allow_system_table_mods=true "
		if [ "$UPGRADE_TO" == "3.1" ]; then
			LOG_MSG "[INFO][$INST_COUNT]:-Running control file upgrader"

			$TRUSTED_SHELL $QE_NAME "$GPCONTROLMOD --catversion $UPGRADE_TO $QE_DIR"
			RETVAL=$?

			if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN][$INST_COUNT]:-Encountered error modifying control file"
			fi
		fi
	fi

	LOG_MSG "[INFO][$INST_COUNT]:-Starting Segment instance $QE_NAME directory $QE_DIR"
	$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};export PGPATH=${APP_PATH};export PGPORT=${QE_PORT};$PG_CTL -w -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT} $EXTRA_START_OPTS \" start >/dev/null 2>&1"
	RETVAL=$?
	if [ $RETVAL -ne 0 ] && [ $START_INVALID -eq 1 ];then
	#Have attempted to start an invalid segment which did not start
	#So exit with a warning as opposed to a fatal
		LOG_MSG "[WARN][$INST_COUNT]:-Unable to start invalid segment $QE_HOST $DE_DIR $QE_PORT" 1
		#QE_LINE="${QE_LINE}:INVALID_START_FAIL"
		QE_LINE="FAILED:${QE_LINE}:INVALID_START_FAIL"
		$ECHO "WARN:$QE_LINE" >> $PARALLEL_STATUS_FILE			
		exit 1
	fi
	if [ $RETVAL -ne 0 ] && [ $START_INVALID -eq 0 ];then
	#This represents a genuine failure
	#So exit with fatal message and exit code 2
		LOG_MSG "[WARN][$INST_COUNT]:-Startup request returned error code $RETVAL" 1
		LOG_MSG "[INFO][$INST_COUNT]:-Command string called which resulted in error" 1
		LOG_MSG "[INFO][$INST_COUNT]:-$TRUSTED_SHELL $QE_NAME ${EXPORT_LIB_PATH};export PGPATH=${APP_PATH};export PGPORT=${QE_PORT};$PG_CTL -w -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT}\" start"  1
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
		QE_LINE="FAILED:${QE_LINE}:VALID_START_FAIL"
		$ECHO "FATAL:$QE_LINE" >> $PARALLEL_STATUS_FILE
		exit 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed pg_ctl start $QE_HOST $DE_DIR $QE_PORT"
	fi
	#*************************************************************************************************************
	# End of segment startup section
	#*************************************************************************************************************

	#*************************************************************************************************************
	# Start of segment status check section
	#*************************************************************************************************************
	#Now check status of postmaster on the segment instance
	LOG_MSG "[INFO][$INST_COUNT]:-Checking status via pg_ctl"
	RET_TEXT=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};$PG_CTL status -D $QE_DIR"`
	RUNNING=`$ECHO $RET_TEXT|$EGREP -c "not running|neither"`
	if [ $RUNNING -ne 0 ]; then
		LOG_MSG "[FATAL][$INST_COUNT]:-Command pg_ctl reports $QE_NAME for directory $QE_DIR port $QE_PORT not running" 1
		LOG_MSG "[INFO][$INST_COUNT]:-Check status via gpstate" 1
		QE_LINE="${QE_LINE}:PG_CTL_NO_RUNNING"
		$ECHO "FATAL:$QE_LINE" >> $PARALLEL_STATUS_FILE
		exit 2
	fi
	if [ $RUNNING -eq 0 ] && [ $RETVAL -ne 0 ]; then		
		LOG_MSG "[WARN][$INST_COUNT]:-Could not start instance, BUT pg_ctl reports instance active, check instance log file via gpstate -e" 1
		QE_LINE="FAILED:${QE_LINE}:PG_CTL_THERE_START_FAILED"
		$ECHO "WARN:$QE_LINE" >> $PARALLEL_STATUS_FILE
		exit 1
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Command pg_ctl indicates started Segment instance $QE_NAME database for directory $QE_DIR"
	#*************************************************************************************************************
	# End of segment status check section
	#*************************************************************************************************************

	#*************************************************************************************************************
	# Start of segment configuration check section
	#*************************************************************************************************************
	QD_LC_COLLATE=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c\"show lc_collate;\""`
	if [ $? -ne 0 ];then
		 LOG_MSG "[WARN][$INST_COUNT]:-Failed to obtain lc_collate value $QE_NAME $QE_DIR" 1
	fi
	if [ $QD_LC_COLLATE != $LC_COLLATE ];then
		QD_LINE="${QD_LINE}:LC_ERROR"
		LOG_MSG "[WARN][$INST_COUNT]:-lc_collate mismatch on $QE_NAME $QE_DIR" 1
	fi
	QD_LC_MONETARY=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c\"show lc_monetary;\""`
	if [ $? -ne 0 ];then
		LOG_MSG "[WARN][$INST_COUNT]:-Failed to obtain lc_monetary value $QE_NAME $QE_DIR" 1
	fi
	if [ $QD_LC_MONETARY != $LC_MONETARY ];then
		QD_LINE="${QD_LINE}:LC_ERROR"
		LOG_MSG "[WARN][$INST_COUNT]:-lc_monetary mismatch on $QE_NAME $QE_DIR" 1
	fi
	QD_LC_NUMERIC=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c\"show lc_numeric;\""`
	if [ $? -ne 0 ];then
		LOG_MSG "[WARN][$INST_COUNT]:-Failed to obtain lc_numeric value $QE_NAME $QE_DIR" 1
	fi
	if [ $QD_LC_NUMERIC != $LC_NUMERIC ];then
		QD_LINE="${QD_LINE}:LC_ERROR"
		LOG_MSG "[WARN][$INST_COUNT]:-lc_numeric mismatch on $QE_NAME $QE_DIR" 1
	fi
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
QE_SWITCH_LOG=$1;shift		#0=No switch segment log 1=switch segment log
QE_NAME=$1;shift		#Hostname holding database segment
QE_PORT=$1;shift		#Segment port
QE_DIR=$1;shift			#Segment directory
QE_VALID=$1;shift		#Status of segment either t or f
START_INVALID=$1;shift	#0=Normal valid startup 1=attempt to start a segment marked as invalid
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift
UPGRADE_TO=$1; shift
QD_DBNAME=$1;shift
QE_LINE="${QE_NAME}:${QE_PORT}:${QE_DIR}"
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
LOG_MSG "[INFO][$INST_COUNT]:-Command line options passed to utility = $*"
PING_CHK
QE_DIR_CHK
START_SEG
$ECHO "COMPLETED:$QE_LINE" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
