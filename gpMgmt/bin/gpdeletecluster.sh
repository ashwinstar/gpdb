#!/bin/bash
#	Filename:-		gpdeletecluster.bsh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Deletes an Greenplum instance.
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code
#******************************************************************************
WORKDIR=`dirname $0`
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
if [ -f $FUNCTIONS ];then
	. $FUNCTIONS
else
	echo "FATAL:-Cannot source $FUNCTIONS"
	exit 2;fi
#******************************************************************************
# Location Variables
#******************************************************************************

#******************************************************************************
# Command Variables
#******************************************************************************
#
#******************************************************************************
# Script Specific Variables
#******************************************************************************
PROG_NAME=`$BASENAME $0`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
FORCE=0
PARALLEL_STATUS_FILE=/tmp/gpdeletecluster_parallel_status_file.$$
GPSEGDELETE=gpsegdelete.sh
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	if [ -f ${GPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
	$MORE ${GPDOCDIR}/$HELP_DOC_NAME
	exit 0
	else
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
        $ECHO "      Deletes a Greenplum instance, use with caution, script is destructive"
        $ECHO
        $ECHO "      Usage:"
        $ECHO "      $0 [OPTIONS]"
        $ECHO
        $ECHO "      General options:"
        $ECHO "      -?, display this help message & exit"
        $ECHO "      -v, display version information & exit"
	$ECHO
	$ECHO "      Logging options:"
	$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
	$ECHO "      -D, set log output to debug level, shows all function calls"
	$ECHO
	$ECHO "      Delete options:"
	$ECHO "      -d, <Master database directory> directory to be deleted along with all segment instances [mandatory]"
	$ECHO "      -f, force deletion, ignore any database backup files"
	$ECHO "      -B, <number> run this batch of delete segment processes in parallel [default $BATCH_DEFAULT]"
	$ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
	$ECHO "Unknown option"
	USAGE
}

CHECK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check that we have a master data directory on this host
	CHK_DIR $DELETE_MASTER_DATA_DIRECTORY
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Directory $DELETE_MASTER_DATA_DIRECTORY does not exist on this host" 2;fi
	if [ ! -f ${DELETE_MASTER_DATA_DIRECTORY}/postmaster.pid ];then
		ERROR_EXIT "[FATAL]:-Greenplum instance not running, activate instance to be able to progress instance deletion" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

OBTAIN_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MASTER_DATA_DIRECTORY=$DELETE_MASTER_DATA_DIRECTORY
	GET_QE_DETAILS
	if [ $EXIT_STATUS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Unable to obtain details of segment instances" 2;fi
	#Check to see if a standby host is configured
	GET_STANDBY_COUNT
	if [ $STANDBY_COUNT -ne 0 ];then
		GET_STANDBY_DETAILS
		STANDBY_HOSTNAME=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f1`
		STANDBY_DATADIR=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f2`
		STANDBY_PORT=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f3`
	else
		STANDBY_HOSTNAME="Not configured"
	fi
	LOG_MSG "[INFO]:-Checking for database dump files, please wait..." 1
	DB_DUMP_CHK
	LOG_MSG "[INFO]:-Completed check for database dump files" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DB_DUMP_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Checking Master instance for dump files"
	DUMP_COUNT=`$FIND $MASTER_DATA_DIRECTORY -name 'gp_dump*' -print|$WC -l`
	if [ $DUMP_COUNT -ne 0 ] && [ $FORCE -eq 0 ];then
		LOG_MSG "[FATAL]:-Located possible database dump file on Master instance host" 1
		LOG_MSG "[FATAL]:-In directory $MASTER_DATA_DIRECTORY"1
		ERROR_EXIT "[FATAL]:-To override database dump file checking use -f option" 2
	else
		LOG_MSG "[INFO]:-Completed Master instance dump file check" 1
	fi	
	LOG_MSG "[INFO]:-Checking segment instances for dump files" 1
	BATCH_LIMIT=${#QE_ARRAY[@]}
	PARALLEL_SETUP  $PARALLEL_STATUS_FILE
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		SET_VAR $DELETE_SEG
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
		$GPSEGDELETE $$ 1 $DELETE_HOSTNAME $DELETE_DATADIR $INST_COUNT $LOG_FILE &
		PARALLEL_COUNT $BATCH_LIMIT $BATCH_DEFAULT
	done
	DUMP_FILE_COUNT=`$CAT $PARALLEL_STATUS_FILE|$GREP -c "DUMP_LOCATED"`
	if [ $DUMP_FILE_COUNT -ne 0 ] && [ $FORCE -eq 0 ];then
		LOG_MSG "[FATAL]:-Located possible database dump files on segment instance hosts" 1
		ERROR_EXIT "[FATAL]:-To override database dump file checking use -f option" 2
	else
		LOG_MSG "[INFO]:-Complete dump files check"
	fi
	$RM -f $PARALLEL_STATUS_FILE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SET_VAR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	DELETE_HOSTNAME=`$ECHO $1|$CUT -d"|" -f1`
	DELETE_DATADIR=`$ECHO $1|$CUT -d"|" -f2`
	DELETE_PORT=`$ECHO $1|$CUT -d"|" -f3`	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Greenplum Instance Deletion Parameters" 1
	LOG_MSG "[INFO]:--------------------------------" 1
	LOG_MSG "[INFO]:-Greenplum Master hostname               = $MASTER_HOSTNAME" 1
	LOG_MSG "[INFO]:-Greenplum Master data directory         = $MASTER_DATA_DIRECTORY" 1
	LOG_MSG "[INFO]:-Greenplum Master port                   = $MASTER_PORT" 1
	LOG_MSG "[INFO]:-Greenplum Master standby host           = $STANDBY_HOSTNAME" 1
	if [ $FORCE -eq 1 ];then
		LOG_MSG "[WARN]:-Greenplum Force delete of dump files    = ON" 1
	else
		LOG_MSG "[INFO]:-Greenplum Force delete of dump files    = Off" 1
	fi	
	if [ $STANDBY_COUNT -ne 0 ];then
	LOG_MSG "[INFO]:-Greenplum Master standby data directory = $STANDBY_DATADIR" 1
	LOG_MSG "[INFO]:-Greenplum Master standby port           = $STANDBY_PORT" 1
	fi
	if [ $FAIL_PRI_SEG ];then
	LOG_MSG "[INFO]:-Greenplum RO Master deletion            = On" 1
	fi
	LOG_MSG "[INFO]:-------------------------------" 1
	LOG_MSG "[INFO]:-Segment Instance List" 1
	LOG_MSG "[INFO]:-------------------------------" 1
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		SET_VAR $DELETE_SEG
	LOG_MSG "[INFO]:-Host=$DELETE_HOSTNAME Data directory=$DELETE_DATADIR Port=$DELETE_PORT" 1
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	if [ -z $NOINTERACTIVE ];then
		$ECHO  "Continue with Greenplum instance deletion Yy/Nn>"
		read REPLY
		CHK_REPLY
		$ECHO "FINAL WARNING, you are about to delete the Greenplum instance on Master host $MASTER_HOSTNAME"
		if [ $DUMP_COUNT -ne 0 ] && [ $FORCE -eq 1 ];then
		$ECHO "There are database dump files, these will be DELETED if you continue!"
		fi
		$ECHO "Continue with Greenplum deletion Yy/Nn>"
		read REPLY
		CHK_REPLY
	else
		LOG_MSG "[INFO]:-Non-interactive mode selected" 1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_REPLY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ -z $REPLY ]; then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
		EXIT_STATUS=1
		exit $EXIT_STATUS
	fi
	if [ $REPLY != Y ] && [ $REPLY != y ]; then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
		EXIT_STATUS=1
		exit $EXIT_STATUS
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DELETE_CLUSTER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Closing down instance" 1
	if [ -f $GPSTOP ];then
		$GPSTOP -zaf -d $DELETE_MASTER_DATA_DIRECTORY
	else
		ERROR_EXIT "[FATAL]:-Unable to locate $GPSTOP" 2
	fi
	LOG_MSG "[INFO]:-Deleting segment data directories, please wait..." 1
	BATCH_LIMIT=${#QE_ARRAY[@]}
	PARALLEL_SETUP  $PARALLEL_STATUS_FILE
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
		SET_VAR $DELETE_SEG
		$GPSEGDELETE $$ 2 $DELETE_HOSTNAME $DELETE_DATADIR $INST_COUNT $LOG_FILE &
		PARALLEL_COUNT $BATCH_LIMIT $BATCH_DEFAULT
	done
	COMPLETED_COUNT=`$CAT $PARALLEL_STATUS_FILE|$GREP -c "COMPLETED"`
	KILLED_COUNT=`$CAT $PARALLEL_STATUS_FILE|$GREP -c "KILLED"`
	DEL_OK_COUNT=`$CAT $PARALLEL_STATUS_FILE|$GREP -c "DEL_OK"`
	DEL_FAIL_COUNT=`$CAT $PARALLEL_STATUS_FILE|$GREP -c "DEL_WARN"`
	LOG_MSG "[INFO]:------------------------------------------------" 1
	LOG_MSG "[INFO]:-Parallel segment delete process exit status" 1
	LOG_MSG "[INFO]:------------------------------------------------" 1
	LOG_MSG "[INFO]:-Total processes marked as completed           = $COMPLETED_COUNT" 1
	if [ $KILLED_COUNT -ne 0 ];then
	LOG_MSG "[WARN]:-Total processes marked as killed              = $KILLED_COUNT $WARN_MARK" 1
	else
	LOG_MSG "[INFO]:-Total processes marked as killed              = 0" 1
	fi
	if [ $DEL_FAIL_COUNT -ne 0 ];then
	LOG_MSG "[WARN]:-Total processes returned delete warning       = $DEL_FAIL_COUNT $WARN_MARK" 1
	else
	LOG_MSG "[INFO]:-Total processes returned delete warning       = 0" 1
	fi	
	LOG_MSG "[INFO]:-Total processes returned no delete warning    = $DEL_OK_COUNT" 1
	LOG_MSG "[INFO]:------------------------------------------------" 1
	$RM -f $PARALLEL_STATUS_FILE
	if [ $STANDBY_COUNT -ne 0 ];then
		LOG_MSG "[INFO]:-Processing Master standby host $STANDBY_HOSTNAME" 1
		RUN_COMMAND_REMOTE $STANDBY_HOSTNAME "$RM -Rf $STANDBY_DATADIR"
		RUN_COMMAND_REMOTE $STANDBY_HOSTNAME "$RM -f ${STANDBY_DATADIR}.log"
	fi
	LOG_MSG "[INFO]:-Processing Master host $MASTER_HOSTNAME" 1
	RUN_COMMAND "$RM -Rf $MASTER_DATA_DIRECTORY"
	RUN_COMMAND "$RM -f ${MASTER_DATA_DIRECTORY}.log"
	LOG_MSG "[INFO]:-The Greenplum instance on Master host $MASTER_HOSTNAME, has been deleted" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DELETE_RO_MASTER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PING_HOST $FAIL_PRI_SEG
	if [ $RETVAL -eq 0 ];then
		GET_PG_PID_ACTIVE $MASTER_PORT $FAIL_PRI_SEG
		if [ $PID -ne 0 ];then
			LOG_MSG "[INFO]:-Master read only host process active, calling shutdown" 1
			$TRUSTED_SHELL $FAIL_PRI_SEG "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$GPSTOP -d $MASTER_DATA_DIRECTORY -m"
		fi
		LOG_MSG "[INFO]:-Deleting Master read only directory and log file" 1
		$TRUSTED_SHELL $FAIL_PRI_SEG "if [ -d $MASTER_DATA_DIRECTORY ];then $RM -Rf $MASTER_DATA_DIRECTORY; $RM -f ${MASTER_DATA_DIRECTORY}.log;fi"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ];then
        while getopts ":v'?'fr:d:l:!DB:" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                '?' ) USAGE ;;
                d ) DELETE_MASTER_DATA_DIRECTORY=$OPTARG ;;
		l ) LOG_DIR=$OPTARG ;;
		f ) FORCE=1 ;;
		r ) FAIL_PRI_SEG=$OPTARG ;;
		D ) DEBUG_LEVEL=1 ;;
		B ) BATCH_DEFAULT=$OPTARG ;;
		! ) SCRIPT_USAGE=1
		    USAGE ;;
                * ) UNKNOWN_OPTION ;;
        esac
        done
else
	USAGE
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
		EXIT_STATUS=1
else
	LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
if [ x"" == x"$DELETE_MASTER_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply -d option" 2
fi
CHK_ON_PASSIVE_STANDBY
CHECK_PARAM
OBTAIN_PARAM
DISPLAY_PARAM
DELETE_CLUSTER
if [ $FAIL_PRI_SEG ];then
	DELETE_RO_MASTER
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
