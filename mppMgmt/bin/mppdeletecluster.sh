#!/bin/bash
#	Filename:-		mppdeletecluster.bsh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Deletes an MPP instance.
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
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
FORCE=0
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
        $ECHO "      Deletes a MPP instance, use with caution, script is destructive"
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
	$ECHO
	$ECHO "      Delete options:"
	$ECHO "      -d, <Master database directory> directory to be deleted along with all segment instances [mandatory]"
	$ECHO "      -f, force deletion, ignore any database backup files"
	$ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
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
		ERROR_EXIT "[FATAL]:-Directory $DELETE_MASTER_DATA_DIRECTORY does not exist on this host";fi
	if [ ! -f ${DELETE_MASTER_DATA_DIRECTORY}/postmaster.pid ];then
		ERROR_EXIT "[FATAL]:-MPP instance not running, activate instance to be able to progress instance deletion" 2
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
	LOG_MSG "[INFO]:-Checking for database dump files"
	DB_DUMP_CHK
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DB_DUMP_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Checking Master instance for dump files"
	DUMP_COUNT=`$FIND $MASTER_DATA_DIRECTORY -name 'mpp_dump*' -print|$WC -l`
	if [ $DUMP_COUNT -ne 0 ] && [ $FORCE -eq 0 ];then
		LOG_MSG "[FATAL]:-Located possible database dump file on Master instance host" 
		LOG_MSG "[FATAL]:-In directory $MASTER_DATA_DIRECTORY"
		ERROR_EXIT "[FATAL]:-To override database dump file checking use -f option" 2
	else
		LOG_MSG "[INFO]:-Complete Master instance dump file check"
	fi	
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		SET_VAR $DELETE_SEG
		LOG_MSG "[INFO]:-Checking host=$DELETE_HOSTNAME Data directory=$DELETE_DATADIR"
		DUMP_COUNT=`$TRUSTED_SHELL $DELETE_HOSTNAME "$FIND $DELETE_DATADIR -name 'mpp_dump*' -print|$WC -l"`
		if [ $DUMP_COUNT -ne 0 ] && [ $FORCE -eq 0 ];then
			LOG_MSG "[FATAL]:-Located possible database dump file on $DELETE_HOSTNAME"
			LOG_MSG "[FATAL]:-In directory $DELETE_DATADIR"
			ERROR_EXIT "[FATAL]:-To override database dump file checking use -f option" 2
		else
			LOG_MSG "[INFO]:-Complete $DELETE_HOSTNAME dump file check"
		fi 
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SET_VAR () {
	DELETE_HOSTNAME=`$ECHO $1|$CUT -d"|" -f1`
	DELETE_DATADIR=`$ECHO $1|$CUT -d"|" -f2`
	DELETE_PORT=`$ECHO $1|$CUT -d"|" -f3`	
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP Instance Deletion Parameters"
	LOG_MSG "[INFO]:--------------------------------"
	LOG_MSG "[INFO]:-MPP Master hostname               = $MASTER_HOSTNAME"
	LOG_MSG "[INFO]:-MPP Master data directory         = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-MPP Master port                   = $MASTER_PORT"
	LOG_MSG "[INFO]:-MPP Master standby host           = $STANDBY_HOSTNAME"
	if [ $FORCE -eq 1 ];then
		LOG_MSG "[WARN]:-MPP Force delete of dump files    = ON"
	else
		LOG_MSG "[INFO]:-MPP Force delete of dump files    = Off"
	fi	
	if [ $STANDBY_COUNT -ne 0 ];then
	LOG_MSG "[INFO]:-MPP Master standby data directory = $STANDBY_DATADIR"
	LOG_MSG "[INFO]:-MPP Master standby port           = $STANDBY_PORT"
	fi
	if [ $FAIL_PRI_SEG ];then
	LOG_MSG "[INFO]:-MPP RO Master deletion            = On"
	fi
	LOG_MSG "[INFO]:-------------------------------"
	LOG_MSG "[INFO]:-Segment Instance List"
	LOG_MSG "[INFO]:-------------------------------"
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		SET_VAR $DELETE_SEG
	LOG_MSG "[INFO]:-Host=$DELETE_HOSTNAME Data directory=$DELETE_DATADIR Port=$DELETE_PORT"
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	if [ -z $NOINTERACTIVE ];then
		$ECHO  "Continue with MPP instance deletion Yy/Nn>"
		read REPLY
		CHK_REPLY
		$ECHO "FINAL WARNING, you are about to delete the MPP instance on Master host $MASTER_HOSTNAME"
		if [ $DUMP_COUNT -ne 0 ] && [ $FORCE -eq 1 ];then
		$ECHO "There are database dump files, these will be DELETED if you continue!"
		fi
		$ECHO "Continue with MPP deletion Yy/Nn>"
		read REPLY
		CHK_REPLY
	else
		LOG_MSG "[INFO]:-Non-interactive mode selected"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_REPLY () {
	if [ -z $REPLY ]; then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!"
		EXIT_STATUS=1
		exit $EXIT_STATUS
	fi
	if [ $REPLY != Y ] && [ $REPLY != y ]; then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!"
		EXIT_STATUS=1
		exit $EXIT_STATUS
	fi
}

DELETE_CLUSTER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Closing down instance"
	if [ -f $MPPSTOP ];then
		$MPPSTOP -zqaf -d $DELETE_MASTER_DATA_DIRECTORY
	else
		ERROR_EXIT "[FATAL]:-Unable to locate $MPPSTOP" 2
	fi
	LOG_MSG "[INFO]:-Deleting segment data directories"
	for DELETE_SEG in "${QE_ARRAY[@]}"
	do
		SET_VAR $DELETE_SEG
		LOG_MSG "[INFO]:-Processing host=$DELETE_HOSTNAME Data directory=$DELETE_DATADIR"
		RUN_COMMAND_REMOTE $DELETE_HOSTNAME "$RM -Rf $DELETE_DATADIR"
		RUN_COMMAND_REMOTE $DELETE_HOSTNAME "$RM -f ${DELETE_DATADIR}.log"
	done
	if [ $STANDBY_COUNT -ne 0 ];then
		LOG_MSG "[INFO]:-Processing Master standby host $STANDBY_HOSTNAME"
		STOP_SYNC_PROCESS $STANDBY_HOSTNAME $STANDBY_DATADIR
		RUN_COMMAND_REMOTE $STANDBY_HOSTNAME "$RM -Rf $STANDBY_DATADIR"
		RUN_COMMAND_REMOTE $STANDBY_HOSTNAME "$RM -f ${STANDBY_DATADIR}.log"
	fi
	LOG_MSG "[INFO]:-Processing Master host $MASTER_HOSTNAME"
	RUN_COMMAND "$RM -Rf $MASTER_DATA_DIRECTORY"
	RUN_COMMAND "$RM -f ${MASTER_DATA_DIRECTORY}.log"
	LOG_MSG "[INFO]:-The MPP instance on Master host $MASTER_HOSTNAME, has been deleted"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DELETE_RO_MASTER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PING_HOST $FAIL_PRI_SEG
	if [ $RETVAL -eq 0 ];then
		GET_PG_PID_ACTIVE $MASTER_PORT $FAIL_PRI_SEG
		if [ $PID -ne 0 ];then
			LOG_MSG "[INFO]:-Master read only host process active, calling shutdown"
			$TRUSTED_SHELL $FAIL_PRI_SEG "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$MPPSTOP -d $MASTER_DATA_DIRECTORY -m"
		fi
		LOG_MSG "[INFO]:-Deleting Master read only directory and log file"
		$TRUSTED_SHELL $FAIL_PRI_SEG "if [ -d $MASTER_DATA_DIRECTORY ];then $RM -Rf $MASTER_DATA_DIRECTORY; $RM -f ${MASTER_DATA_DIRECTORY}.log;fi"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ];then
        while getopts ":v'?'fr:d:l:" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                '?' ) USAGE ;;
                d ) DELETE_MASTER_DATA_DIRECTORY=$OPTARG ;;
		l ) LOG_DIR=$OPTARG ;;
		f ) FORCE=1 ;;
		r ) FAIL_PRI_SEG=$OPTARG ;;
                * ) UNKNOWN_OPTION ;;
        esac
        done
else
	USAGE
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
		EXIT_STATUS=1
else
	LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
if [ x"" == x"$DELETE_MASTER_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply -d option"
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
