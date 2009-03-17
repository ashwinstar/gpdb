#!/bin/bash
#	Filename:-		gpsegmirrors.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Oct 2007
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
RSYNC="rsync"
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpaddmirrors, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

PROCESS_MIRROR () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO][$INST_COUNT]:-Commencing primary data directory copy $QE_PRI_NAME to $QE_MIR_NAME"
	KB_COPY=`$TRUSTED_SHELL ${QE_PRI_NAME} "$DU -k -s $DU_TXT ${QE_PRI_BASE_DIR}|$TAIL -1"|$AWK '{print $1}'`
	LOG_MSG "[INFO][$INST_COUNT]:-Process will transfer approximately $KB_COPY KB of data to $QE_MIR_NAME"
	$TRUSTED_SHELL ${QE_MIR_NAME} "mkdir ${QE_MIR_BASE_DIR};chmod 700 ${QE_MIR_BASE_DIR}"
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:MKDIR" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed to mkdir ${QE_MIR_BASE_DIR};chmod 700 ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed mkdir ${QE_MIR_BASE_DIR};chmod 700 ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}"
	fi
	OBJECT_LIST=(`$TRUSTED_SHELL ${QE_PRI_NAME} "ls ${QE_PRI_BASE_DIR}|$GREP -v db_dumps|$TR '\n' ' '"`)	
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:OBJECT_LIST" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed to obtain object list from ${QE_PRI_NAME} for ${QE_PRI_BASE_DIR}" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	else
		 LOG_MSG "[INFO][$INST_COUNT]:-Completed obtain object list from ${QE_PRI_NAME} for ${QE_PRI_BASE_DIR}"
	fi

	if [ x"rsync" == x"$RSYNCOPT" ]; then
	    # gprsync
	    ((RSYNC_PORT=$QE_MIR_PORT+500))
	    $TRUSTED_SHELL ${QE_PRI_NAME} "${EXPORT_GPHOME}; ${GPHOME}/bin/lib/gprsync.py -p ${RSYNC_PORT} -d ${QE_PRI_BASE_DIR} -o '-av' ${OBJECT_LIST[@]} ${QE_MIR_NAME}:${QE_MIR_BASE_DIR}" > $LOG_FILE 2>&1
	    if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:GPRSYNC_COPY" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed gprsync copy from ${QE_PRI_NAME} to ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	    else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed gprsync copy from ${QE_PRI_NAME} to ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}"
	    fi
	else
	    $TRUSTED_SHELL ${QE_PRI_NAME} "cd ${QE_PRI_BASE_DIR};$TAR cf - ${OBJECT_LIST[@]}|$TRUSTED_SHELL ${QE_MIR_NAME} \"cd ${QE_MIR_BASE_DIR};$TAR xf -\"" >> $LOG_FILE  2>&1
	    if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:TAR_COPY" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed tar copy from ${QE_PRI_NAME} to ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	    else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed tar copy from ${QE_PRI_NAME} to ${QE_MIR_BASE_DIR} on ${QE_MIR_NAME}"
	    fi
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Setting port to $QE_MIR_PORT on ${QE_MIR_NAME}"
	ED_FILE ${QE_MIR_BASE_DIR}/$PG_CONF "port=" "port=${QE_MIR_PORT}" 0 $QE_MIR_NAME 
	LOG_MSG "[INFO][$INST_COUNT]:-Starting mirror database in admin mode on ${QE_MIR_NAME} for directory ${QE_MIR_BASE_DIR}"
	$TRUSTED_SHELL $QE_MIR_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -w -l $QE_MIR_BASE_DIR/pg_log/startup.log -D $QE_MIR_BASE_DIR -o \"-i -p $QE_MIR_PORT \" start" >> $LOG_FILE 2>&1
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:ADMIN_START" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed to start $QE_MIR_NAME database for directory $QE_MIR_BASE_DIR" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed start $QE_MIR_NAME database for directory $QE_MIR_BASE_DIR"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Updating $GP_ID_TABLE $QE_MIR_NAME database for directory $QE_MIR_BASE_DIR"
	$TRUSTED_SHELL $QE_MIR_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $QE_MIR_PORT -d $DEFAULTDB -c \"update $GP_ID_TABLE set dbid=${QE_MIR_DBID} where dbid<>${QE_MIR_DBID};\"" >> $LOG_FILE 2>&1
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:UPDATE_ID_TABLE" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed update $GP_ID_TABLE set dbid=${QE_MIR_DBID} where dbid<>${QE_MIR_DBID}" 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed update $GP_ID_TABLE set dbid=${QE_MIR_DBID} where dbid<>${QE_MIR_DBID}"
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-Closing down ${QE_MIR_NAME} for directory ${QE_MIR_BASE_DIR} database from admin mode"
	$TRUSTED_SHELL $QE_MIR_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -w stop -D $QE_MIR_BASE_DIR -m smart " >> $LOG_FILE 2>&1
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:ADMIN_SHUTDOWN" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed $PG_CTL -w stop -D $QE_MIR_BASE_DIR -m smart " 1
		ERROR_EXIT "[FATAL][$INST_COUNT]:-Exiting, unable to continue" 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed $PG_CTL -w stop -D $QE_MIR_BASE_DIR -m smart "
	fi	
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}


CHK_RSYNC () {
    LOG_MSG "[INFO]:-Start Function $FUNCNAME"
    if [ x"rsync" == x"$RSYNCOPT" ] ; then
	RSYNC=`findCmdInPath rsync`
	RETVAL=$?
        if [ $RETVAL -ne 0 ]; then
	    ERROR_EXIT "[FATAL]:-rsync not found." 2
        fi
    fi
    LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED:${QE_NAME}:${QE_BASE_DIR}" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
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
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift		#Central utility log file
QE_PRI_NAME=$1;shift		#Name of primary segment host
QE_MIR_NAME=$1;shift		#Name of mirror segment host
QE_PRI_BASE_DIR=$1;shift	#Name of primary segment data directory
QE_MIR_BASE_DIR=$1;shift	#Name of mirror segment data directory
QE_MIR_PORT=$1;shift		#Port number for mirror segment instance
QE_MIR_DBID=$1;shift		#DBID of mirror segment instance
PARALLEL_STATUS_FILE=$1;shift
RSYNCOPT=$1;shift
CHK_RSYNC
QE_LINE=${QE_PRI_NAME}:${QE_PRI_BASE_DIR}:${QE_MIR_NAME}:${QE_MIR_BASE_DIR}:${QE_MIR_PORT}:${QE_MIR_DBID}
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
PROCESS_MIRROR
$ECHO "COMPLETED:${QE_LINE}" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
