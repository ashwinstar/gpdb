#!/bin/bash
#	Filename:-		gpsegexpand.sh
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
BACKOUT_FILE=/tmp/${PROG_NAME}_backout.$$
TMP_COUNT=/tmp/${PROG_NAME}_tmp_count.$$
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpexpandsystem, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

PARA_EXIT () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	if [ $1 -ne 0 ];then
		$ECHO "FAILED:$2" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed $2"
		exit 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed $2"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

MOVE_TEMPLATE_DIR () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	$TRUSTED_SHELL $HOST "$MKDIR $DATADIR;chmod 700 $DATADIR" >> $LOG_FILE 2>&1
	PARA_EXIT $? "$MKDIR $DATADIR;chmod 700 $DATADIR on $HOST"
	BACKOUT_COMMAND "$TRUSTED_SHELL ${HOST} \"$RM -rf $DATADIR > /dev/null 2>&1\""		
	BACKOUT_COMMAND "$ECHO \"Removing $DATADIR on $HOST\""
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing template tar file to $HOST"
	$TRUSTED_SHELL $BASE_HOST "$SCP -pr ${BASE_DATADIR}/$SEG_TAR_FILE ${HOST}:$DATADIR" >> $LOG_FILE 2>&1
	PARA_EXIT $? "copy tar file to $HOST"
	LOG_MSG "[INFO][$INST_COUNT]:-Un-tar $SEG_TAR_FILE on $HOST"
	$TRUSTED_SHELL $HOST "cd $DATADIR;$TAR xf $SEG_TAR_FILE" >> $LOG_FILE 2>&1
	PARA_EXIT $? "untar $SEG_TAR_FILE on $HOST"
	LOG_MSG "[INFO][$INST_COUNT]:-Removing postgres info files"
	$TRUSTED_SHELL $HOST "$RM -f $DATADIR/$PG_OPT $DATADIR/$PG_PID" >> $LOG_FILE 2>&1
	PARA_EXIT $? "remove postgres info files on $HOST for $DATADIR"
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

ADD_NEW_IP_ADDRESSES () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	ADD_HOST=$1;ADD_DATA_DIR=$2
	LOG_MSG "[INFO][$INST_COUNT]:-Adding new IP addresses to $PG_HBA on $ADD_HOST"
	for IP_ADDRESS in ${NEW_IP_ARRAY[@]}
	do
		THERE=`$TRUSTED_SHELL ${ADD_HOST} "$GREP -c \"$IP_ADDRESS\" ${ADD_DATA_DIR}/$PG_HBA"`
		if [ $THERE -eq 0 ];then
			$TRUSTED_SHELL $ADD_HOST "$ECHO host  all     all     ${IP_ADDRESS}/32 trust >> ${ADD_DATA_DIR}/$PG_HBA" >> $LOG_FILE 2>&1
			PARA_EXIT $? "add IP address ${IP_ADDRESS} to ${ADD_DATA_DIR}/$PG_HBA on $ADD_HOST"
			LOG_MSG "[INFO][$INST_COUNT]:-Added IP ${IP_ADDRESS} to ${ADD_DATA_DIR}/$PG_HBA on $ADD_HOST"
		fi
	done	
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

PROCESS_HOST () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	MOVE_TEMPLATE_DIR
	LOG_MSG "[INFO][$INST_COUNT]:-Setting port number to $PORT for $HOST in $DATADIR"
	ED_FILE ${DATADIR}/$PG_CONF "$PORT_TXT" port=$PORT 0 $HOST
	ADD_NEW_IP_ADDRESSES $HOST $DATADIR
	LOG_MSG "[INFO][$INST_COUNT]:-Starting Functioning instance on segment $HOST $DATADIR"
	$TRUSTED_SHELL ${HOST} "$EXPORT_LIB_PATH;export PGPORT=${PORT}; $PG_CTL -w -D $DATADIR -l ${DATADIR}.log -o \"-i -p ${PORT}\" start" >> $LOG_FILE 2>&1
	PARA_EXIT $? "start instance on segment $HOST $DATADIR port $PORT"
	BACKOUT_COMMAND "$TRUSTED_SHELL ${HOST} \"$RM -f ${DATADIR}.log\""
	BACKOUT_COMMAND "$ECHO \"Removing segment log file ${DATADIR}.log on segment $HOST\""
	BACKOUT_COMMAND "$TRUSTED_SHELL $HOST \"${EXPORT_LIB_PATH};export PGPORT=${PORT}; $PG_CTL -w -D $DATADIR -l ${DATADIR}.log -o \"-i -p ${PORT}\" -m immediate  stop\""
	BACKOUT_COMMAND "$ECHO \"Stopping instance on $HOST $DATADIR\""
	LOG_MSG "[INFO][$INST_COUNT]:-Updating $GP_ID_TABLE on $HOST $DATADIR"
	$TRUSTED_SHELL $HOST "$EXPORT_LIB_PATH;env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $PORT $DEFAULTDB -c \"delete from $GP_ID_TABLE;\"" >> $LOG_FILE 2>&1
	PARA_EXIT $? "delete from $GP_ID_TABLE on $HOST $DATADIR"
	$TRUSTED_SHELL $HOST "$EXPORT_LIB_PATH;env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $PORT $DEFAULTDB -c \"insert into $GP_ID_TABLE (gpname, numsegments, dbid, content) values ('${ARRAY_NAME}',${FINAL_TOTAL_SEG},${DBID},${CONTENT});\"" >> $LOG_FILE 2>&1
	PARA_EXIT $? "update $GP_ID_TABLE on $HOST $DATADIR"
	LOG_MSG "[INFO][$INST_COUNT]:-Updating Master instance $CONFIG_TABLE for $HOST $DATADIR"
	${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME  -A -t -c"insert into $CONFIG_TABLE values ($CONTENT, '$DEF_PRIMARY', $DBID, '$IS_PRIMARY', '$VALID', '$HOST', $PORT, '$DATADIR');"
	PARA_EXIT $? "update $CONFIG_TABLE for $HOST $DATADIR"
	BACKOUT_COMMAND "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -q -p $MASTER_PORT $QD_DBNAME  -A -t -c\"delete from $CONFIG_TABLE where dbid=$DBID;\""
	BACKOUT_COMMAND "$ECHO \"Deleting $HOST $DATADIR from $CONFIG_TABLE\""
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

CHK_HOST () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	IS_THERE=`${EXPORT_LIB_PATH};$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where hostname='$HOST' and content<>-1;"`
	PARA_EXIT $? "check for $HOST in $CONFIG_TABLE"
	if [ $IS_THERE -ne 0 ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Host $HOST has an entry in $CONFIG_TABLE" 1
		PARA_EXIT 2 "${QE_LINE}:CONFIG_EXISTS"
	fi
	PING_HOST $HOST 1
	if [ $RETVAL -ne 0 ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Issue with ping of $HOST" 1
		PARA_EXIT 2 "${QE_LINE}:PING_FAIL"
	fi
	HOST_INITDB_VERSION=`$TRUSTED_SHELL $HOST "$EXPORT_LIB_PATH;$INITDB -V"`
	PARA_EXIT $? "${QE_LINE}:INITDB_CHK"
	if [ "$HOST_INITDB_VERSION" != "$MASTER_INITDB_VERSION" ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Initdb version mismatch on $HOST"
		PARA_EXIT 2 "${QE_LINE}:INITDB_VER_ERROR"
	fi
	NEW_OS_TYPE=`$TRUSTED_SHELL $HOST "uname -s|tr '[A-Z]' '[a-z]'"`
	PARA_EXIT $? "${QE_LINE}:ARCH_CHECK"
	if [ $MASTER_OS_TYPE != $NEW_OS_TYPE ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Host $HOST architecture type $NEW_ARCH_TYPE, master is $MASTER_ARCH_TYPE" 1
		PARA_EXIT 2 "${QE_LINE}:ARCH_INCORRECT"
	fi
	CHK_DIR $GPHOME $NEW_HOST
	if [ $EXISTS -ne 0 ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-No $GPHOME directory on $NEW_HOST" 1
		PARA_EXIT 2 "${QE_LINE}:NO_GPHOME"
	fi	
	for REMOTE_DIR in "${DIR_PREFIX_ARRAY[@]}"
	do
		CHK_DIR $REMOTE_DIR $HOST
		if [ $EXISTS -ne 0 ];then
			LOG_MSG "[FATAL][$INST_COUNT]:-No $REMOTE_DIR on $HOST" 1
			PARA_EXIT 2 "${QE_LINE}:NO_MOUNT_DIR"
		fi
		CHK_WRITE_DIR $REMOTE_DIR $HOST
		if [ $RETVAL -ne 0 ];then
			LOG_MSG "[FATAL][$INST_COUNT]:-Test write issue for $REMOTE_DIR on $HOST" 1
			PARA_EXIT 2 "${QE_LINE}:WRITE_FAIL"
		fi
		EXISTS=`$TRUSTED_SHELL $HOST "$LS $REMOTE_DIR/$SEG_PREFIX*|$WC -l" 2>/dev/null`
		if [ $EXISTS -ne 0 ];then
			LOG_MSG "[FATAL][$INST_COUNT]:-Directory $REMOTE_DIR/$SEG_PREFIX* exists on $HOST" 1
			PARA_EXIT 2 "${QE_LINE}:EXISTS_SEG_DIR"
		fi
	done
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}

REDISTRIB_TABLE () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO][$INST_COUNT]:-Obtaining table count for $TABLE_NAME" 
	PRE_TABLE_REC_COUNT=`${EXPORT_LIB_PATH};$PSQL -q -p $MASTER_PORT $DB -A -t -c"select count(*) from only ${S_NAME}.$TABLE_NAME;"`
	PARA_EXIT $? "${QE_LINE}:PRE_COPY_COUNT"
	LOG_MSG "[INFO][$INST_COUNT]:-Table ${S_NAME}.$TABLE_NAME has $PRE_TABLE_REC_COUNT records"	
	LOG_MSG "[INFO][$INST_COUNT]:-Creating temporary holding table"
	$PSQL -A -t -p $MASTER_PORT $DB -c"create table ${EXPAND_SCHEMA}.$TABLE_NAME (like ${S_NAME}.${TABLE_NAME});" >> $LOG_FILE 2>&1
	PARA_EXIT $? "${QE_LINE}:TABLE_CREATE"
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing stage I insert"
	$PSQL -A -t -p $MASTER_PORT $DB -c"insert into ${EXPAND_SCHEMA}.$TABLE_NAME select * from only ${S_NAME}.${TABLE_NAME};" > $TMP_COUNT 2>&1
	PARA_EXIT $? "${QE_LINE}:STAGE_I_INSERT"
	STAGE_COUNT=`$CAT $TMP_COUNT|$GREP INSERT|$AWK '{print $3}'`
	$RM -f $TMP_COUNT
	if [ $STAGE_COUNT -ne $PRE_TABLE_REC_COUNT ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Record count mismatch between ${EXPAND_SCHEMA}.$TABLE_NAME and ${S_NAME}.${TABLE_NAME}"
		PARA_EXIT 2 "${QE_LINE}:STAGE_I_COUNT"
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Stage I insert record counts match at $PRE_TABLE_REC_COUNT"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing truncate of ${S_NAME}.${TABLE_NAME}"
	$PSQL -A -t -p $MASTER_PORT $DB -c"truncate table ${S_NAME}.${TABLE_NAME} restrict;" >> $LOG_FILE 2>&1
	PARA_EXIT $? "${QE_LINE}:TRUNCATE"
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing stage II insert"
	$PSQL -A -t -p $MASTER_PORT $DB -c"insert into ${S_NAME}.$TABLE_NAME select * from only ${EXPAND_SCHEMA}.${TABLE_NAME};" > $TMP_COUNT 2>&1
	PARA_EXIT $? "${QE_LINE}:STAGE_II_INSERT"
	STAGE_COUNT=`$CAT $TMP_COUNT|$GREP INSERT|$AWK '{print $3}'`
	$RM -f $TMP_COUNT
	if [ $STAGE_COUNT -ne $PRE_TABLE_REC_COUNT ];then
		LOG_MSG "[FATAL][$INST_COUNT]:-Record count mismatch between ${EXPAND_SCHEMA}.$TABLE_NAME and ${S_NAME}.${TABLE_NAME}"
		PARA_EXIT 2 "${QE_LINE}:STAGE_II_COUNT"
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Stage II insert record counts match at $PRE_TABLE_REC_COUNT"
	fi
	if [ $KEEP_SCHEMA -eq 0 ];then
		LOG_MSG "[INFO][$INST_COUNT]:-Dropping table ${EXPAND_SCHEMA}.$TABLE_NAME"
		$PSQL -A -t -p $MASTER_PORT $DB -c"drop table ${EXPAND_SCHEMA}.$TABLE_NAME" >> $LOG_FILE 2>&1
		if [ $? -ne 0 ];then
			LOG_MSG "[WARN]:-Issue with drop table ${EXPAND_SCHEMA}.$TABLE_NAME"
		fi
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Keeping table ${EXPAND_SCHEMA}.$TABLE_NAME, as -k option supplied" 
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
PARENT_PID=$1;shift		#PID of gpstate process calling this script
CHK_CALL
MODE=$1;shift
INST_COUNT=$1;shift
LOG_FILE=$1;shift
PARALLEL_STATUS_FILE=$1;shift
case $MODE in
	1)	HOST=$1;shift
		PORT=$1;shift
		BASE_HOST=$1;shift
		BASE_DATADIR=$1;shift
		SEG_TAR_FILE=$1;shift
		DATADIR=$1;shift
		ARRAY_STRING=$1;shift
		ARRAY_NAME=`$ECHO $ARRAY_STRING|$TR '~' ' '`
		FINAL_TOTAL_SEG=$1;shift
		DBID=$1;shift
		CONTENT=$1;shift
		DEF_PRIMARY=$1;shift
		IS_PRIMARY=$1;shift
		VALID=$1;shift
		NEW_IP_STRING=$1;shift
		NEW_IP_ARRAY=(`$ECHO $NEW_IP_STRING|$TR '~' ' '`)
		QE_LINE="${HOST}:${DATADIR}:${PORT}"
		LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
		PROCESS_HOST
		;;
	2)
		HOST=$1;shift
		ARRAY_STRING=$1;shift
		DIR_PREFIX_ARRAY=(`$ECHO $ARRAY_STRING|$TR '~' ' '`)
		QE_LINE="${HOST}"
		LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
		CHK_HOST
		;;
	3)
		TABLE_NAME=$1;shift
		S_NAME=$1;shift
		DB=$1;shift
		EXPAND_SCHEMA=$1;shift
		KEEP_SCHEMA=$1;shift
		QE_LINE="${TABLE_NAME}"
		LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
		REDISTRIB_TABLE
		;;
esac
$ECHO "COMPLETED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
