#!/bin/bash
#	Filename:-		gpsegsize.sh
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
FUNCTIONS=$WORKDIR/gp_bash_fuctions.sh
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
		$ECHO "      Script called by gpsizecalc.sh, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

SIZE_ERROR_EXIT () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	$ECHO "${QE_HOST}|${QE_BASE_DIR}|${QE_PORT}|0" >> $TMPFILE
	$ECHO "FAILED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
	LOG_MSG "[WARN]:-Failed to process $1 size request"
	exit 1
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

GET_DATABASE_SIZE () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	QE_BASE_DIR=`$BASENAME $QE_DIR`
	QE_DB=${QE_DIR}/base/${DB_OID}
	$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"${QE_PORT}"|"`$TRUSTED_SHELL $QE_HOST "ls -al ${QE_DB}|$GREP -v '^d'|$GREP -vi total 2>/dev/null" |$AWK '{total += \$5} END {print total}'` >> $TMPFILE
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then SIZE_ERROR_EXIT database;fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

GET_TABLE_SIZE () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	QE_TABLE=${QE_DIR}/base/${DB_OID}/${QE_TABLE_OID}
	QE_BASE_DIR=`$BASENAME $QE_DIR`
	$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"${QE_PORT}"|"`$TRUSTED_SHELL $QE_HOST "if [ -f $QE_TABLE ];then ls -al ${QE_TABLE};else $ECHO 0 0 0 0 0;fi"|$TAIL -1|$AWK '{print \$5}'` >> $TMPFILE
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then SIZE_ERROR_EXIT table;fi
	$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"${QE_PORT}"|"`$TRUSTED_SHELL $QE_HOST "ls -al ${QE_TABLE}.*|$GREP -v '^d'|$GREP -vi total" 2>/dev/null|$AWK '{total += \$5} END {print total}'` >> $TMPFILE
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then SIZE_ERROR_EXIT table;fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

GET_INDEX_SIZE () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	INDEX_ARRAY=(`$TRUSTED_SHELL -n $QE_HOST "env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_PORT $QD_DBNAME -A -t -c\"select relfilenode from pg_class where oid in (select indexrelid from pg_index where indrelid in (select oid from pg_class where relname='${TABLE_NAME}'));\""`)
	RETVAL=$?
	if [ $RETVAL -ne 0 ] || [ ${#INDEX_ARRAY[@]} -eq 0 ];then
		SIZE_ERROR_EXIT index
	else	
		for IDX in ${INDEX_ARRAY[@]} 
		do
			QE_INDEX=${QE_DIR}/base/${DB_OID}/$IDX
			QE_BASE_DIR=`$BASENAME $QE_DIR`
			$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"${QE_PORT}"|"`$TRUSTED_SHELL $QE_HOST "ls -al ${QE_INDEX}"|$TAIL -1|$AWK '{print \$5}'` >> $TMPFILE
			$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"${QE_PORT}"|"`$TRUSTED_SHELL $QE_HOST "ls -al ${QE_INDEX}.*|$GREP -v '^d'|$GREP -vi total" 2>/dev/null|$AWK '{total += \$5} END {print total}'` >> $TMPFILE
		done
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
PARENT_PID=$1;shift		#PID of gpstate.sh process calling this script
CHK_CALL
QE_HOST=$1;shift		#Hostname holding database segment
QE_PORT=$1;shift		#Segment port
QE_DIR=$1;shift			#Segment directory
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift
QE_TABLE_OID=$1;shift
DB_OID=$1;shift
TYPE=$1;shift
TMPFILE=$1;shift
QD_DBNAME=$1;shift
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
case $TYPE in
	1 ) GET_TABLE_SIZE ;;
	2 ) GET_INDEX_SIZE ;;
	3 ) GET_DATABASE_SIZE ;;
esac
QE_LINE="${QE_HOST}:${QE_PORT}:${QE_DIR}"
$ECHO "COMPLETED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
