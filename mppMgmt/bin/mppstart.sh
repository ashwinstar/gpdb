#!/bin/bash
#	Filename:-		mppstart.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		MPP Conversion SE Co Internal
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Starts the cluster database.
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#
#******************************************************************************
# Prep Code
# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
WORKDIR=`dirname $0`
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
if [ -f $FUNCTIONS ]; then
		. $FUNCTIONS
else
		echo "[FATAL:]-Cannot source $FUNCTIONS file Script Exits!"
		exit 2
fi
#******************************************************************************
# Location Variables
#******************************************************************************

#******************************************************************************
# Command Variables
#******************************************************************************

#******************************************************************************
# Script Specific Variables
#******************************************************************************
INTERACTIVE=1
VERBOSE=1
WAIT="-w"
MODE=smart
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
QD_NAME=`hostname`
EXIT_STATUS=0
declare -a FAILED_LIST_ARRAY
unset FAILED
unset ADMIN
FAILED_COUNT=0
TAIL_COUNT=0
START_INVALID=0
STANDBY_COUNT=0
FORCE=0
STOP_PROG=$MPPSTOP
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Starts the full MPP database, or Master database only in utility mode"
		$ECHO
		$ECHO "      Usage:"
		$ECHO "      $0 [OPTIONS]"
		$ECHO
		$ECHO "      General options:"
		$ECHO "      -?, display this help message & exit" 
		$ECHO "      -v, display version information & exit"
		$ECHO "      -t, display default parameter settings & exit"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "      -a, don't ask to confirm startup [default:- ask]"
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master database directory> name of alternative Master database to start up [optional]"
		$ECHO
		$ECHO "      Database startup options:"
		$ECHO "      -n, do not wait for startup return code of each segment instance [default:- wait]"
		$ECHO "      -m, start master database only in maintenance mode [default:- full MPP startup]"
		$ECHO "      -z, force startup of segment instance marked as invalid"
		$ECHO "      -y, start master standby server [default:- do not start]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but database operational"
		$ECHO "      2 Fatal error, database not started, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

DISPLAY_PARAM () {
		if [ -z $MASTER_PORT ]; then
				MASTER_PORT="Not set in ENV"
		fi
		$ECHO
		$ECHO "Current Default Startup Parameters"
		$ECHO "----------------------------------"
		$ECHO "MPP QD database port        = $MASTER_PORT      [Override with -p <port number>]"
		$ECHO "MPP QD database directory   = $MASTER_DATA_DIRECTORY [Override with -d <Master directory>]"
		if [ $INTERACTIVE ]; then
				$ECHO "Interactive                 = True            [Override with -a]"
		else
				$ECHO "Interactive                 = False"
		fi
		if [ $VERBOSE ]; then
				$ECHO "Verbose                     = True            [Override with -q]"
		else
				$ECHO "Verbose                     = False"
		fi 
		if [ "$WAIT" == "-w" ]; then
				$ECHO "Wait for startup            = True            [Override with -n]"
		else
				$ECHO "Wait for startup            = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}


CHK_QD_DB_RUNNING () {
		if [  -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
			LOG_MSG "[WARN]:-$PG_PID file exists on QD, checking if recovery startup required" 
			RECOVERY_STARTUP
		fi
}

RECOVERY_STARTUP () {
	LOG_MSG "[INFO]:-Commencing recovery startup checks"
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	GET_PG_PID_ACTIVE $MASTER_PORT
	if [ $PID -ne 0 ];then	
		ERROR_EXIT "[FATAL]:-Master QD process running" 2
	fi
	LOG_MSG "[INFO]:-No QD process, entering recovery startup mode"
	LOG_MSG "[INFO]:-Clearing QD lock files"
	$RM -f /tmp/.s.PGSQL.${MASTER_PORT}.lock /tmp/.s.PGSQL.${MASTER_PORT}
	LOG_MSG "[INFO]:-Clearing QD $PG_PID file"
	$RM -f ${MASTER_DATA_DIRECTORY}/$PG_PID
	START_ADMIN
	LOG_MSG "[INFO]:-Commencing forced cluster shutdown"
	$STOP_PROG -faz
	GET_DETAILS
	PROCESS_STARTUP
	EXIT_STATUS=1
	exit $EXIT_STATUS	
}

GET_DETAILS () {
		#Get the database name
		#Start the QD datase in admin mode
		export PGPORT=$MASTER_PORT
		LOG_MSG "[INFO]:-Starting Master database in admin mode"
		$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT"  start >/dev/null 2>&1
		ERROR_CHK $? "startup Master instance" 2
		LOG_MSG "[INFO]:-Obtaining MPP Master catalog information"
		#Now get tail details
		GET_QE_DETAILS
		QE_COUNT=${#QE_ARRAY[@]}
		if [ x"" != x"$START_SYNC" ];then
			GET_STANDBY_COUNT
			if [ $STANDBY_COUNT -ne 0 ];then
				GET_STANDBY_DETAILS
			fi
		fi
		#Shutdown the QD database
		LOG_MSG "[INFO]:-Shutting down Master database"
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE	 >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master instance" 2
}

START_ADMIN () {
		LOG_MSG "[INFO]:-Starting master database in admin mode"
		export PGPORT=$MASTER_PORT
		env PGOPTIONS="-c mpp_session_role=utility" $PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT " start
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				$TAIL ${MASTER_DATA_DIRECTORY}.log|$TEE -a $LOG_FILE
				ERROR_EXIT "[FATAL]:-Could not start master database in admin mode" 2
		fi
		LOG_MSG "[INFO]:-Successfully started master database in admin mode"
}


PROMPT_CONTINUE () {
		LOG_MSG "[INFO]:-Master parameters:"
		LOG_MSG "--------------------------"
		LOG_MSG "[INFO]:-Database                   = $QD_DBNAME"
		LOG_MSG "[INFO]:-Master Port                = $MASTER_PORT"
		LOG_MSG "[INFO]:-Master directory           = $MASTER_DATA_DIRECTORY"
		if [ x"" != x"$START_SYNC" ] && [ $STANDBY_COUNT -ne 0 ];then
		LOG_MSG "[INFO]:-Master standby start       = On"
		else
		LOG_MSG "[INFO]:-Master standby start       = Off"
		fi
		LOG_MSG "[INFO]:-Segment instances that will be started:"
		LOG_MSG "--------------------------------------------------"
		COUNT=1
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
				QE_PRIMARY=`$ECHO $QNODE|$CUT -d"|" -f5`
				if [ "$QE_VALID" == "t" ]; then
					LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT"
				else
					LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT invalid will not start"
				fi
				((COUNT=$COUNT+1))
		done
		$ECHO
		$ECHO "Continue with MPP database startup yY|nN >"
		read REPLY
		if [ -z $REPLY ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
}	

PROCESS_STARTUP () {
		if [ x"" != x"$START_SYNC" ] && [ $STANDBY_COUNT -ne 0 ];then
			STANDBY_HOSTNAME=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f1`
			STANDBY_DATADIR=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f2`
			STANDBY_PORT=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f3`	
			START_SYNC_PROCESS $STANDBY_HOSTNAME $STANDBY_DATADIR $STANDBY_PORT
		fi
		INDEX=0
		QE_START_COUNT=0
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
				QE_PRIMARY=`$ECHO $QNODE|$CUT -d"|" -f5`
				if [ "$QE_VALID" == "t" ] || [ $START_INVALID -eq 1 ];then
					LOG_MSG "[INFO]:-Starting Segment instance $QE_NAME directory $QE_DIR"
					$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};export PGPATH=${APP_PATH};export PGPORT=${QE_PORT};$PG_CTL $WAIT -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT}\" start" 
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						((FAILED_COUNT=$FAILED_COUNT+1))
						LOG_MSG "[WARN]:-Startup request returned error code $RETVAL"
						LOG_MSG "[INFO]:-Tail'ing Segment instance log file"
						$TRUSTED_SHELL $QE_NAME "$LS $QE_DIR" > /dev/null 2>&1
						if [ $? -ne 0 ]; then
								LOG_MSG "[WARN]:-Segment directory $QE_DIR on $QE_NAME does not exist"
								EXIT_STATUS=1
						else
								$TRUSTED_SHELL $QE_NAME "$TAIL ${QE_DIR}.log"|$TEE -a $LOG_FILE
								LOG_MSG "[WARN]:-Issue with $TRUSTED_SHELL to $QE_NAME for $QE_DIR database"
								EXIT_STATUS=1
						fi
				else
						LOG_MSG "[INFO]:-Successfully started Segment instance $QE_NAME database"
				fi
				#Now check status of postmaster on the tail
				LOG_MSG "[INFO]:-Checking status via pg_ctl"
				RET_TEXT=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};$PG_CTL status -D $QE_DIR"`
				RUNNING=`$ECHO $RET_TEXT|$EGREP -c "not running|neither"`
				if [ $RUNNING -ne 0 ]; then
						LOG_MSG "[WARN]:-Command pg_ctl reports $QE_NAME for directory $QE_DIR port $QE_PORT not running"
						EXIT_STATUS=1
				fi
				if [ $RUNNING -eq 0 ] && [ $RETVAL -ne 0 ]; then
						LOG_MSG "[WARN]:-Could not start instance, BUT pg_ctl reports instance active, check instance log file via mppstate.bsh -e"
						EXIT_STATUS=1
				fi
				if [ $RUNNING -eq 0 ] && [ $RETVAL -eq 0 ]; then
						LOG_MSG "[INFO]:-Command pg_ctl indicates started Segment instance $QE_NAME database"
				fi
				((QE_START_COUNT=$QE_START_COUNT+1))
				((INDEX=$INDEX+1))
				else
						LOG_MSG "[WARN]:-Segment instance $QE_NAME directory $QE_DIR invalid will not start"
						EXIT_STATUS=1
				fi
		done
		LOG_MSG "[INFO]:-Starting Master $QD_NAME directory $MASTER_DATA_DIRECTORY"
		if [ $WAIT == "-W" ]; then
				$SLEEP 2
		fi
		export PGPORT=${MASTER_PORT}
		export PGPATH=${APP_PATH}
		$PG_CTL $WAIT  -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -E -p $MASTER_PORT " start -m $MODE  
		ERROR_CHK $? "startup Master instance" 2
		RET_TEXT=`$PG_CTL status -D $MASTER_DATA_DIRECTORY`
		RUNNING=`$ECHO $RET_TEXT|$EGREP -c "not running|neither"`
		if [ $RUNNING -ne 0 ]; then
			LOG_MSG "[WARN]:-Command pg_ctl reports $QD_NAME for directory $MASTER_DATA_DIRECTORY port $MASTER_PORT not running"
						EXIT_STATUS=1
		else
			LOG_MSG "[INFO]:-Command pg_ctl reports Master $QD_NAME instance active"
		fi
		if [ $FAILED_COUNT -ne 0 ]; then
				LOG_MSG "[WARN]:-Failed to start $FAILED_COUNT of a total of $QE_COUNT Segment instances"
		else
				LOG_MSG "[INFO]:----------------------------------------------------------"
				LOG_MSG "[INFO]:-Successfully started $QE_START_COUNT Segment instances and Master instance"
		fi
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
		while getopts ":vt'?'aqyznml:d:" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						a ) unset INTERACTIVE ;;
						q ) unset VERBOSE ;;
						n ) WAIT="-W" ;; 
						m ) ADMIN=1 ;;
						z ) START_INVALID=1 ;;
						l ) LOG_DIR=$OPTARG ;;
						y ) START_SYNC=1 ;;
						* ) USAGE ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
CHK_ON_PASSIVE_STANDBY
#MPP-308
if [ x"" != x"$MPPHOME" ];then
	APP_PATH=${MPPHOME}/bin
else
	APP_PATH=${BIZHOME}/bin
fi
CHK_QD_DB_RUNNING
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
if [ $ADMIN ]; then
	START_ADMIN
	exit $EXIT_STATUS
fi
GET_DETAILS
if [ $INTERACTIVE ]; then
	PROMPT_CONTINUE
fi
PROCESS_STARTUP
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
