#!/bin/bash
#	Filename:-		mppsegrec.sh
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
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
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
MPP_USER=$USER_NAME
EXIT_STATUS=0
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by mpprecoverseg.sh, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

FAIL_SET_VAR () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	FAIL_QE_HOST=`$ECHO $1|$CUT -d"|" -f1`
	FAIL_QE_PORT=`$ECHO $1|$CUT -d"|" -f2`
	FAIL_QE_DIR=`$ECHO $1|$CUT -d"|" -f3`
	FAIL_QE_DBID=`$ECHO $1|$CUT -d"|" -f4`
	FAIL_QE_CONTENTID=`$ECHO $1|$CUT -d"|" -f5`
	FAIL_QE_TOP_DIR=`$DIRNAME $FAIL_QE_DIR`
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

RECOVER_SEGMENT () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	#See if we can ping the host with the failed instance
	PING_HOST $FAIL_QE_HOST 1
	if [ $RETVAL -eq 0 ];then
	#Can reach the host, so continue
		LOG_MSG "[INFO][$INST_COUNT]:-Contact established with host $FAIL_QE_HOST"
		LOG_MSG "[INFO][$INST_COUNT]:-Checking for invalid postmaster process on port ${FAIL_QE_HOST}"
		GET_PG_PID_ACTIVE $FAIL_QE_PORT $FAIL_QE_HOST
		if [ $PID -ne 0 ]; then
			LOG_MSG "[INFO][$INST_COUNT]:-Orphaned PID located on $FAIL_QE_PORT clearing down"
			$TRUSTED_SHELL $FAIL_QE_HOST "$KILL -9 $PID" >> $LOG_FILE 2>&1
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}" >> $LOG_FILE 2>&1
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock" >> $LOG_FILE 2>&1
		else
			LOG_MSG "[INFO][$INST_COUNT]:-No active pid found for $FAIL_QE_HOST for port=$FAIL_QE_PORT"
			#Force a remove of the lock files just incase postmaster has been killed with -9 signal
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}" >> $LOG_FILE 2>&1
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock" >> $LOG_FILE 2>&1
		fi
		LOG_MSG "[INFO][$INST_COUNT]:-Deleting failed database directory on ${FAIL_QE_HOST}"
		$TRUSTED_SHELL ${FAIL_QE_HOST} "$RM -Rf ${FAIL_QE_DIR}"
		RETVAL=$?
		if [ $RETVAL -ne 0 ];then
			LOG_MSG "[WARN][$INST_COUNT]:-Issue with forced delete of ${FAIL_QE_DIR} on ${FAIL_QE_HOST}"
		else
			LOG_MSG "[INFO][$INST_COUNT]:-Deleted ${FAIL_QE_DIR} on ${FAIL_QE_HOST}"
		fi
		LOG_MSG "[INFO][$INST_COUNT]:-Deleting failed database log file on ${FAIL_QE_HOST}"
		$TRUSTED_SHELL ${FAIL_QE_HOST} "$RM -Rf ${FAIL_QE_DIR}.log"
		RETVAL=$?
		if [ $RETVAL -ne 0 ];then
			LOG_MSG "[WARN][$INST_COUNT]:-Issue with forced delete ${FAIL_QE_DIR}.log on ${FAIL_QE_HOST}"
		else
			LOG_MSG "[INFO][$INST_COUNT]:-Deleted ${FAIL_QE_DIR}.log on ${FAIL_QE_HOST}"
		fi
	else
	#Unable to reach failed segment host
		LOG_MSG "[INFO][$INST_COUNT]:-Unable to contact host $FAIL_QE_HOST"
		if [ `$ECHO $POOL_ARRAY|$WC -c` -eq 1 ];then
		#We do not have a pool host or pool directory set, so unable to continue
			LOG_MSG "[FATAL][$INST_COUNT]:-Unable to reach failed host $FAIL_QE_HOST" 1
			LOG_MSG "[FATAL][$INST_COUNT]:-No pool host of pool directory defined, unable to continue" 1
			$ECHO "FAILED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
			exit 1
		fi
	fi
	if [ $STOPDB -eq 0 ] && [ $FORCESTOP -eq 0 ]; then
		LOG_MSG "[INFO][$INST_COUNT]:-Issuing checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT"
		$TRUSTED_SHELL $MIR_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MIR_QE_PORT $DEFAULTDB -c \"checkpoint;\" > /dev/null 2>&1"
		ERROR_CHK $? "instance checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT" 1
	fi
	#Now check to see if we are recovering to a pool host or pool directory
	if [ `$ECHO $POOL_ARRAY|$WC -c` -gt 2 ];then
	#Pool host or pool directory set
		FAIL_SET_VAR $POOL_ARRAY
		LOG_MSG "[INFO][$INST_COUNT]:-Reconfiguring for pool host or pool directory recovery"
	fi
	#Now start recovery process
	LOG_MSG "[INFO][$INST_COUNT]:-Recovering data directory from ${MIR_QE_HOST} to ${FAIL_QE_HOST}" 1
	LOG_MSG "[INFO][$INST_COUNT]:-This could take some time depending on data volume" 1
	KB_COPY=`$TRUSTED_SHELL ${MIR_QE_HOST} "$DU -k -s $DU_TXT ${MIR_QE_DIR}|$TAIL -1"|$AWK '{print $1}'`
	LOG_MSG "[INFO][$INST_COUNT]:-Copy will transfer $KB_COPY KBytes of data"  1
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing data transfer, please wait..." 1
	$TRUSTED_SHELL ${MIR_QE_HOST} "$TRUSTED_COPY -pr ${MIR_QE_DIR} ${FAIL_QE_HOST}:${FAIL_QE_DIR}"
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-$TRUSTED_COPY -pr ${MIR_QE_DIR} ${FAIL_QE_HOST}:${FAIL_QE_DIR} failed" 1
		$ECHO "FAILED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
		exit 1
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Transfer complete, commencing post transfer processing" 1
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Setting recovered port to ${FAIL_QE_PORT} on ${FAIL_QE_HOST}"
	ED_FILE ${FAIL_QE_DIR}/$PG_CONF "port=" "port=${FAIL_QE_PORT}" 0 ${FAIL_QE_HOST}
	LOG_MSG "[INFO][$INST_COUNT]:-Clearing old postmaster.opts file"
	$TRUSTED_SHELL ${FAIL_QE_HOST} "$RM -f ${FAIL_QE_DIR}/postmaster.opts"
	if [ $STOPDB -eq 0 ] && [ $FORCESTOP -eq 0 ]; then
		LOG_MSG "[INFO][$INST_COUNT]:-Clearing old postmaster.pid file"
		$TRUSTED_SHELL ${FAIL_QE_HOST} "$RM -f ${FAIL_QE_DIR}/postmaster.pid"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED|${QE_LINE}" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
PARENT_PID=$1;shift		#PID of mppstate.sh process calling this script
CHK_CALL
FAIL_QE_HOST=$1;shift
FAIL_QE_PORT=$1;shift
FAIL_QE_DIR=$1;shift
FAIL_QE_DBID=$1;shift
FAIL_QE_CONTENTID=$1;shift
FAIL_QE_TOP_DIR=$1;shift
MIR_QE_HOST=$1;shift
MIR_QE_PORT=$1;shift
MIR_QE_DIR=$1;shift
MIR_QE_DBID=$1;shift
MIR_QE_TOP_DIR=$1;shift
INST_COUNT=$1;shift
LOG_FILE=$1;shift
STOPDB=$1;shift
FORCESTOP=$1;shift
POOL_ARRAY=$1;shift
QE_LINE="${MIR_QE_HOST}:${MIR_QE_PORT}:${MIR_QE_DIR}"
RECOVER_SEGMENT
$ECHO "COMPLETED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
